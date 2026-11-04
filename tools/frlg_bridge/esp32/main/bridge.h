/*
 * The bridge's leg toward a 3DS running the UDP build of local wireless
 * play (3ds_port/src/3ds_local.c, LOCAL_LINK_UDP): a C port of
 * tools/frlg_bridge/bridge.py, byte for byte of its wire.
 *
 * It hosts a session the way a UDS network would: beacons every 500 ms,
 * the adapter's own messages on top of the message streams (a
 * PEER_CONNECT is answered with a PEER_ACCEPT, a PEER_DATA comes back as
 * one), and the go-back-N streams between the two.
 *
 * Platform-free on purpose: on the board it runs under ESP-IDF (main.c),
 * and on a development machine it is the loopback test's other end
 * (test/bridge_host.c, run by 3ds_port/tests/local_esp32bridge_test.py).
 * The board's Switch leg (LDN) is its own program, relaying where the
 * harness echoes.
 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* The counter the session is for (the game meets a session only at its
 * own): 1 Union Room, 2 Direct Corner (the trades), 3 Game Corner, 4 Berry
 * Blender, 5 Contest, 6 Battle Tower. */
void bridge_init(uint8_t kind, bool verbose, bool loopback);

/* Our socket, bound: the beacons leave from here, the 3DS answers here.
 * 0 if it worked. */
int bridge_bind(void);

/* The bridge's whole life: reads the socket (20 ms waits), ticks the
 * session along. Never returns. */
void bridge_run_loop(void);

#endif
