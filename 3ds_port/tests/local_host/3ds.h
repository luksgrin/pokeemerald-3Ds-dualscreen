/*
 * Host double for the bridge leg of local wireless play (src/3ds_local.c,
 * built with LOCAL_LINK_UDP): the few libctru pieces the UDP build touches,
 * so the message streams, the beacons and the session state machine run on
 * a development machine — the socket is the real one, and a test speaks the
 * bridge's side of the wire on loopback. See tests/local_udp_test.c and
 * tools/frlg_bridge.
 */
#ifndef LOCAL_HOST_3DS_H
#define LOCAL_HOST_3DS_H

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int32_t s32;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int32_t Result;
typedef int Handle;

#define R_FAILED(res) ((res) < 0)
#define R_SUCCEEDED(res) ((res) >= 0)

/* The tick is microseconds here; the code divides it into milliseconds. */
#define CPU_TICKS_PER_MSEC 1000
#define RESET_ONESHOT 0
#define CUR_THREAD_HANDLE 0

static inline u64 svcGetSystemTick(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}

static inline void svcSleepThread(s64 ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };

    nanosleep(&ts, NULL);
}

/* ---- Locks ---------------------------------------------------------------- */

typedef struct { pthread_mutex_t mutex; } LightLock;

static inline void LightLock_Init(LightLock *lock) { pthread_mutex_init(&lock->mutex, NULL); }
static inline void LightLock_Lock(LightLock *lock) { pthread_mutex_lock(&lock->mutex); }
static inline void LightLock_Unlock(LightLock *lock) { pthread_mutex_unlock(&lock->mutex); }

/* ---- Events: a pipe apiece; a signal is a byte, taken by the wait ---------- */

static struct { Handle read; int write; } sEventPipes[16];

static inline int *EventWriteEnd(Handle read)
{
    for (unsigned i = 0; i < sizeof(sEventPipes) / sizeof(sEventPipes[0]); i++)
        if (sEventPipes[i].read == read)
            return &sEventPipes[i].write;
    for (unsigned i = 0; i < sizeof(sEventPipes) / sizeof(sEventPipes[0]); i++)
        if (sEventPipes[i].write == 0)
        {
            sEventPipes[i].read = read;
            return &sEventPipes[i].write;
        }
    return NULL;
}

static inline Result svcCreateEvent(Handle *event, int mode)
{
    int fds[2];

    (void)mode;
    if (pipe(fds) != 0)
        return -1;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    *event = fds[0];
    *EventWriteEnd(fds[0]) = fds[1];
    return 0;
}

static inline Result svcSignalEvent(Handle event)
{
    u8 one = 1;
    int *end = EventWriteEnd(event);

    return end && write(*end, &one, 1) == 1 ? 0 : -1;
}

static inline Result svcClearEvent(Handle event)
{
    u8 buffer[64];

    while (read(event, buffer, sizeof(buffer)) > 0)
        ;
    return 0;
}

static inline Result svcCloseHandle(Handle event)
{
    int *write = EventWriteEnd(event);

    if (write)
    {
        close(*write);
        *write = 0;
    }
    return close(event);
}

/* Waits for one of the handles (a byte in its pipe), up to the timeout, and
 * takes what it saw: oneshot. *Which is the index of the handle, or -1. */
