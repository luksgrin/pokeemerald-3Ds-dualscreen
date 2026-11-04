/*
 * Local wireless play: the radio of the emulated Wireless Adapter
 * (3ds_rfu_adapter.c) is the 3DS's local wireless (the UDS service), so the
 * game's wireless features find other systems nearby. See include/3ds_local.h.
 *
 * Builds. Only identical builds link: the game's link code exchanges its
 * structures raw, and two builds can disagree on them. Every build carries a
 * SHA-256 of its executable and data (engine/build_id.bin, written by
 * scripts/gen_build_id.py). A session's beacon shows it and its passphrase
 * holds it, so another build can neither find nor join the session; a player
 * whose counter has only sessions of other builds nearby is told so, rather
 * than left hosting a session nobody nearby can join.
 *
 * Sessions. Wireless Adapters see every adapter in range; UDS has networks,
 * each with a host and up to 15 others. So players meet in a network, a
 * session: at a link counter the game looks for a session of that counter
 * nearby (its beacon's application data says which, and the protocol
 * version) and joins it, or hosts one if there is none. Everyone in a
 * session then sees everyone else, as adapters in range would.
 *
 * Messages. Every system in a session sends straight to every other (UDS
 * forwards between clients itself). UDS frames can be lost, and the adapter
 * expects what it sends to arrive, in order, as the relay of the online
 * version guaranteed; so each pair of systems keeps a stream of messages:
 * numbered, acknowledged, and sent again until they are (go-back-N). A
 * frame's messages to a system go in one UDS frame.
 *
 * The UDS calls are made by a thread of their own: looking for sessions takes
 * a second or two, and the game must not stop meanwhile. The game's side (the
 * adapter, the counters' script) only queues messages, reads events and the
 * table of players, under a lock.
 *
 * The thread sleeps until there is something to do: a frame came in (the
 * bind's event), the game queued a message (sL.wake), or WAIT_MS passed
 * (messages to send again, players coming and going). It used to look every
 * 2 ms, 500 requests a second to the wireless service even with nothing to
 * read; on an Old 3DS that service shares the system core with the GPU's,
 * and a link battle dropped to 30 fps.
 *
 * The wire. LOCAL_LINK_UDP compiles in a second radio: the same message
 * streams over a UDP socket, whose session a bridge on the network hosts
 * (tools/frlg_bridge, and later a small board by the Switch) — the leg
 * toward the Switch's GBA games; its beacons, not radio scans, are what a
 * search finds there. LOCAL_LINK_NO_UDS takes the air back out again (a
 * test build, no play with unmodified consoles). With neither taken out —
 * the usual build — both radios are in: a session is hosted on the air, a
 * search hears the air and then the wire, and a lone host moves to a
 * bridge it hears.
 */
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "3ds_local.h"
#include "3ds_log.h"
#include "3ds_platform.h"
/* The counters' kinds and states are the game's own (the wireless patch
 * makes them); the host tests have theirs until the tree is bootstrapped. */
#if defined(__has_include) && __has_include("../../include/constants/local_link.h")
#include "../../include/constants/local_link.h"
#else
#include "../tests/local_host/constants_stub.h"
#endif
#ifdef LOCAL_LINK_UDP
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* UDS: our network's id and passphrase, and our data channel. */
#define COMM_ID 0x45E3D501u
#define PASSPHRASE "EM3DS local"  /* then 16 bytes of the build id in hex: 45 bytes in all */
#define BUILD_ID_PATH "engine/build_id.bin"
#define BUILD_ID_SIZE 32
#define DATA_CHANNEL 1
#define SHAREDMEM_SIZE 0x3000
#define MAX_NODES 5 /* a session: the host and four others, as the Union Room */
/* Sessions of other versions of this protocol are not joined. */
#define PROTOCOL_VERSION 1
static const char kMagic[4] = {'E', 'M', '3', 'L'};

/* How long to look for a session before hosting one: a second and a half,
 * plus up to one more at random. Two players who talk to their attendants
 * together can still both end up hosting, each alone: a host with nobody in
 * its session looks again every couple of seconds, and moves to another
 * session of its counter if there is one (see MergeAlone). */
#define SEARCH_MS 1500
#define SEARCH_JITTER_MS 1000
#define MERGE_MS 1500
#define MERGE_JITTER_MS 1500
/* Only so long after it started hosting: the case is two players starting
 * together. A player who comes later finds the host itself. On hardware a
 * host can't look while hosting, so each look closes and reopens it. */
#define MERGE_WINDOW_MS 20000

/* UDS connection status values (udsConnectionStatus.status). */
#define STATUS_IDLE 3
#define STATUS_HOST 6
#define STATUS_CLIENT 9
#define STATUS_SPECTATOR 10

#define NODE_SLOTS 17     /* UDS node ids are 1-16 */
#define OUT_QUEUE 128     /* messages to one system not yet acknowledged */
#define MSG_MAX 168       /* the largest message: an advert, or 160 bytes of data */
#define RESEND_MS 100     /* unacknowledged this long: sent again */
#define ACK_DELAY_MS 30   /* an ack alone waits this long for a message to ride on */
#define EVENT_QUEUE 512
#define FRAME_MAX 1400    /* a UDS data frame takes 0x5C6 bytes */
#define THREAD_STACK 0x4000
#define WAIT_MS 20        /* the thread looks this often while a message waits for its ack */
#define IDLE_MS 100       /* and this often otherwise (and reads, should no event come) */
#define STATS_MS 10000    /* its counts go to the log this often */
/* A system that left a message of ours unacknowledged this long after it
 * was first sent, and sent nothing meanwhile, is gone: one switched off or
 * out of range may stay in the network's list for a long time (in Azahar, a
 * closed one for good), and the link would wait for it. */
#define SILENT_MS 6000

enum
{
    MSG_DATA = 1,   /* the adapter's data, for LOCAL_EV_RECV */
    MSG_ADVERT = 2, /* u8 hosting, u8 nextSlot, u8 hasAdvert, advert */
};

/* A UDS frame: this header, then messages (u16 seq, u8 type, u8 0, u16 len,
 * data), then zeros up to a multiple of 4 bytes. Azahar loses frames of some
 * sizes, and later ones behind them; padded, they all arrive. */
struct FrameHeader
{
    uint8_t magic; /* 0xE3 */
    uint8_t version;
    uint16_t ack;  /* the next message we expect from the receiver */
    uint16_t bodyLen;
    uint16_t reserved;
};
#define FRAME_MAGIC 0xE3
#define MSG_HEADER 6

/* What a session's beacon tells systems looking for one. */
struct AppData
{
    char magic[4];
    uint8_t version;
    uint8_t kind;
    uint8_t reserved[2];
    uint32_t token; /* the host's, at random: which of two lone hosts moves */
    uint8_t build[BUILD_ID_SIZE]; /* the host's build (engine/build_id.bin) */
};

/* Ours, and the passphrase made with it. */
static uint8_t sBuildId[BUILD_ID_SIZE];
static char sPassphrase[sizeof(PASSPHRASE) + 2 * 16 + 2];

#ifdef LOCAL_LINK_UDP
/* The bridge leg (tools/frlg_bridge): the same message streams, over a UDP
 * socket instead of UDS. The session's host is not this system's wireless
 * but a program on the network — the bridge — which sits at the counter's
 * other side. It broadcasts a beacon every UDP_BEACON_MS: the AppData of a
 * UDS beacon, before it a sender's id, flags, and how many are in. A system
 * looking for a session hears the beacons on the same socket and joins the
 * one it likes; data goes straight to its recipient, every datagram naming
 * its sender. A beacon is told from a frame by the AppData magic that
 * follows the name. A bridge is a build of its own: a beacon flagged as one
 * is joined whatever build it names. A session of two: the bridge is 1, we
 * are 2. */
#define UDP_PORT 58325
#define UDP_BEACON_MS 500
#define UDP_BEACON_STALE_MS 2000
#define UDP_FLAG_BRIDGE 1
#define UDP_CLIENT_ID 2

struct UdpBeacon
{
    uint16_t senderId;
    uint16_t flags;
    uint8_t players;   /* in the session now, the host counts itself */
    uint8_t maxPlayers;
    uint16_t reserved;
    struct AppData app; /* from here: what a UDS beacon's data says */
};

/* The two sizes are told here, for the bridge lays the same bytes out. */
_Static_assert(sizeof(struct AppData) == 44, "AppData is 44 bytes on the wire");
_Static_assert(sizeof(struct UdpBeacon) == 52, "UdpBeacon is 52 bytes on the wire");

/* socInit's buffer, and what the socket reads: a named frame at most. */
static uint32_t sSocBuffer[0x40000] __attribute__((aligned(0x1000)));
static uint8_t sUdpBuf[2 + FRAME_MAX + 8];
#endif

enum
{
    SESSION_NO,    /* not one of ours, or another counter's */
    SESSION_YES,   /* this counter's, this build, room for us */
    SESSION_OTHER, /* this counter's, another build (or protocol) */
};

