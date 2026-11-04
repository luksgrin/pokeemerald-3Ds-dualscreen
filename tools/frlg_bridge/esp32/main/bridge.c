/*
 * The bridge's leg toward a 3DS: a C port of tools/frlg_bridge/bridge.py,
 * which mirrors 3ds_port/src/3ds_local.c's FlushNode/HandleFrame exactly.
 * The wire (little-endian, all packed by hand here):
 *
 *     beacon   52 bytes  <HHBBH> + <4sBB2sI32s>
 *                      senderId, flags, players, maxPlayers, 0;
 *                      "EM3L", version, kind, 0, token, build id (32B)
 *     data     <H> senderId, then frames:
 *     frame    <BBHHH>  magic 0xE3, version, ack (what I have of yours),
 *                      bodyLen, 0; messages; padded to a multiple of 4
 *     message  <HBBH>   seq, type, 0, len; data
 *                      type 1: the adapter's message (PEER_* below)
 *                      type 2: the advert [hosting][nextSlot][hasAdvert][24B]
 *
 * Today it is the harness's other end: a 3DS finds this bridge on the
 * network and meets it at the counter. The board's Switch leg relays to
 * there instead of echoing, once it exists.
 */
#include "bridge.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#else
/* The host test's other end: the same machine, the same clocks. */
#include <stdlib.h>
#include <time.h>
#endif

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- The wire's constants (3ds_local.c's, and bridge.py's) --------------- */

#define PORT_3DS 58325    /* where beacons go, and where a 3DS's socket is bound */
#define PORT_BRIDGE 58326 /* where our socket is bound */

#define FRAME_MAGIC 0xE3
#define PROTOCOL_VERSION 1
#define FLAG_BRIDGE 1

#define MSG_DATA 1
#define MSG_ADVERT 2

#define PEER_CONNECT 1
#define PEER_ACCEPT 2
#define PEER_REJECT 3
#define PEER_DATA 4
#define PEER_DISCONNECT 5

#define FRAME_MAX 1400  /* a frame's body at most (3ds_local.c's FRAME_MAX) */
#define MSG_MAX 168
#define MSG_HDR 6
#define OUT_MAX 128
#define RESEND_MS 100
#define ACK_DELAY_MS 30
#define FRAMES_A_FLUSH 4 /* FlushNode's budget per pass */

#define BEACON_MS 500
#define ADVERT_MS 1000
#define HEARTBEAT_MS 100
#define SILENT_MS 6000

#define LOCAL_ADVERT_SIZE 24
#define BEACON_SIZE 52    /* 8 of header + 44 of app data */
#define HOST_ID 1
#define CLIENT_ID 2

static const uint8_t kMagic[4] = {'E', 'M', '3', 'L'};

/* SeqBefore (3ds_local.c): a is behind b, in wraparound. */
static bool seq_before(uint16_t a, uint16_t b)
{
    return ((uint16_t)(a - b) & 0xFFFF) >= 0x8000;
}

