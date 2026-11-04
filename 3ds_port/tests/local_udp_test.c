/*
 * Host test for the bridge leg of local wireless play: src/3ds_local.c built
 * with LOCAL_LINK_UDP joins a session whose host is tools/frlg_bridge on
 * loopback, exchanges the adapter's PEER_* messages with it, and leaves.
 * The runner is tests/local_udp_test.py (it starts the bridge first).
 *
 * The bridge's side of the wire is the game's own adapter protocol
 * (src/3ds_rfu_adapter.c): a PEER_CONNECT is answered with a PEER_ACCEPT,
 * and a PEER_DATA comes back as one. The game's side here is the adapter's
 * own calls, straight to the local link.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "3ds.h"
#include "3ds_local.h"
#include "3ds_log.h"
#include "3ds_platform.h"
#include "local_host/constants_stub.h"

static int sFailures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++sFailures; } } while (0)

/* ---- The port's own services, as the test needs them ---------------------- */

void CtrLog_Write(CtrLogCategory category, const char *format, ...)
{
    va_list args;

    printf("[%d] ", (int)category);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

bool CtrLog_Enabled(void) { return false; }

FILE *CtrFs_OpenAsset(const char *relativePath)
{
    (void)relativePath;
    return NULL; /* no build id: the bridge leg joins any build's bridge */
}

/* ---- The test -------------------------------------------------------------- */

/* The adapter's PEER_* (3ds_rfu_adapter.c). */
enum
{
    PEER_CONNECT = 1,
    PEER_ACCEPT = 2,
    PEER_DATA = 4,
    PEER_DISCONNECT = 5,
};

#define HOST_ID 1

static bool WaitSessionIn(int ms)
{
    for (int waited = 0; waited < ms; waited += 20)
    {
        if (LocalLink_GetSession() == LOCAL_SESSION_IN)
            return true;
        svcSleepThread(20 * 1000000LL);
    }
    return false;
}

static bool WaitEvent(struct LocalEvent *ev, int ms)
{
    for (int waited = 0; waited < ms; waited += 5)
    {
        if (LocalLink_PollEvent(ev))
            return true;
        svcSleepThread(5 * 1000000LL);
    }
    return false;
}

int main(void)
{
    struct LocalPeer peers[LOCAL_MAX_PEERS];
    struct LocalEvent ev;
    u8 connect[1] = { PEER_CONNECT };
    u8 data[3] = { PEER_DATA, 1, 0xAA };
    int accepted = 0, echoed = 0, burstsIn = 0;

    LocalLink_Init();
    CHECK(LocalLink_Enabled());
    if (!LocalLink_Enabled())
        return 1;

    LocalLink_StartSession(LOCAL_KIND_DIRECT_CORNER);
    if (!WaitSessionIn(15000))
    {
        printf("FAIL: no session came (state %u)\n", LocalLink_GetSession());
        return 1;
    }
    printf("session in, my id %u\n", LocalLink_MyId());
    CHECK(LocalLink_MyId() == 2); /* a session of two: the bridge is 1 */

    /* The bridge's advert says it is hosting. */
    for (int waited = 0; waited < 3000; waited += 20)
    {
        if (LocalLink_GetPeers(peers, LOCAL_MAX_PEERS) >= 1 && peers[0].hasAdvert && peers[0].hosting)
            break;
        svcSleepThread(20 * 1000000LL);
    }
    CHECK(LocalLink_GetPeers(peers, LOCAL_MAX_PEERS) == 1);
    CHECK(peers[0].id == HOST_ID);
    CHECK(peers[0].hasAdvert);
    CHECK(peers[0].hosting);
    if (sFailures)
        return 1;

    /* Connect to it, as the adapter does at a counter. */
    LocalLink_Send(HOST_ID, connect, sizeof(connect));
    while (WaitEvent(&ev, 2000))
    {
        if (ev.type == LOCAL_EV_RECV && ev.id == HOST_ID && ev.len >= 2 && ev.data[0] == PEER_ACCEPT)
        {
            printf("accepted, slot %u\n", ev.data[1]);
            accepted = 1;
            break;
        }
    }
    CHECK(accepted);
    if (!accepted)
        return 1;

    /* A frame of link data comes back the way it went. */
    LocalLink_Send(HOST_ID, data, sizeof(data));
    while (WaitEvent(&ev, 2000))
    {
        if (ev.type == LOCAL_EV_RECV && ev.id == HOST_ID && ev.len >= 3 && ev.data[0] == PEER_DATA
         && ev.data[1] == 1 && ev.data[2] == 0xAA)
        {
            echoed = 1;
            break;
        }
    }
    CHECK(echoed);

    /* A burst: more messages than one frame carries, to make the streams
     * queue, split and acknowledge. */
    for (int i = 0; i < 30; i++)
    {
        u8 frame[40];

        memset(frame, 0, sizeof(frame));
        frame[0] = PEER_DATA;
        frame[1] = 38;
        frame[2] = (u8)i;
        LocalLink_Send(HOST_ID, frame, sizeof(frame));
    }
    {
        u32 deadline = LocalLink_Milliseconds() + 5000;

        while (burstsIn < 30 && LocalLink_Milliseconds() < deadline)
        {
            if (!WaitEvent(&ev, 20))
                continue;
            if (ev.type == LOCAL_EV_RECV && ev.id == HOST_ID && ev.len >= 3 && ev.data[0] == PEER_DATA
             && ev.data[1] == 38)
                burstsIn++;
        }
        CHECK(burstsIn == 30);
    }

    /* And the session is left clean. */
    LocalLink_LeaveSession();
    for (int waited = 0; waited < 2000; waited += 20)
    {
        if (LocalLink_GetSession() == LOCAL_SESSION_OFF || LocalLink_GetSession() == LOCAL_SESSION_NONE)
            break;
        svcSleepThread(20 * 1000000LL);
    }
    CHECK(LocalLink_GetSession() != LOCAL_SESSION_IN);

    LocalLink_Shutdown();
    printf(sFailures ? "LOCAL UDP TEST: %d FAILURES\n" : "LOCAL UDP TEST: PASS\n", sFailures);
    return sFailures != 0;
}