/* Which leg a session rides: the air (the system's local wireless, for
 * consoles nearby), or the wire (a UDP socket, whose session a bridge on
 * the network hosts — the way to the Switch's GBA games). */
enum
{
    BACKEND_AIR,
    BACKEND_WIRE,
};

struct OutMessage
{
    uint16_t seq;
    uint8_t type;
    uint16_t len;
    uint32_t sentMs; /* 0: not sent yet */
    uint32_t firstMs; /* when it was first sent */
    uint8_t data[MSG_MAX];
};

struct Node
{
    bool present;
    struct LocalPeer info;
    /* Messages to it: the oldest unacknowledged first. */
    struct OutMessage out[OUT_QUEUE];
    uint16_t outHead, outCount;
    uint16_t nextSeq;
    bool overflowed;
    uint32_t heardMs; /* its last frame, or when it joined */
    bool heard;       /* a frame of its own has come */
    /* Messages from it. */
    uint16_t expect;
    bool ackDue;
    uint32_t ackDueMs; /* since when */
};

static struct
{
    LightLock lock;
    Thread thread;
    bool ready;  /* LocalLink_Init done */
    volatile bool quit;
#ifndef LOCAL_LINK_NO_UDS
    bool udsReady;
#endif

    /* Asked for by the game, done by the thread. */
    bool startAsked, leaveAsked;
    volatile bool woke; /* the system woke from sleep: the service starts again */
    uint8_t askedKind;

    uint8_t state; /* LOCAL_SESSION_* */
    uint8_t kind;
    bool inNetwork; /* the thread's: hosting or joined a network */
    bool hosting;
    uint32_t token;       /* ours, in our session's beacon */
    uint32_t nextMergeMs; /* a lone host looks for other sessions then */
    uint32_t mergeUntilMs; /* and not after this */
    uint16_t myId;
    uint32_t generation;
    uint8_t backend; /* BACKEND_*: which leg the session rides */
#ifdef LOCAL_LINK_UDP
    /* The wire's bookkeeping: where the session's host is (a client), where
     * each player is (a host), and the last beacon seen. */
    int sock;
    bool socReady;   /* the system's sockets are up: once; it survives sleep */
    bool radioReady; /* our socket is bound */
    struct sockaddr_in hostAddr;
    bool haveHost;
    struct sockaddr_in peerAddr[NODE_SLOTS];
    uint32_t nextBeaconMs;
    struct UdpBeacon beacon;
    struct sockaddr_in beaconAddr;
    uint32_t beaconMs;
#endif
#ifndef LOCAL_LINK_NO_UDS
    udsBindContext bind;
#endif

    struct Node nodes[NODE_SLOTS];
    uint16_t nodeMask; /* nodes in the session, us included */

    /* Our adapter's broadcast. */
    uint8_t advertHosting, advertNextSlot, hasAdvert;
    uint8_t advert[LOCAL_ADVERT_SIZE];

    struct LocalEvent events[EVENT_QUEUE];
    uint16_t eventHead, eventCount;

    Handle wake; /* the game queued something */
    /* What the thread did since the last log line. */
    struct { uint32_t wakes, pulls, framesIn, framesOut, resent, sinceMs; } stats;
} sL;

#ifndef LOCAL_LINK_NO_UDS
static uint8_t sFrame[UDS_DATAFRAME_MAXSIZE]; /* a UDS frame takes 0x5C6 bytes */
#else
static uint8_t sFrame[FRAME_MAX + 12]; /* the header, the messages, and padding */
#endif

uint32_t LocalLink_Milliseconds(void)
{
    uint32_t ms = (uint32_t)(svcGetSystemTick() / CPU_TICKS_PER_MSEC);
    return ms ? ms : 1;
}

static bool SeqBefore(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) < 0;
}

/* ---- Events for the game (lock held) ------------------------------------ */

static void PushEvent(uint8_t type, uint16_t id, const uint8_t *data, uint32_t len)
{
    struct LocalEvent *ev;

    if (sL.eventCount == EVENT_QUEUE)
    {
        CtrLog_Write(CTR_LOG_ERROR, "local: event queue full, dropping");
        return;
    }
    ev = &sL.events[(sL.eventHead + sL.eventCount++) % EVENT_QUEUE];
    ev->type = type;
    ev->id = id;
    if (len > sizeof(ev->data))
        len = sizeof(ev->data);
    ev->len = (uint16_t)len;
    if (len)
        memcpy(ev->data, data, len);
}

/* ---- Message streams (lock held) ---------------------------------------- */

static void QueueMessage(uint16_t id, uint8_t type, const void *data, uint32_t len)
{
    struct Node *n;
    struct OutMessage *m;

    if (id == 0 || id >= NODE_SLOTS || !sL.nodes[id].present || id == sL.myId)
        return;
    n = &sL.nodes[id];
    if (len > MSG_MAX)
        len = MSG_MAX;
    if (n->outCount == OUT_QUEUE)
    {
        /* The other system stopped answering; UDS will drop it soon. */
        if (!n->overflowed)
            CtrLog_Write(CTR_LOG_ERROR, "local: queue to %u full", id);
        n->overflowed = true;
        return;
    }
    m = &n->out[(n->outHead + n->outCount++) % OUT_QUEUE];
    m->seq = n->nextSeq++;
    m->type = type;
    m->len = (uint16_t)len;
    m->sentMs = 0;
    memcpy(m->data, data, len);
}

static void QueueAdvert(uint16_t id)
{
    uint8_t msg[3 + LOCAL_ADVERT_SIZE];

    msg[0] = sL.advertHosting;
    msg[1] = sL.advertNextSlot;
    msg[2] = sL.hasAdvert;
    memcpy(msg + 3, sL.advert, LOCAL_ADVERT_SIZE);
    QueueMessage(id, MSG_ADVERT, msg, sizeof(msg));
}

static void NodeJoined(uint16_t id)
{
    struct Node *n = &sL.nodes[id];

    memset(n, 0, sizeof(*n));
    n->present = true;
    n->heardMs = LocalLink_Milliseconds();
    n->info.valid = 1;
    n->info.id = id;
    if (id != sL.myId)
    {
        CtrLog_Write(CTR_LOG_GAME, "local: player %u joined", id);
        QueueAdvert(id);
    }
}

static void NodeLeft(uint16_t id)
{
    if (!sL.nodes[id].present)
        return;
    if (id != sL.myId)
    {
        CtrLog_Write(CTR_LOG_GAME, "local: player %u left", id);
        PushEvent(LOCAL_EV_LEFT, id, NULL, 0);
    }
    memset(&sL.nodes[id], 0, sizeof(sL.nodes[id]));
}

static void HandleMessage(uint16_t from, uint8_t type, const uint8_t *data, uint16_t len)
{
    struct Node *n = &sL.nodes[from];

    if (type == MSG_DATA)
    {
        PushEvent(LOCAL_EV_RECV, from, data, len);
    }
    else if (type == MSG_ADVERT && len >= 3 + LOCAL_ADVERT_SIZE)
    {
        n->info.hosting = data[0];
        n->info.nextSlot = data[1];
        n->info.hasAdvert = data[2];
        memcpy(n->info.advert, data + 3, LOCAL_ADVERT_SIZE);
        if (n->info.hosting)
            n->info.lastHostingMs = LocalLink_Milliseconds();
    }
}

static void HandleFrame(uint16_t from, const uint8_t *frame, size_t size)
{
    const struct FrameHeader *h = (const struct FrameHeader *)frame;
    struct Node *n;
    uint32_t pos, end;

    if (from == 0 || from >= NODE_SLOTS || size < sizeof(*h) || h->magic != FRAME_MAGIC
     || h->version != PROTOCOL_VERSION || sizeof(*h) + h->bodyLen > size)
        return;
    n = &sL.nodes[from];
    if (!n->present)
        NodeJoined(from); /* its frame came before the status said so */
    n->heardMs = LocalLink_Milliseconds();
    n->heard = true;

    /* What it has of ours. */
    while (n->outCount && SeqBefore(n->out[n->outHead].seq, h->ack))
    {
        n->outHead = (n->outHead + 1) % OUT_QUEUE;
        n->outCount--;
        n->overflowed = false;
    }

    pos = sizeof(*h);
    end = sizeof(*h) + h->bodyLen;
    while (pos + MSG_HEADER <= end)
    {
        uint16_t seq = frame[pos] | (frame[pos + 1] << 8);
        uint8_t type = frame[pos + 2];
        uint16_t len = frame[pos + 4] | (frame[pos + 5] << 8);

        pos += MSG_HEADER;
        if (pos + len > end)
            break;
        if (!n->ackDue)
            n->ackDueMs = LocalLink_Milliseconds();
        n->ackDue = true;
        /* No room for it in the game's events: not acknowledged, so it comes
         * again (dropped and acknowledged, it was lost for good). */
        if (seq == n->expect && type == MSG_DATA && sL.eventCount == EVENT_QUEUE)
        {
            static unsigned told;
            if (told++ < 8)
                CtrLog_Write(CTR_LOG_ERROR, "local: event queue full, message from %u waits", from);
            break;
        }
        if (seq == n->expect)
        {
            n->expect++;
            HandleMessage(from, type, frame + pos, len);
        }
        /* Older: we had it, our ack was lost. Newer: one before it was lost,
         * and it comes again with that one. */
        pos += len;
    }
}