/* Little-endian, laid out by hand: the same bytes on every machine. */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static uint16_t get16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* ---- The state (bridge.py's Client and Bridge) --------------------------- */

struct OutMsg
{
    uint16_t seq;
    uint8_t type;
    uint16_t len;
    uint8_t data[MSG_MAX];
    uint32_t sentMs;  /* 0: not sent yet */
    uint32_t firstMs; /* when it was first sent */
};

/* One 3DS: its address, our message stream to it, and its stream to us. */
struct BridgeClient
{
    struct sockaddr_in addr;
    uint16_t id;
    /* Its messages to us. */
    uint16_t expect;
    bool ackDue;
    uint32_t ackDueMs;
    /* Ours to it: the oldest unacknowledged first. */
    struct OutMsg out[OUT_MAX];
    uint16_t outHead, outCount;
    uint16_t nextSeq;
    bool overflowed;
    uint32_t lastHeard;
    /* The adapter protocol: it asked, we answered, and its next heartbeat
     * may go once this one's seq comes back in its ack. */
    bool accepted;
    uint8_t slot;
    int heartSeq; /* -1: none waiting */
    uint32_t dataFramesIn;
};

static struct
{
    uint8_t kind;
    bool verbose;
    bool loopback;
    uint32_t token;
    bool haveChild;
    struct BridgeClient child;
    uint32_t beaconDue, advertDue, heartbeatDue;
    uint8_t childAdvert[LOCAL_ADVERT_SIZE];
    int sock;
    struct sockaddr_in to3ds, toLoop;
} b;

/* ---- Saying --------------------------------------------------------------- */

static uint32_t millis(void);

static void say(const char *fmt, ...)
{
    va_list args;

    if (!b.verbose)
        return;
    va_start(args, fmt);
#ifdef ESP_PLATFORM
    {
        char line[192];

        vsnprintf(line, sizeof(line), fmt, args);
        ESP_LOGI("bridge", "%s", line);
    }
#else
    {
        char line[192];

        vsnprintf(line, sizeof(line), fmt, args);
        printf("[bridge %7u] %s\n", (unsigned)(millis() % 10000000u), line);
    }
#endif
    va_end(args);
}

static uint32_t millis(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
#endif
}

/* ---- The socket ------------------------------------------------------------ */

static int sendto_addr(const struct sockaddr_in *to, const void *buf, uint32_t len)
{
    return (int)sendto(b.sock, buf, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* ---- The beacon ------------------------------------------------------------ */

static void send_beacon(void)
{
    uint8_t packet[BEACON_SIZE];
    uint8_t players = 1 + (b.haveChild ? 1 : 0);

    put16(packet + 0, HOST_ID);
    put16(packet + 2, FLAG_BRIDGE);
    packet[4] = players;
    packet[5] = 5; /* maxPlayers (3ds_local.c's MAX_NODES) */
    put16(packet + 6, 0);
    memcpy(packet + 8, kMagic, 4);
    packet[12] = PROTOCOL_VERSION;
    packet[13] = b.kind;
    put16(packet + 14, 0);
    put16(packet + 16, (uint16_t)(b.token & 0xFFFF));
    put16(packet + 18, (uint16_t)(b.token >> 16));
    memset(packet + 20, 0, 32); /* the build id: the bridge has none */
    sendto_addr(&b.to3ds, packet, sizeof(packet));
    if (b.loopback)
        sendto_addr(&b.toLoop, packet, sizeof(packet));
}

/* ---- Our messages to the client: the go-back-N stream ---------------------- */

static struct OutMsg *out_at(struct BridgeClient *c, uint16_t i)
{
    return &c->out[(c->outHead + i) % OUT_MAX];
}

static void queue(struct BridgeClient *c, uint8_t mtype, const uint8_t *data, uint16_t len)
{
    struct OutMsg *m;

    if (len > MSG_MAX)
        len = MSG_MAX;
    if (c->outCount >= OUT_MAX)
    {
        if (!c->overflowed)
            say("queue to %u full", (unsigned)c->id);
        c->overflowed = true;
        return;
    }
    m = out_at(c, c->outCount);
    memset(m, 0, sizeof(*m));
    m->seq = c->nextSeq++;
    m->type = mtype;
    m->len = len;
    memcpy(m->data, data, len);
    c->outCount++;
}

/* The advert, that the player keeps seeing a host. */
static void queue_advert(struct BridgeClient *c)
{
    uint8_t msg[3 + LOCAL_ADVERT_SIZE];

    msg[0] = 1; /* hosting */
    msg[1] = c->accepted ? 1 : 0;
    msg[2] = 1; /* hasAdvert */
    memcpy(msg + 3, b.childAdvert, LOCAL_ADVERT_SIZE);
    queue(c, MSG_ADVERT, msg, sizeof(msg));
}

/* One datagram: our name, the frame header (its ack inside), the messages,
 * and padding to a multiple of 4. */
static int send_frame(struct BridgeClient *c, const uint8_t *body, uint16_t bodyLen)
{
    static uint8_t wire[2 + 8 + FRAME_MAX + 3];
    uint16_t size = 8 + bodyLen; /* what the padding aligns: header + body */
    uint16_t padded = (size + 3) & ~3u;

    wire[0] = HOST_ID;
    wire[1] = 0;
    wire[2] = FRAME_MAGIC;
    wire[3] = PROTOCOL_VERSION;
    put16(wire + 4, c->expect);
    put16(wire + 6, bodyLen);
    put16(wire + 8, 0);
    memcpy(wire + 10, body, bodyLen);
    memset(wire + 10 + bodyLen, 0, padded - size);
    return sendto_addr(&c->addr, wire, 2 + padded);
}

/* What's due to the client now, as FlushNode sends it: messages not sent
 * yet; when the oldest has waited too long, everything from it again; an
 * ack alone if nothing else goes for a while. */
static void flush_node(struct BridgeClient *c, uint32_t now)
{
    static uint8_t body[FRAME_MAX];
    bool resend = c->outCount && c->out[c->outHead].sentMs
               && now - c->out[c->outHead].sentMs >= RESEND_MS;
    uint16_t i = 0;
    int frames = 0;

    while (frames < FRAMES_A_FLUSH)
    {
        uint16_t bodyLen = 0;

        while (i < c->outCount)
        {
            struct OutMsg *m = out_at(c, i);

            if (m->sentMs && !resend)
            {
                i++;
                continue;
            }
            if (bodyLen + MSG_HDR + m->len > FRAME_MAX)
                break;
            put16(body + bodyLen, m->seq);
            body[bodyLen + 2] = m->type;
            body[bodyLen + 3] = 0;
            put16(body + bodyLen + 4, m->len);
            memcpy(body + bodyLen + MSG_HDR, m->data, m->len);
            bodyLen += MSG_HDR + m->len;
            m->sentMs = now;
            if (!m->firstMs)
                m->firstMs = now;
            i++;
        }
        if (bodyLen == 0 && !(c->ackDue && now - c->ackDueMs >= ACK_DELAY_MS))
            break;
        send_frame(c, body, bodyLen);
        c->ackDue = false;
        frames++;
        if (i >= c->outCount)
            break;
    }
}

/* ---- The client's messages ------------------------------------------------- */

static void on_peer(struct BridgeClient *c, const uint8_t *msg, uint16_t len)
{
    if (len < 1)
        return;
    switch (msg[0])
    {
    case PEER_CONNECT:
        if (!c->accepted)
        {
            c->accepted = true;
            c->slot = 0;
            say("PEER_CONNECT: accepted as slot %u", (unsigned)c->slot);
        }
        {
            uint8_t answer[2] = {PEER_ACCEPT, c->slot};

            queue(c, MSG_DATA, answer, sizeof(answer));
        }
        break;
    case PEER_ACCEPT:
        /* Only a child of a host sends that; we host. */
        say("PEER_ACCEPT? we are the host");
        break;
    case PEER_REJECT:
        say("PEER_REJECT");
        break;
    case PEER_DATA:
        if (len < 2)
            break;
        /* The harness stands for the game on the bridge's other side: its
         * data comes back as ours. The board's Switch leg relays it there
         * instead. */
        c->dataFramesIn++;
        {
            uint16_t n = len > MSG_MAX ? MSG_MAX : len;
            uint8_t echo[MSG_MAX];

            echo[0] = PEER_DATA;
            memcpy(echo + 1, msg + 1, (uint16_t)(n - 1));
            queue(c, MSG_DATA, echo, n);
        }
        break;
    case PEER_DISCONNECT:
        say("PEER_DISCONNECT");
        b.haveChild = false;
        break;
    }
}

static void on_message(struct BridgeClient *c, uint8_t mtype, const uint8_t *data, uint16_t len)
{
    if (mtype == MSG_ADVERT)
    {
        bool any = false;

        if (len < 3 + LOCAL_ADVERT_SIZE || !data[2])
            return;
        for (int i = 0; i < LOCAL_ADVERT_SIZE; i++)
            any = any || data[3 + i] != 0;
        if (any)
        {
            memcpy(b.childAdvert, data + 3, LOCAL_ADVERT_SIZE);
            say("the player's broadcast: %02x%02x%02x%02x...", b.childAdvert[0],
                b.childAdvert[1], b.childAdvert[2], b.childAdvert[3]);
        }
    }
    else if (mtype == MSG_DATA)
    {
        on_peer(c, data, len);
    }
}

static void on_frame(struct BridgeClient *c, const uint8_t *frame, size_t len, uint32_t now)
{
    uint8_t magic, version;
    uint16_t ack, bodyLen;
    size_t pos, end;

    if (len < 8)
        return;
    magic = frame[0];
    version = frame[1];
    ack = get16(frame + 2);
    bodyLen = get16(frame + 4);
    if (magic != FRAME_MAGIC || version != PROTOCOL_VERSION || 8 + bodyLen > len)
    {
        say("a frame ignored (magic %02x version %u, %u bytes)", (unsigned)magic,
            (unsigned)version, (unsigned)len);
        return;
    }
    /* What it has of ours: gone, and a waiting heartbeat with it. */
    while (c->outCount && seq_before(c->out[c->outHead].seq, ack))
    {
        uint16_t goneSeq = c->out[c->outHead].seq;

        c->outHead = (c->outHead + 1) % OUT_MAX;
        c->outCount--;
        c->overflowed = false;
        if (goneSeq == c->heartSeq)
            c->heartSeq = -1;
    }

    pos = 8;
    end = 8 + bodyLen;
    while (pos + MSG_HDR <= end)
    {
        uint16_t seq = get16(frame + pos);
        uint8_t mtype = frame[pos + 2];
        uint16_t mlen = get16(frame + pos + 4);

        pos += MSG_HDR;
        if (pos + mlen > end)
            break;
        if (!c->ackDue)
            c->ackDueMs = now;
        c->ackDue = true;
        if (seq == c->expect)
        {
            c->expect++;
            on_message(c, mtype, frame + pos, mlen);
        }
        /* Older: it had it, its ack was lost. Newer: one before it was
         * lost, and both come again. */
        pos += mlen;
    }
    c->lastHeard = now;
    flush_node(c, now);
}

static void on_datagram(const struct sockaddr_in *src, const uint8_t *data, size_t len)
{
    uint16_t sender;

    if (len < 3)
        return;
    sender = get16(data);
    /* A beacon (a 3DS's own, or another bridge's): we are not looking. */
    if (len >= BEACON_SIZE && memcmp(data + 8, kMagic, 4) == 0)
        return;
    if (!b.haveChild && sender == CLIENT_ID)
    {
        memset(&b.child, 0, sizeof(b.child));
        b.child.addr = *src;
        b.child.id = CLIENT_ID; /* a session of two: the bridge hosts as 1 */
        b.child.heartSeq = -1;
        b.child.lastHeard = millis();
        b.haveChild = true;
        b.advertDue = 0;
        b.heartbeatDue = 0;
        {
            char ip[16];

            inet_ntop(AF_INET, &src->sin_addr, ip, sizeof(ip));
            say("a player joined from %s:%u", ip, (unsigned)ntohs(src->sin_port));
        }
        queue_advert(&b.child);
    }
    if (b.haveChild && src->sin_addr.s_addr == b.child.addr.sin_addr.s_addr
     && src->sin_port == b.child.addr.sin_port && sender == b.child.id)
        on_frame(&b.child, data + 2, len - 2, millis());
}

/* ---- The session along ------------------------------------------------------ */

static void tick(uint32_t now)
{
    if (b.sock < 0)
        return;
    if (now - b.beaconDue >= BEACON_MS)
    {
        b.beaconDue = now;
        send_beacon();
    }
    if (!b.haveChild)
        return;
    if (now - b.advertDue >= ADVERT_MS)
    {
        b.advertDue = now;
        queue_advert(&b.child);
    }
    /* A host's frame with nothing in it, that the link stays alive: the
     * next only when the last was answered, as the adapter sends its own. */
    if (b.child.heartSeq < 0 && now - b.heartbeatDue >= HEARTBEAT_MS)
    {
        uint8_t beat[2] = {PEER_DATA, 0};

        b.heartbeatDue = now;
        b.child.heartSeq = b.child.nextSeq;
        queue(&b.child, MSG_DATA, beat, sizeof(beat));
    }
    flush_node(&b.child, now);
    if (now - b.child.lastHeard > SILENT_MS)
    {
        say("the player is silent: it is gone");
        b.haveChild = false;
    }
}

/* ---- Life -------------------------------------------------------------------- */

void bridge_init(uint8_t kind, bool verbose, bool loopback)
{
    memset(&b, 0, sizeof(b));
    b.kind = kind;
    b.verbose = verbose;
    b.loopback = loopback;
    b.sock = -1;
#ifdef ESP_PLATFORM
    b.token = esp_random();
#else
    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&b);
    b.token = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
#endif
}

int bridge_bind(void)
{
    struct sockaddr_in addr;
    int on = 1;
    struct timeval tv = {0, 20000}; /* the loop's wait: reads and ticks */

    b.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (b.sock < 0)
        return -1;
    setsockopt(b.sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(b.sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(b.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT_BRIDGE);
    if (bind(b.sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(b.sock);
        b.sock = -1;
        return -1;
    }
    memset(&b.to3ds, 0, sizeof(b.to3ds));
    b.to3ds.sin_family = AF_INET;
    b.to3ds.sin_port = htons(PORT_3DS);
    b.to3ds.sin_addr.s_addr = INADDR_BROADCAST;
    b.toLoop = b.to3ds;
    b.toLoop.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    say("the bridge is up: kind %u, port %u, beacons to %u%s", (unsigned)b.kind,
        (unsigned)PORT_BRIDGE, (unsigned)PORT_3DS, b.loopback ? ", loopback too" : "");
    return 0;
}

void bridge_run_loop(void)
{
    static uint8_t buf[2048];

    for (;;)
    {
        struct sockaddr_in src;
        socklen_t srcLen = sizeof(src);
        int got = recvfrom(b.sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srcLen);

        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            say("recvfrom: errno %d", errno);
        if (got > 0)
            on_datagram(&src, buf, (size_t)got);
        tick(millis());
    }
}