static inline Result svcWaitSynchronizationN(s32 *which, const Handle *handles, s32 count,
                                             bool waitAll, s64 timeoutNs)
{
    struct pollfd fds[8];
    int ms = (int)((timeoutNs + 999999LL) / 1000000LL);
    int ready;

    (void)waitAll;
    if (count > 8)
        count = 8;
    for (s32 i = 0; i < count; i++)
    {
        fds[i].fd = handles[i];
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    ready = poll(fds, (nfds_t)count, ms);
    for (s32 i = 0; i < count; i++)
    {
        u8 buffer[64];

        while (read(handles[i], buffer, sizeof(buffer)) > 0)
            ;
    }
    *which = -1;
    for (s32 i = 0; i < count && ready > 0; i++)
        if (fds[i].revents & POLLIN)
        {
            *which = i;
            break;
        }
    return ready > 0 ? 0 : -1;
}

/* ---- Threads ---------------------------------------------------------------- */

typedef struct { pthread_t thread; void (*fn)(void *); void *arg; } *Thread;

static inline void svcGetThreadPriority(s32 *priority, int handle)
{
    (void)handle;
    *priority = 0x30;
}

static inline void *HostThread(void *p)
{
    Thread t = p;

    t->fn(t->arg);
    return NULL;
}

static inline Thread threadCreate(void (*fn)(void *), void *arg, size_t stack, int priority,
                                  int core, bool detached)
{
    Thread t = (Thread)malloc(sizeof(*t));

    (void)stack; (void)priority; (void)core; (void)detached;
    if (t == NULL)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->thread, NULL, HostThread, t) != 0)
    {
        free(t);
        return NULL;
    }
    return t;
}

static inline Result threadJoin(Thread t, s64 timeoutNs)
{
    (void)timeoutNs;
    return pthread_join(t->thread, NULL) == 0 ? 0 : -1;
}

static inline void threadFree(Thread t) { free(t); }

/* ---- Sockets are the system's: socInit is a formality ----------------------- */

static inline Result socInit(void *buffer, size_t size) { (void)buffer; (void)size; return 0; }
static inline Result socExit(void) { return 0; }

/* ---- UDS: declarations only, for a syntax check of the builds with the
 * air in them (-fsyntax-only; a host cannot run the system's wireless, and
 * the wire-only build of the test references none of it). ------------------- */

#define UDS_DATAFRAME_MAXSIZE 0x5C6
#define UDS_SENDFLAG_Default 1
#define UDS_BROADCAST_NETWORKNODEID 1
#define UDS_DEFAULT_RECVBUFSIZE 0x3400

typedef enum { UDSCONTYPE_Client = 4 } udsConnectionType;

typedef struct
{
    u32 status;
    u32 cur_NetworkNodeID;
    u32 total_nodes;
    u32 node_bitmask;
} udsConnectionStatus;

typedef struct { u8 total_nodes; u8 max_nodes; } udsNetworkStruct;
typedef struct { udsNetworkStruct network; } udsNetworkScanInfo;
typedef struct { Handle event; } udsBindContext;

Result udsInit(size_t sharedmem_size, const void *extra);
Result udsExit(void);
bool udsWaitConnectionStatusEvent(bool next, bool wait);
Result udsGetConnectionStatus(udsConnectionStatus *status);
Result udsSendTo(u16 nid, u8 channel, u32 flags, const void *data, size_t size);
Result udsPullPacket(const udsBindContext *bind, void *buf, size_t size, size_t *actual_size, u16 *nid);
Result udsScanBeacons(void *buf, size_t size, udsNetworkScanInfo **nets, size_t *max_nets,
                      u32 id, u8 id_offset, const void *filter, bool regex);
Result udsGetNetworkStructApplicationData(const udsNetworkStruct *net, void *data, size_t size,
                                          size_t *actual_size);
Result udsConnectNetwork(const udsNetworkStruct *net, const char *passphrase, size_t passphrase_size,
                         udsBindContext *bind, u16 nid, udsConnectionType type, u8 channel,
                         u32 recvbufsize);
Result udsCreateNetwork(const udsNetworkStruct *net, const char *passphrase, size_t passphrase_size,
                        udsBindContext *bind, u8 channel, u32 recvbufsize);
Result udsDestroyNetwork(void);
Result udsDisconnectNetwork(void);
Result udsUnbind(udsBindContext *bind);
Result udsEjectClient(u16 nid);
Result udsSetApplicationData(const void *data, u32 size);
Result udsGenerateDefaultNetworkStruct(udsNetworkStruct *net, u32 id, u8 id_offset, u8 max_nodes);

#endif