/* ---- The bridge leg: the socket ------------------------------------------ */

#ifdef LOCAL_LINK_UDP

/* A datagram from the socket, having waited at most waitMs for one: its
 * size, 0 (none came, or a slip), or -1 (no socket). */
static int UdpPull(struct sockaddr_in *src, void *buf, uint32_t size, int waitMs)
{
    struct sockaddr_in anywhere;
    struct timeval tv = { waitMs / 1000, (waitMs % 1000) * 1000 };
    fd_set set;
    socklen_t len;
    int got;

    if (sL.sock < 0)
        return -1;
    if (src == NULL)
        src = &anywhere;
    len = sizeof(*src);
    if (waitMs)
    {
        FD_ZERO(&set);
        FD_SET(sL.sock, &set);
        if (select(sL.sock + 1, &set, NULL, NULL, &tv) <= 0)
            return 0;
    }
    got = (int)recvfrom(sL.sock, buf, size, 0, (struct sockaddr *)src, &len);
    return got < 0 ? 0 : got;
}

/* One datagram to one player, its sender's name in front: the receiver
 * reads the name, and the frame behind it. A client sends everything to the
 * host it joined; a host sends to where a player's datagrams came from. */
static int UdpSendTo(uint16_t id, const void *buf, uint32_t len)
{
    static uint8_t wire[2 + FRAME_MAX + 12];
    const struct sockaddr_in *to = NULL;

    if (!sL.hosting)
        to = sL.haveHost ? &sL.hostAddr : NULL;
    else if (id < NODE_SLOTS)
        to = sL.peerAddr[id].sin_port ? &sL.peerAddr[id] : NULL;
    if (to == NULL || sL.sock < 0 || len > sizeof(wire) - 2)
        return -1;
    /* sendto copies the bytes before it returns: the scratch is ours again
     * at once. Only the local thread sends. */
    wire[0] = sL.myId & 0xFF;
    wire[1] = sL.myId >> 8;
    memcpy(wire + 2, buf, len);
    return (int)sendto(sL.sock, wire, len + 2, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* The beacon a host sends: what a UDS beacon's data says, with a name,
 * flags and the count of players in front of it. */
static void UdpBeaconSend(void)
{
    struct UdpBeacon b;
    struct sockaddr_in to;

    memset(&b, 0, sizeof(b));
    b.senderId = 1;
    b.players = 0;
    b.maxPlayers = MAX_NODES;
    for (uint16_t id = 1; id < NODE_SLOTS; id++)
        b.players += sL.nodes[id].present ? 1 : 0;
    b.app.version = PROTOCOL_VERSION;
    b.app.kind = sL.kind;
    b.app.token = sL.token;
    memcpy(b.app.magic, kMagic, sizeof(kMagic));
    memcpy(b.app.build, sBuildId, BUILD_ID_SIZE);
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(UDP_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(sL.sock, &b, sizeof(b), 0, (struct sockaddr *)&to, sizeof(to));
}

/* Is the datagram a beacon, and which: laid out as sent. */
static bool UdpBeaconAt(const uint8_t *buf, uint32_t len, struct UdpBeacon *b)
{
    if (len < sizeof(*b) || memcmp(buf + 8, kMagic, sizeof(kMagic)) != 0)
        return false;
    memcpy(b, buf, sizeof(*b));
    return true;
}

/* A lone host keeps the beacon it saw, for its next look around. */
static void UdpBeaconSeen(const struct sockaddr_in *src, const struct UdpBeacon *b)
{
    if (!sL.hosting || sL.myId == 0 || (sL.nodeMask & ~(1u << (sL.myId - 1))))
        return;
    sL.beacon = *b;
    sL.beaconAddr = *src;
    sL.beaconMs = LocalLink_Milliseconds();
}

/* The thread's pass over the socket: a host's beacon, when it is due. */
static void UdpUpdate(uint32_t now)
{
    if (!sL.inNetwork || !sL.hosting || sL.sock < 0)
        return;
    if (sL.nextBeaconMs && (int32_t)(now - sL.nextBeaconMs) < 0)
        return;
    sL.nextBeaconMs = now + UDP_BEACON_MS;
    UdpBeaconSend();
}

#ifndef LOCAL_LINK_NO_UDS
/* A host on the air hears the wire too: what its socket has gathered (a
 * bridge's beacons, mostly), for MergeAlone's look. Nothing waits: what is
 * not a beacon is dropped, nothing here is for a session of ours. */
static void UdpHearBeacons(void)
{
    for (int i = 0; i < 16; i++)
    {
        struct sockaddr_in src;
        struct UdpBeacon beacon;
        int got = UdpPull(&src, sUdpBuf, sizeof(sUdpBuf), 0);

        if (got <= 0)
            break;
        if (UdpBeaconAt(sUdpBuf, (uint32_t)got, &beacon))
            UdpBeaconSeen(&src, &beacon);
    }
}
#endif
#endif /* LOCAL_LINK_UDP */

/* ---- The thread's side of UDS ------------------------------------------- */

static void SendFrame(uint16_t to, uint32_t bodyLen)
{
    struct FrameHeader *h = (struct FrameHeader *)sFrame;
    uint32_t size = (sizeof(*h) + bodyLen + 3) & ~3u;

    h->magic = FRAME_MAGIC;
    h->version = PROTOCOL_VERSION;
    h->ack = sL.nodes[to].expect;
    h->bodyLen = (uint16_t)bodyLen;
    h->reserved = 0;
    memset(sFrame + sizeof(*h) + bodyLen, 0, size - sizeof(*h) - bodyLen);
    /* The lock is not held over the call to the service: the game takes it
     * several times a frame. The frame is ours (only this thread uses sFrame),
     * and meanwhile the game can only add messages behind the ones in it. */
    LightLock_Unlock(&sL.lock);
#ifdef LOCAL_LINK_UDP
    if (sL.backend == BACKEND_WIRE)
    {
        int sent = UdpSendTo(to, sFrame, size);

        LightLock_Lock(&sL.lock);
        sL.stats.framesOut++;
        if (sent < 0)
        {
            static unsigned told;

            if (told++ < 8)
                CtrLog_Write(CTR_LOG_ERROR, "local: send to %u failed (errno %d)", to, errno);
        }
        return;
    }
#endif
#ifndef LOCAL_LINK_NO_UDS
    {
        Result ret = udsSendTo(to, DATA_CHANNEL, UDS_SENDFLAG_Default, sFrame, size);

        LightLock_Lock(&sL.lock);
        sL.stats.framesOut++;
        /* UDS_CHECK_SENDTO_FATALERROR, which -Wsign-compare rejects: that one
         * error only means the frame wasn't sent. */
        if (R_FAILED(ret) && (u32)ret != 0xC86113F0u)
            CtrLog_Write(CTR_LOG_ERROR, "local: send to %u failed (%08lx)", to, (unsigned long)ret);
    }
#else
    LightLock_Lock(&sL.lock); /* the wire-only build never gets here */
#endif
}

/* What's due to one system: messages not sent yet, and when the oldest has
 * waited too long, everything from it again. An ack alone if nothing else
 * goes for a while: in a link both sides send every frame, and the ack rides
 * on that (one UDS frame a game frame each way, not two). */
static void FlushNode(uint16_t id, uint32_t now)
{
    struct Node *n = &sL.nodes[id];
    bool resend = n->outCount && n->out[n->outHead].sentMs && now - n->out[n->outHead].sentMs >= RESEND_MS;
    uint16_t i = 0;
    int frames = 0;

    while (frames < 4)
    {
        uint32_t body = 0;
        uint8_t *p = sFrame + sizeof(struct FrameHeader);

        for (; i < n->outCount; i++)
        {
            struct OutMessage *m = &n->out[(n->outHead + i) % OUT_QUEUE];

            if (m->sentMs && !resend)
                continue;
            /* Room first: a message left for the next frame is not sent yet. */
            if (body + MSG_HEADER + m->len > FRAME_MAX)
                break;
            if (m->sentMs)
                sL.stats.resent++;
            else
                m->firstMs = now;
            p[0] = m->seq & 0xFF;
            p[1] = m->seq >> 8;
            p[2] = m->type;
            p[3] = 0;
            p[4] = m->len & 0xFF;
            p[5] = m->len >> 8;
            memcpy(p + MSG_HEADER, m->data, m->len);
            p += MSG_HEADER + m->len;
            body += MSG_HEADER + m->len;
            m->sentMs = now;
        }
        if (body == 0 && !(n->ackDue && now - n->ackDueMs >= ACK_DELAY_MS))
            break;
        SendFrame(id, body);
        n->ackDue = false;
        frames++;
        if (i >= n->outCount)
            break;
    }
}

static void ReadFrames(void)
{
#ifndef LOCAL_LINK_NO_UDS
    if (sL.backend == BACKEND_AIR)
    {
        /* Cleared before reading: a frame that comes in meanwhile signals it
         * again, so the thread wakes for it. */
        if (sL.bind.event)
            svcClearEvent(sL.bind.event);
        for (int i = 0; i < 64; i++)
        {
            size_t got = 0;
            u16 from = 0;
            Result ret = udsPullPacket(&sL.bind, sFrame, sizeof(sFrame), &got, &from);

            sL.stats.pulls++;

            if (R_FAILED(ret))
            {
                static unsigned told;
                if (told++ < 8)
                    CtrLog_Write(CTR_LOG_ERROR, "local: receive failed (%08lx)", (unsigned long)ret);
                break;
            }
            if (got == 0)
                break;
            sL.stats.framesIn++;
            LightLock_Lock(&sL.lock);
            HandleFrame(from, sFrame, got);
            LightLock_Unlock(&sL.lock);
        }
        return;
    }
#endif
#ifdef LOCAL_LINK_UDP
    for (int i = 0; i < 64; i++)
    {
        struct sockaddr_in src;
        struct UdpBeacon beacon;
        uint16_t id;
        int got = UdpPull(&src, sUdpBuf, sizeof(sUdpBuf), 0);

        if (got <= 0)
            break; /* nothing more in the socket now */
        if (got < 3)
            continue;
        if (UdpBeaconAt(sUdpBuf, (uint32_t)got, &beacon))
        {
            UdpBeaconSeen(&src, &beacon);
            continue;
        }
        id = sUdpBuf[0] | (sUdpBuf[1] << 8);
        if (id == 0 || id >= NODE_SLOTS || id == sL.myId)
            continue;
        /* A client hears its host only. A host hears a player it does not
         * know too, whose first datagram is then where it is: but a session
         * of two is what the bridge leg is for, and one with a player hears
         * no other. */
        if (!sL.hosting
         && (!sL.haveHost || memcmp(&src.sin_addr, &sL.hostAddr.sin_addr, sizeof(src.sin_addr)) != 0
          || src.sin_port != sL.hostAddr.sin_port))
            continue;
        if (sL.hosting && !sL.nodes[id].present && (sL.nodeMask & ~(1u << (sL.myId - 1))))
            continue;
        sL.stats.framesIn++;
        LightLock_Lock(&sL.lock);
        if (sL.hosting && !sL.nodes[id].present && sL.peerAddr[id].sin_port == 0)
            sL.peerAddr[id] = src;
        HandleFrame(id, sUdpBuf + 2, (size_t)got - 2);
        if (sL.nodes[id].present && !(sL.nodeMask & (1u << (id - 1))))
            sL.nodeMask |= 1u << (id - 1);
        LightLock_Unlock(&sL.lock);
    }
#endif
}

static void EndSession(uint8_t state)
{
    LightLock_Lock(&sL.lock);
    for (uint16_t id = 1; id < NODE_SLOTS; id++)
        NodeLeft(id);
    sL.nodeMask = 0;
    sL.myId = 0;
    sL.generation++;
    sL.state = state;
    LightLock_Unlock(&sL.lock);
}

static void LeaveNetwork(bool quiet)
{
    if (!sL.inNetwork)
        return;
    sL.inNetwork = false;
    if (sL.backend == BACKEND_WIRE)
    {
#ifdef LOCAL_LINK_UDP
        /* Nothing to tell the network: it is the bookkeeping that is left. */
        sL.haveHost = false;
        memset(sL.peerAddr, 0, sizeof(sL.peerAddr));
        memset(&sL.beacon, 0, sizeof(sL.beacon));
        memset(&sL.beaconAddr, 0, sizeof(sL.beaconAddr));
        sL.beaconMs = 0;
        sL.nextBeaconMs = 0;
#endif
    }
#ifndef LOCAL_LINK_NO_UDS
    else
    {
        Result left = sL.hosting ? udsDestroyNetwork() : udsDisconnectNetwork();
        Result unbound = udsUnbind(&sL.bind);

        if (R_FAILED(left) || R_FAILED(unbound))
            CtrLog_Write(CTR_LOG_ERROR, "local: leaving: %08lx, unbinding: %08lx", (unsigned long)left, (unsigned long)unbound);
    }
#endif
    sL.hosting = false;
    if (!quiet)
        CtrLog_Write(CTR_LOG_GAME, "local: left the session");
}

#ifndef LOCAL_LINK_NO_UDS
/* Who's in the network now: players joining and leaving. */
static void UpdateNodes(void)
{
    udsConnectionStatus st;
    uint16_t mask, changed;

    if (!udsWaitConnectionStatusEvent(false, false))
        return;
    if (R_FAILED(udsGetConnectionStatus(&st)))
        return;
    /* Not in a network any more: the host went, or we were sent away. */
    if (st.status != 6 && st.status != 9 && st.status != 10)
    {
        CtrLog_Write(CTR_LOG_GAME, "local: the session ended (status %lu)", (unsigned long)st.status);
        LeaveNetwork(false);
        EndSession(LOCAL_SESSION_NONE);
        return;
    }
    LightLock_Lock(&sL.lock);
    sL.myId = st.cur_NetworkNodeID;
    mask = st.node_bitmask;
    changed = mask ^ sL.nodeMask;
    for (uint16_t id = 1; id < NODE_SLOTS; id++)
    {
        if (!(changed & (1u << (id - 1))))
            continue;
        if (mask & (1u << (id - 1)))
        {
            if (!sL.nodes[id].present)
                NodeJoined(id);
        }
        else
        {
            NodeLeft(id);
        }
    }
    sL.nodeMask = mask;
    LightLock_Unlock(&sL.lock);
}
#endif /* not LOCAL_LINK_NO_UDS */

#ifdef LOCAL_LINK_UDP
/* The wire: the system's sockets, then ours — a socket on the port, open to
 * broadcast and hearing it. socInit done, it stays; the socket does not
 * survive a sleep (the wireless does not either), and is bound again for a
 * session. */
static bool StartRadioWire(void)
{
    struct sockaddr_in addr;
    int on = 1;

    if (sL.radioReady)
        return true;
    /* Right after the system wakes the network takes a moment to come back:
     * a few tries before the wireless counts as switched off. */
    for (int tries = 0; tries < 8 && !sL.quit; tries++)
    {
        if (tries)
            svcSleepThread(250000000LL);
        if (!sL.socReady)
        {
            Result ret = socInit(sSocBuffer, sizeof(sSocBuffer));

            if (R_FAILED(ret))
            {
                static unsigned told;

                if (told++ < 8)
                    CtrLog_Write(CTR_LOG_ERROR, "local: socInit failed (%08lx)", (unsigned long)ret);
                continue;
            }
            sL.socReady = true;
        }
        sL.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sL.sock < 0)
            continue;
        setsockopt(sL.sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        fcntl(sL.sock, F_SETFL, O_NONBLOCK);
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(UDP_PORT);
        if (bind(sL.sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            sL.radioReady = true;
            CtrLog_Write(CTR_LOG_GAME, "local: the socket is up, port %u", UDP_PORT);
            return true;
        }
        close(sL.sock);
        sL.sock = -1;
    }
    CtrLog_Write(CTR_LOG_ERROR, "local: no socket, the wireless is off?");
    return false;
}

#endif /* LOCAL_LINK_UDP */

/* After sleep: the system switched the wireless off, and the radios' state
 * is gone with it — the socket closed, the service's handle open but past
 * its own state. Left as they were, every scan and host from then on failed
 * and the counters said the wireless was off. Closed here, both are started
 * afresh by the next session. */
static void AfterWake(void)
{
    bool was = sL.inNetwork;

    if (sL.inNetwork)
        LeaveNetwork(true);
#ifdef LOCAL_LINK_UDP
    if (sL.radioReady)
    {
        close(sL.sock);
        sL.sock = -1;
        sL.radioReady = false;
    }
#endif
#ifndef LOCAL_LINK_NO_UDS
    if (sL.udsReady)
        udsExit();
    sL.udsReady = false;
#endif
    sL.inNetwork = false;
    sL.hosting = false;
    if (was || sL.state == LOCAL_SESSION_IN || sL.state == LOCAL_SESSION_SEARCHING)
        EndSession(LOCAL_SESSION_NONE);
    CtrLog_Write(CTR_LOG_GAME, "local: woke from sleep: the wireless closed%s", was ? ", session ended" : "");
}

#ifndef LOCAL_LINK_NO_UDS
/* The air: the system's local wireless. */
static bool StartRadioAir(void)
{
    Result ret = 0;

    if (sL.udsReady)
        return true;
    /* Right after the system wakes the radio takes a moment to come back:
     * a few tries before the wireless counts as switched off. */
    for (int tries = 0; tries < 8 && !sL.quit; tries++)
    {
        if (tries)
            svcSleepThread(250000000LL);
        ret = udsInit(SHAREDMEM_SIZE, NULL);
        if (R_SUCCEEDED(ret))
        {
            if (tries)
                CtrLog_Write(CTR_LOG_GAME, "local: udsInit worked on try %d", tries + 1);
            sL.udsReady = true;
            return true;
        }
    }
    CtrLog_Write(CTR_LOG_ERROR, "local: udsInit failed (%08lx)", (unsigned long)ret);
    return false;
}

/* The wireless service started again: what it was in is gone. On hardware it
 * was once left refusing to scan or host (c8a113ea, a wrong state) after a
 * session ended. */
static bool RestartRadio(void)
{
    CtrLog_Write(CTR_LOG_GAME, "local: restarting the wireless service");
    if (sL.inNetwork)
        udsUnbind(&sL.bind);
    sL.inNetwork = false;
    sL.hosting = false;
    if (sL.udsReady)
        udsExit();
    sL.udsReady = false;
    svcSleepThread(100000000LL);
    return StartRadioAir();
}

/* The air's service in no network (nothing left over from the last
 * session), or started again. */
static bool AirIdle(void)
{
    udsConnectionStatus st;
    Result ret;

    ret = udsGetConnectionStatus(&st);
    if (R_SUCCEEDED(ret) && st.status == STATUS_IDLE)
        return true;
    CtrLog_Write(CTR_LOG_GAME, "local: wireless not idle (%08lx, status %lu): closing", (unsigned long)ret, R_SUCCEEDED(ret) ? (unsigned long)st.status : 0ul);
    if (R_SUCCEEDED(ret) && st.status == STATUS_HOST)
        ret = udsDestroyNetwork();
    else if (R_SUCCEEDED(ret))
        ret = udsDisconnectNetwork();
    if (sL.inNetwork)
        udsUnbind(&sL.bind);
    sL.inNetwork = false;
    sL.hosting = false;
    if (R_SUCCEEDED(udsGetConnectionStatus(&st)) && st.status == STATUS_IDLE)
        return true;
    return RestartRadio();
}
#endif /* not LOCAL_LINK_NO_UDS */

/* Before looking or hosting: the radios up, the air's service idle. In the
 * build with both, either radio will do: the session fails only if neither
 * came up. */
static bool EnsureIdle(void)
{
#ifdef LOCAL_LINK_NO_UDS
    return StartRadioWire();
#elif !defined(LOCAL_LINK_UDP)
    return StartRadioAir() && AirIdle();
#else
    bool wire = StartRadioWire();

    if (!StartRadioAir())
        return wire; /* the air is out; the wire may not be */
    return AirIdle() || wire;
#endif
}

static const char *BuildHex(char *out, const uint8_t *id)
{
    for (int i = 0; i < BUILD_ID_SIZE; i++)
        snprintf(out + 2 * i, 3, "%02x", id[i]);
    return out;
}

#ifdef LOCAL_LINK_UDP
/* What a beacon says about the session looked for. */
static int UdpSessionCheck(const struct UdpBeacon *b, uint8_t kind, uint32_t *token)
{
    if (memcmp(b->app.magic, kMagic, sizeof(kMagic)) != 0)
        return SESSION_NO;
    if (b->app.kind != kind)
        return SESSION_NO;
    /* A bridge is a build of its own, and serves all of ours: what its
     * beacon names is not held against it. */
    if (b->app.version != PROTOCOL_VERSION
     || (!(b->flags & UDP_FLAG_BRIDGE) && memcmp(b->app.build, sBuildId, BUILD_ID_SIZE) != 0))
    {
        static uint8_t told[BUILD_ID_SIZE];

        if (!(b->flags & UDP_FLAG_BRIDGE) && memcmp(told, b->app.build, sizeof(told)) != 0)
        {
            char hex[2 * BUILD_ID_SIZE + 1];

            memcpy(told, b->app.build, sizeof(told));
            CtrLog_Write(CTR_LOG_GAME, "local: a session of this counter nearby is another build (protocol %u, build %s)",
                         b->app.version, BuildHex(hex, b->app.build));
        }
        return SESSION_OTHER;
    }
    if (token)
        *token = b->app.token;
    return SESSION_YES;
}

/* Joining is one thing: where the host is. Our first datagram there, our
 * advert in it, tells it a player came. */
static bool UdpConnect(const struct sockaddr_in *src, const struct UdpBeacon *b)
{
    sL.hostAddr = *src;
    sL.haveHost = true;
    sL.hosting = false;
    sL.backend = BACKEND_WIRE;
    CtrLog_Write(CTR_LOG_GAME, "local: joining the session at %s%s", inet_ntoa(src->sin_addr),
                 (b->flags & UDP_FLAG_BRIDGE) ? ", the bridge" : "");
    return true;
}
#endif /* LOCAL_LINK_UDP */
#ifndef LOCAL_LINK_NO_UDS
static int SessionCheck(const udsNetworkScanInfo *net, uint8_t kind, uint32_t *token)
{
    struct AppData app = {0};   /* a short beacon leaves the rest unread */
    size_t size = 0;

    if (R_FAILED(udsGetNetworkStructApplicationData(&net->network, &app, sizeof(app), &size))
     || size < 6 || memcmp(app.magic, kMagic, sizeof(kMagic)) != 0)
        return SESSION_NO;
    if (app.kind != kind)
        return SESSION_NO;
    if (app.version != PROTOCOL_VERSION || size < sizeof(app)
     || memcmp(app.build, sBuildId, BUILD_ID_SIZE) != 0)
    {
        static uint8_t told[BUILD_ID_SIZE];

        if (memcmp(told, app.build, sizeof(told)) != 0)
        {
            char hex[2 * BUILD_ID_SIZE + 1];

            memcpy(told, app.build, sizeof(told));
            CtrLog_Write(CTR_LOG_GAME, "local: a session of this counter nearby is another build (protocol %u, build %s)",
                         app.version, BuildHex(hex, app.build));
        }
        return SESSION_OTHER;
    }
    if (token)
        *token = app.token;
    return net->network.total_nodes < net->network.max_nodes ? SESSION_YES : SESSION_NO;
}

static bool Connect(const udsNetworkScanInfo *net)
{
    Result ret = udsConnectNetwork(&net->network, sPassphrase, strlen(sPassphrase) + 1, &sL.bind,
                                   UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, DATA_CHANNEL,
                                   UDS_DEFAULT_RECVBUFSIZE);

    if (R_FAILED(ret))
    {
        CtrLog_Write(CTR_LOG_ERROR, "local: joining failed (%08lx)", (unsigned long)ret);
        return false;
    }
    sL.hosting = false;
    sL.backend = BACKEND_AIR;
    return true;
}
#endif /* not LOCAL_LINK_NO_UDS */

/* The search is no longer wanted: the game asked to leave or to start again,
 * the system slept, or the program is closing. Left to run, it went on to
 * host a session nobody asked for. */
static bool SearchCancelled(void)
{
    bool cancelled;

    LightLock_Lock(&sL.lock);
    cancelled = sL.quit || sL.woke || sL.leaveAsked || sL.startAsked;
    LightLock_Unlock(&sL.lock);
    return cancelled;
}

/* otherBuild: a session of this counter of another build was seen. */
static bool JoinNearby(uint8_t kind, uint32_t lookMs, bool *otherBuild)
{
    uint32_t until = LocalLink_Milliseconds() + lookMs;
#ifndef LOCAL_LINK_NO_UDS
    int failures = 0;
    static uint32_t scanBuffer[0x4000 / 4];
#endif

#ifdef LOCAL_LINK_UDP
    /* What gathered in the socket while nothing was going on: gone. */
    while (UdpPull(NULL, sUdpBuf, sizeof(sUdpBuf), 0) > 0)
        ;
#endif
    do
    {
#ifndef LOCAL_LINK_NO_UDS
        /* The air first: the consoles nearby. */
        udsNetworkScanInfo *nets = NULL;
        size_t total = 0;
        Result ret = udsScanBeacons(scanBuffer, sizeof(scanBuffer), &nets, &total, COMM_ID, 0, NULL, false);

        if (R_FAILED(ret))
        {
            CtrLog_Write(CTR_LOG_ERROR, "local: scan failed (%08lx)", (unsigned long)ret);
            free(nets);
            /* Once more from scratch; then give up for this time. */
            if (++failures > 1 || !RestartRadio())
                return false;
            continue;
        }
        for (size_t i = 0; i < total; i++)
        {
            int check = SessionCheck(&nets[i], kind, NULL);

            if (check == SESSION_OTHER)
                *otherBuild = true;
            else if (check == SESSION_YES && Connect(&nets[i]))
            {
                free(nets);
                return true;
            }
        }
        free(nets);
#endif
#ifdef LOCAL_LINK_UDP
        /* Then the wire: a bridge's beacon, when one is heard. */
        {
            struct sockaddr_in src;
            struct UdpBeacon beacon;
            uint32_t token = 0;
            int check, got = UdpPull(&src, sUdpBuf, sizeof(sUdpBuf), 100);

#ifdef LOCAL_LINK_NO_UDS
            if (got < 0)
                return false;
#else
            if (got < 0)
                continue; /* no socket: the air is still an option */
#endif
            if (got == 0 || !UdpBeaconAt(sUdpBuf, (uint32_t)got, &beacon))
                continue;
            check = UdpSessionCheck(&beacon, kind, &token);
            if (check == SESSION_OTHER)
                *otherBuild = true;
            else if (check == SESSION_YES && UdpConnect(&src, &beacon))
                return true;
        }
#endif
    } while (!SearchCancelled() && LocalLink_Milliseconds() < until);
    return false;
}

#ifdef LOCAL_LINK_NO_UDS
static bool HostSession(uint8_t kind)
{
    (void)kind; /* the beacon says the rest: it is made when it is sent */
    sL.hosting = true;
    sL.backend = BACKEND_WIRE;
    sL.nextBeaconMs = 0;
    sL.nextMergeMs = LocalLink_Milliseconds() + MERGE_MS + (uint32_t)(rand() % MERGE_JITTER_MS);
    return true;
}
#else
static bool HostSession(uint8_t kind)
{
    udsNetworkStruct net;
    struct AppData app = {{0}, PROTOCOL_VERSION, kind, {0, 0}, sL.token, {0}};
    Result ret;

    memcpy(app.magic, kMagic, sizeof(kMagic));
    memcpy(app.build, sBuildId, BUILD_ID_SIZE);
    udsGenerateDefaultNetworkStruct(&net, COMM_ID, 0, MAX_NODES);
    ret = udsCreateNetwork(&net, sPassphrase, strlen(sPassphrase) + 1, &sL.bind, DATA_CHANNEL,
                           UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(ret))
    {
        CtrLog_Write(CTR_LOG_ERROR, "local: hosting failed (%08lx)", (unsigned long)ret);
        if (!RestartRadio())
            return false;
        ret = udsCreateNetwork(&net, sPassphrase, strlen(sPassphrase) + 1, &sL.bind, DATA_CHANNEL,
                               UDS_DEFAULT_RECVBUFSIZE);
        if (R_FAILED(ret))
        {
            CtrLog_Write(CTR_LOG_ERROR, "local: hosting failed again (%08lx)", (unsigned long)ret);
            return false;
        }
    }
    ret = udsSetApplicationData(&app, sizeof(app));
    if (R_FAILED(ret))
        CtrLog_Write(CTR_LOG_ERROR, "local: beacon data not set (%08lx)", (unsigned long)ret);
    sL.hosting = true;
    sL.backend = BACKEND_AIR;
    sL.nextMergeMs = LocalLink_Milliseconds() + MERGE_MS + (uint32_t)(rand() % MERGE_JITTER_MS);
    return true;
}
#endif /* LOCAL_LINK_NO_UDS */

/* A new session hosted: lone, it may look for another for a while. */
static bool HostNewSession(uint8_t kind)
{
    if (!HostSession(kind))
        return false;
    sL.mergeUntilMs = LocalLink_Milliseconds() + MERGE_WINDOW_MS;
    return true;
}

/* Who's in the network we just joined or made: our id, and the others so far
 * (the status event may already be spent). */
static void TakeNetwork(bool quiet)
{
    sL.inNetwork = true;
    if (sL.backend == BACKEND_WIRE)
    {
#ifdef LOCAL_LINK_UDP
        /* A session of two: the host is 1 and we are 2. Its advert comes
         * with its first messages, as ours go with ours. */
        LightLock_Lock(&sL.lock);
        for (uint16_t id = 1; id < NODE_SLOTS; id++)
            memset(&sL.nodes[id], 0, sizeof(sL.nodes[id]));
        sL.myId = sL.hosting ? 1 : UDP_CLIENT_ID;
        NodeJoined(sL.myId);
        if (!sL.hosting)
        {
            NodeJoined(1);
            sL.nodeMask = (1u << 0) | (1u << (sL.myId - 1));
        }
        else
            sL.nodeMask = 1u << 0;
        LightLock_Unlock(&sL.lock);
        if (!quiet)
            CtrLog_Write(CTR_LOG_GAME, "local: %s a session as player %u",
                         sL.hosting ? "hosting" : "joined", sL.myId);
#endif
        return;
    }
#ifndef LOCAL_LINK_NO_UDS
    {
        udsConnectionStatus st = {0};

        /* The status event spent before the status is read: a player joining or
         * leaving after the read signals it again, and UpdateNodes sees it. */
        udsWaitConnectionStatusEvent(false, false);
        if (R_FAILED(udsGetConnectionStatus(&st)) || st.cur_NetworkNodeID == 0)
        {
            /* No id and no node mask to trust: garbage would join random nodes. */
            CtrLog_Write(CTR_LOG_ERROR, "local: no connection status, session dropped");
            LeaveNetwork(true);
            EndSession(LOCAL_SESSION_FAILED);
            return;
        }
        LightLock_Lock(&sL.lock);
        for (uint16_t id = 1; id < NODE_SLOTS; id++)
            memset(&sL.nodes[id], 0, sizeof(sL.nodes[id]));
        sL.myId = st.cur_NetworkNodeID;
        for (uint16_t id = 1; id < NODE_SLOTS; id++)
            if (st.node_bitmask & (1u << (id - 1)))
                NodeJoined(id);
        sL.nodeMask = st.node_bitmask;
        LightLock_Unlock(&sL.lock);
        if (!quiet)
            CtrLog_Write(CTR_LOG_GAME, "local: %s a session as player %u (%u in it)",
                     sL.hosting ? "hosting" : "joined", st.cur_NetworkNodeID, st.total_nodes);
    }
#endif
}

#ifdef LOCAL_LINK_UDP
/* The wire half of a lone host's look (see MergeAlone): the session its
 * socket last heard — a bridge's, or a lone wire host's — if it is fresh,
 * of this counter, and worth moving to: players in it, or a lone one whose
 * token is lower. Whether we moved. */
static bool MergeToWire(void)
{
    struct sockaddr_in addr;
    struct UdpBeacon beacon;
    uint32_t token = 0;

    if (!sL.beaconMs || LocalLink_Milliseconds() - sL.beaconMs >= UDP_BEACON_STALE_MS)
        return false;
    if (UdpSessionCheck(&sL.beacon, sL.kind, &token) != SESSION_YES || token == sL.token)
        return false;
    if (sL.beacon.players <= 1 && token > sL.token)
        return false;
    addr = sL.beaconAddr;
    beacon = sL.beacon;
    CtrLog_Write(CTR_LOG_GAME, "local: another session of this counter nearby, moving to it");
    LeaveNetwork(true);
    if (UdpConnect(&addr, &beacon))
    {
        TakeNetwork(false);
        return true;
    }
    if (!HostSession(sL.kind))
    {
        EndSession(LOCAL_SESSION_FAILED);
        return true;
    }
    TakeNetwork(true);
    return true;
}
#endif

#ifndef LOCAL_LINK_NO_UDS
/* The air half of a lone host's look (see MergeAlone). A host can look
 * while its network stays up (the system scans "on connection"); where that
 * fails (Azahar doesn't have it), the network is closed while it looks and
 * opened again if it stays. A player trying to join meanwhile finds
 * nothing, hosts, and the two meet the next time one of them looks. */
static void MergeAloneAir(void)
{
    static uint32_t scanBuffer[0x4000 / 4];
    static bool scanHostingFails;
    udsNetworkScanInfo *nets = NULL;
    size_t total = 0, target = 0;
    bool found = false;
    Result ret = -1;

    if (!scanHostingFails)
    {
        ret = udsScanBeacons(scanBuffer, sizeof(scanBuffer), &nets, &total, COMM_ID, 0, NULL, true);
        if (R_FAILED(ret))
        {
            CtrLog_Write(CTR_LOG_GAME, "local: no scanning while hosting (%08lx): closing the session to look", (unsigned long)ret);
            scanHostingFails = true;
            free(nets);
            nets = NULL;
            total = 0;
        }
    }
    if (scanHostingFails)
    {
        LeaveNetwork(true);
        ret = EnsureIdle() ? udsScanBeacons(scanBuffer, sizeof(scanBuffer), &nets, &total, COMM_ID, 0, NULL, false) : -1;
        if (R_FAILED(ret))
        {
            CtrLog_Write(CTR_LOG_ERROR, "local: looking while alone failed (%08lx)", (unsigned long)ret);
            free(nets);
            nets = NULL;
            total = 0;
        }
    }
    for (size_t i = 0; i < total && !found; i++)
    {
        uint32_t token = 0;

        if (SessionCheck(&nets[i], sL.kind, &token) != SESSION_YES || token == sL.token)
            continue;
        if (nets[i].network.total_nodes <= 1 && token > sL.token)
            continue;
        found = true;
        target = i;
    }
    if (found)
    {
        CtrLog_Write(CTR_LOG_GAME, "local: another session of this counter nearby, moving to it");
        if (sL.inNetwork)
            LeaveNetwork(true);
        if (!EnsureIdle() || !Connect(&nets[target]))
            found = false;
    }
    free(nets);
    if (sL.inNetwork)
        return; /* looked while hosting, and stays */
    if (!found && !HostSession(sL.kind))
    {
        EndSession(LOCAL_SESSION_FAILED);
        return;
    }
    TakeNetwork(!found);
}
#endif /* not LOCAL_LINK_NO_UDS */

/*
 * A host still alone in its session looks for another session of its
 * counter, and moves to it: one with players in it, or another lone host's
 * if that host's token is lower (the other one stays put, so the two don't
 * both move). Nobody is linked to a lone host, so the adapter carries on
 * as it was: the session's generation stays the same.
 *
 * With both radios in, the air is looked first (the consoles nearby); a
 * bridge the socket has heard — the way to the Switch's GBA games — is
 * moved to if the air had nothing.
 */
static void MergeAlone(void)
{
    uint32_t now = LocalLink_Milliseconds();

    if (!sL.hosting || sL.myId == 0 || now < sL.nextMergeMs || now >= sL.mergeUntilMs
     || (sL.nodeMask & ~(1u << (sL.myId - 1))))
        return;
    sL.nextMergeMs = now + MERGE_MS + (uint32_t)(rand() % MERGE_JITTER_MS);
#if !defined(LOCAL_LINK_NO_UDS) && defined(LOCAL_LINK_UDP)
    MergeAloneAir();
    if (sL.hosting && (sL.nodeMask & ~(1u << (sL.myId - 1))) == 0)
        MergeToWire(); /* still lone: the bridge the socket heard, then */
#elif !defined(LOCAL_LINK_NO_UDS)
    MergeAloneAir();
#elif defined(LOCAL_LINK_UDP)
    MergeToWire();
#endif
}

static void StartSession(uint8_t kind)
{
    bool in, otherBuild = false;

    LeaveNetwork(false);
    EndSession(LOCAL_SESSION_SEARCHING);
    if (!EnsureIdle())
    {
        EndSession(LOCAL_SESSION_FAILED);
        return;
    }
    CtrLog_Write(CTR_LOG_GAME, "local: looking for a session of kind %u", kind);
    in = JoinNearby(kind, SEARCH_MS + (uint32_t)(rand() % SEARCH_JITTER_MS), &otherBuild);
    /* Still searching: the thread's next pass deals with what came. */
    if (!in && SearchCancelled())
        return;
    if (!in && otherBuild)
    {
        /* Hosting, we would wait for players who can't come: the ones nearby
         * have another build. */
        EndSession(LOCAL_SESSION_OTHER_BUILD);
        return;
    }
    if (!in)
        in = EnsureIdle() && HostNewSession(kind);
    if (!in)
    {
        EndSession(LOCAL_SESSION_FAILED);
        return;
    }
    LightLock_Lock(&sL.lock);
    sL.kind = kind;
    sL.generation++;
    sL.state = LOCAL_SESSION_IN;
    LightLock_Unlock(&sL.lock);
    TakeNetwork(false);
}

/* Systems we wait on that went silent are gone (see SILENT_MS). Without the
 * host (node 1) there is no session; a host sends a silent client away. */
static void DropSilent(uint32_t now)
{
    for (uint16_t id = 1; id < NODE_SLOTS; id++)
    {
        struct Node *n = &sL.nodes[id];
        bool silent;

        LightLock_Lock(&sL.lock);
        /* An idle session sends nothing: what counts is how long the
         * oldest message waits, and nothing heard all that time. */
        /* Not one never heard from: right after a join (a merge most of
         * all) the first frames can take a while each way, and in Azahar
         * the first many seconds; a player then dropped had to start
         * again at the counter. */
        silent = n->present && n->heard && id != sL.myId && n->outCount && n->out[n->outHead].sentMs
              && (int32_t)(now - n->out[n->outHead].firstMs) >= SILENT_MS
              && (int32_t)(now - n->heardMs) >= SILENT_MS;
        LightLock_Unlock(&sL.lock);
        if (!silent)
            continue;
        CtrLog_Write(CTR_LOG_GAME, "local: player %u silent for %lu ms, a message waiting %lu ms: gone", id,
                     (unsigned long)(now - n->heardMs), (unsigned long)(now - n->out[n->outHead].firstMs));
        if (!sL.hosting && id == 1) /* the host */
        {
            LeaveNetwork(false);
            EndSession(LOCAL_SESSION_NONE);
            return;
        }
#ifndef LOCAL_LINK_NO_UDS
        if (sL.hosting)
            udsEjectClient(id);
#endif
        LightLock_Lock(&sL.lock);
        NodeLeft(id);
        sL.nodeMask &= ~(1u << (id - 1));
        LightLock_Unlock(&sL.lock);
    }
}

/* Messages waiting for their ack, or an ack waiting to go: the thread looks
 * again soon (lock held). */
static bool Unacked(void)
{
    for (uint16_t id = 1; id < NODE_SLOTS; id++)
        if (sL.nodes[id].present && (sL.nodes[id].outCount || sL.nodes[id].ackDue))
            return true;
    return false;
}

static void LocalThread(void *unused)
{
    bool frameReady = false; /* the bind's event woke us (the air) */
    uint32_t lastRead = 0;

    (void)unused;
    /* rand() is per thread (newlib's reent): unseeded here it started from 1
     * on every system, the same search and merge waits on all of them, and
     * two players who talked to their attendants together both hosted. */
    srand((unsigned)svcGetSystemTick() ^ sL.token);
    while (!sL.quit)
    {
        bool start, leave;
        uint8_t kind;

        if (sL.woke)
        {
            sL.woke = false;
            AfterWake();
        }
        LightLock_Lock(&sL.lock);
        start = sL.startAsked;
        leave = sL.leaveAsked;
        kind = sL.askedKind;
        sL.startAsked = sL.leaveAsked = false;
        /* Searching from now: a session that just ended (EndSession(NONE))
         * left NONE here, which the game read as a failed search. */
        if (start)
            sL.state = LOCAL_SESSION_SEARCHING;
        LightLock_Unlock(&sL.lock);

        if (start)
        {
            StartSession(kind);
        }
        else if (leave)
        {
            LeaveNetwork(false);
            EndSession(LOCAL_SESSION_NONE);
        }

        if (sL.inNetwork)
        {
            uint32_t now = LocalLink_Milliseconds();

#if defined(LOCAL_LINK_UDP) && !defined(LOCAL_LINK_NO_UDS)
            if (sL.backend == BACKEND_AIR && sL.hosting)
                UdpHearBeacons(); /* a lone host on the air hears the wire's bridges */
#endif
#ifndef LOCAL_LINK_NO_UDS
            if (sL.backend == BACKEND_AIR)
                UpdateNodes();
#endif
#ifdef LOCAL_LINK_UDP
            if (sL.backend == BACKEND_WIRE)
                UdpUpdate(now); /* the beacon, if we host */
#endif
            if (sL.inNetwork)
                DropSilent(now);
            if (sL.inNetwork)
                MergeAlone();
            if (sL.inNetwork)
            {
                if (sL.backend == BACKEND_WIRE || frameReady || now - lastRead >= IDLE_MS)
                {
                    ReadFrames();
                    lastRead = now;
                }
                LightLock_Lock(&sL.lock);
                for (uint16_t id = 1; id < NODE_SLOTS; id++)
                    if (sL.nodes[id].present && id != sL.myId)
                        FlushNode(id, now);
                LightLock_Unlock(&sL.lock);
            }
            if (now - sL.stats.sinceMs >= STATS_MS)
            {
                if (sL.stats.sinceMs)
                    CtrLog_Write(CTR_LOG_GAME, "local: %u s: %lu wakes, %lu reads, %lu frames in, %lu out, %lu messages resent",
                                 (unsigned)((now - sL.stats.sinceMs) / 1000),
                                 (unsigned long)sL.stats.wakes, (unsigned long)sL.stats.pulls,
                                 (unsigned long)sL.stats.framesIn, (unsigned long)sL.stats.framesOut,
                                 (unsigned long)sL.stats.resent);
                memset(&sL.stats, 0, sizeof(sL.stats));
                sL.stats.sinceMs = now;
            }
        }
        {
            Handle handles[2];
            s32 count = 0, which = -1;
            bool busy;
            uint32_t wait;

            LightLock_Lock(&sL.lock);
            busy = sL.backend == BACKEND_WIRE || Unacked();
            LightLock_Unlock(&sL.lock);
            wait = (sL.inNetwork && busy ? WAIT_MS : IDLE_MS) * 1000000LL;
            handles[count++] = sL.wake;
#ifndef LOCAL_LINK_NO_UDS
            if (sL.inNetwork && sL.backend == BACKEND_AIR && sL.bind.event)
                handles[count++] = sL.bind.event;
#endif
            svcWaitSynchronizationN(&which, handles, count, false, wait);
            frameReady = which == 1;
            sL.stats.wakes++;
        }
    }
    LeaveNetwork(false);
}

/* The thread has something to do. */
static void Wake(void)
{
    if (sL.wake)
        svcSignalEvent(sL.wake);
}

/* ---- The game's side ---------------------------------------------------- */

void LocalLink_Init(void)
{
    s32 priority = 0x30;

    if (sL.ready)
        return;
    LightLock_Init(&sL.lock);
    sL.state = LOCAL_SESSION_NONE;
    {
        FILE *file = CtrFs_OpenAsset(BUILD_ID_PATH);
        size_t got = file ? fread(sBuildId, 1, BUILD_ID_SIZE, file) : 0;
        int n = snprintf(sPassphrase, sizeof(sPassphrase), "%s ", PASSPHRASE);

        if (file)
            fclose(file);
        if (got != BUILD_ID_SIZE)
        {
            /* Links only with builds without one too. */
            memset(sBuildId, 0, sizeof(sBuildId));
            CtrLog_Write(CTR_LOG_ERROR, "local: no %s", BUILD_ID_PATH);
        }
        for (int i = 0; i < 16 && n + 2 < (int)sizeof(sPassphrase); i++)
            n += snprintf(sPassphrase + n, sizeof(sPassphrase) - n, "%02x", sBuildId[i]);
        {
            char hex[2 * BUILD_ID_SIZE + 1];

            CtrLog_Write(CTR_LOG_GAME, "local: build %s", BuildHex(hex, sBuildId));
        }
    }
    if (R_FAILED(svcCreateEvent(&sL.wake, RESET_ONESHOT)))
    {
        CtrLog_Write(CTR_LOG_ERROR, "local: no event, local wireless off");
        return;
    }
    srand((unsigned)svcGetSystemTick());
    sL.token = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    /* Just below the game: it runs while the game waits for the next frame. */
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    sL.thread = threadCreate(LocalThread, NULL, THREAD_STACK, priority < 0x3F ? priority + 1 : 0x3F, -2, false);
    if (sL.thread == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "local: no thread, local wireless off");
        svcCloseHandle(sL.wake);
        sL.wake = 0;
        return;
    }
    sL.ready = true;
}

void LocalLink_Shutdown(void)
{
    if (!sL.ready)
        return;
    sL.quit = true;
    Wake();
    if (R_FAILED(threadJoin(sL.thread, 3000000000LL)))
    {
        /* Still inside UDS (a scan, a join): closing its handle and the
         * service under it would crash or hang the exit. */
        CtrLog_Write(CTR_LOG_ERROR, "local: thread still busy at exit, UDS left open");
        return;
    }
    threadFree(sL.thread);
    svcCloseHandle(sL.wake);
    sL.wake = 0;
#ifdef LOCAL_LINK_UDP
    if (sL.radioReady)
    {
        close(sL.sock);
        sL.sock = -1;
        sL.radioReady = false;
    }
    if (sL.socReady)
        socExit();
    sL.socReady = false;
#endif
#ifndef LOCAL_LINK_NO_UDS
    if (sL.udsReady)
        udsExit();
    sL.udsReady = false;
#endif
    sL.ready = false;
}

uint8_t LocalLink_Enabled(void)
{
    return sL.ready;
}

uint16_t LocalLink_MyId(void)
{
    uint16_t id;

    if (!sL.ready)
        return 0;
    LightLock_Lock(&sL.lock);
    id = sL.state == LOCAL_SESSION_IN ? sL.myId : 0;
    LightLock_Unlock(&sL.lock);
    return id;
}

uint32_t LocalLink_Generation(void)
{
    uint32_t gen;

    if (!sL.ready)
        return 0;
    LightLock_Lock(&sL.lock);
    gen = sL.generation;
    LightLock_Unlock(&sL.lock);
    return gen;
}

void LocalLink_SetAdvert(uint8_t hosting, uint8_t nextSlot, const uint8_t *data)
{
    uint8_t has = data != NULL;

    if (!sL.ready)
        return;
    LightLock_Lock(&sL.lock);
    if (hosting != sL.advertHosting || nextSlot != sL.advertNextSlot || has != sL.hasAdvert
     || (has && memcmp(data, sL.advert, LOCAL_ADVERT_SIZE) != 0))
    {
        sL.advertHosting = hosting;
        sL.advertNextSlot = nextSlot;
        sL.hasAdvert = has;
        if (has)
            memcpy(sL.advert, data, LOCAL_ADVERT_SIZE);
        for (uint16_t id = 1; id < NODE_SLOTS; id++)
            if (sL.nodes[id].present && id != sL.myId)
                QueueAdvert(id);
        Wake();
    }
    LightLock_Unlock(&sL.lock);
}

int LocalLink_GetPeers(struct LocalPeer *out, int max)
{
    int n = 0;
    uint32_t now = LocalLink_Milliseconds();

    if (!sL.ready)
        return 0;
    LightLock_Lock(&sL.lock);
    for (uint16_t id = 1; id < NODE_SLOTS && n < max; id++)
    {
        struct LocalPeer *p = &sL.nodes[id].info;

        if (!sL.nodes[id].present || id == sL.myId || sL.state != LOCAL_SESSION_IN)
            continue;
        out[n] = *p;
        out[n].hostedAgoMs = p->hosting ? 0 : p->lastHostingMs ? now - p->lastHostingMs : 0xFFFFFFFFu;
        n++;
    }
    LightLock_Unlock(&sL.lock);
    return n;
}

void LocalLink_Send(uint16_t dest, const void *data, uint32_t len)
{
    if (!sL.ready)
        return;
    if (len > sizeof(((struct LocalEvent *)0)->data))
    {
        CtrLog_Write(CTR_LOG_ERROR, "local: message of %lu bytes cut", (unsigned long)len);
        len = sizeof(((struct LocalEvent *)0)->data);
    }
    LightLock_Lock(&sL.lock);
    if (sL.state == LOCAL_SESSION_IN)
        QueueMessage(dest, MSG_DATA, data, len);
    LightLock_Unlock(&sL.lock);
    Wake();
}

uint8_t LocalLink_PollEvent(struct LocalEvent *ev)
{
    uint8_t got = 0;

    if (!sL.ready)
        return 0;
    LightLock_Lock(&sL.lock);
    if (sL.eventCount)
    {
        *ev = sL.events[sL.eventHead];
        sL.eventHead = (sL.eventHead + 1) % EVENT_QUEUE;
        sL.eventCount--;
        got = 1;
    }
    LightLock_Unlock(&sL.lock);
    return got;
}

void LocalLink_StartSession(uint8_t kind)
{
    if (!sL.ready)
        return;
    LightLock_Lock(&sL.lock);
    sL.startAsked = true;
    sL.leaveAsked = false;
    sL.askedKind = kind;
    sL.state = LOCAL_SESSION_SEARCHING;
    LightLock_Unlock(&sL.lock);
    Wake();
}

void LocalLink_LeaveSession(void)
{
    if (!sL.ready)
        return;
    LightLock_Lock(&sL.lock);
    if (sL.state == LOCAL_SESSION_IN || sL.state == LOCAL_SESSION_SEARCHING || sL.startAsked)
    {
        sL.leaveAsked = true;
        sL.startAsked = false;
    }
    LightLock_Unlock(&sL.lock);
    Wake();
}

void LocalLink_AfterWake(void)
{
    if (!sL.ready)
        return;
    sL.woke = true;
    Wake();
}

void LocalLink_LeaveBeforeSleep(void)
{
    if (!LocalLink_InSession())
        return;
    CtrLog_Write(CTR_LOG_GAME, "local: going to sleep: leaving the session");
    LocalLink_LeaveSession();
    for (int i = 0; i < 50 && LocalLink_InSession(); i++)
        svcSleepThread(10000000LL);
}

uint8_t LocalLink_GetSession(void)
{
    uint8_t state;

    if (!sL.ready)
        return LOCAL_SESSION_OFF;
    LightLock_Lock(&sL.lock);
    state = sL.startAsked ? LOCAL_SESSION_SEARCHING : sL.state;
    LightLock_Unlock(&sL.lock);
    return state;
}

uint8_t LocalLink_GetSessionKind(void)
{
    return sL.ready ? sL.kind : LOCAL_KIND_ANY;
}

uint8_t LocalLink_InSession(void)
{
    uint8_t state = LocalLink_GetSession();

    return state == LOCAL_SESSION_IN || state == LOCAL_SESSION_SEARCHING;
}
