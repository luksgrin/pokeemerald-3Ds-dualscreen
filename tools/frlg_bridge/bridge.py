#!/usr/bin/env python3
"""
The bridge's leg toward a 3DS running the UDP build of local wireless play
(3ds_port/src/3ds_local.c, LOCAL_LINK_UDP).

It hosts a session the way a UDS network would: beacons every 500 ms (the
same AppData a 3DS's beacon carries, before it a sender's id, flags and the
count of players), the adapter's own messages on top of the message streams
(3ds_rfu_adapter.c: a PEER_CONNECT is answered with a PEER_ACCEPT, a
PEER_DATA comes back as one), and the go-back-N streams 3ds_local.c keeps
between two players.

The wire (3ds_port/src/3ds_local.c, little-endian):

    beacon   52 bytes  <HHBBH> + <4sBB2sI32s>
                     senderId, flags, players, maxPlayers, 0;
                     "EM3L", version, kind, 0, token, build id (32B)
    data     <H> senderId, then frames:
    frame    <BBHHH>  magic 0xE3, version, ack (what I have of yours),
                     bodyLen, 0; messages; padded to a multiple of 4
    message  <HBBH>   seq, type, 0, len; data
                     type 1: the adapter's message (PEER_* below)
                     type 2: the advert [hosting][nextSlot][hasAdvert][24B]

Today it is the harness for the first build: a 3DS (or the host test,
tests/local_udp_test.py) finds this bridge on the network and meets it at
the Direct Corner. The board by the Switch takes this program's place, and
speaks the Switch's side of the bridge besides.

Usage:
    python3 bridge.py                       # beacons on the local network
    python3 bridge.py --kind 2 --verbose    # the Direct Corner, chatty
    python3 bridge.py --loopback            # for a test on one machine
"""

import argparse
import random
import socket
import struct
import sys
import threading
import time

PORT_3DS = 58325    # where beacons go, and where a 3DS's socket is bound
PORT_BRIDGE = 58326  # where this bridge's socket is bound (its beacons leave from here)

FRAME_HDR = struct.Struct("<BBHHH")   # magic 0xE3, version, ack, bodyLen, reserved
MSG_HDR = struct.Struct("<HBBH")      # seq, type, 0, len
BEACON_HDR = struct.Struct("<HHBBH")  # senderId, flags, players, maxPlayers, reserved
APPDATA = struct.Struct("<4sBB2sI32s")  # "EM3L", version, kind, reserved, token, build id

MAGIC = b"EM3L"
PROTOCOL_VERSION = 1
FLAG_BRIDGE = 1

MSG_DATA = 1
MSG_ADVERT = 2

PEER_CONNECT = 1
PEER_ACCEPT = 2
PEER_REJECT = 3
PEER_DATA = 4
PEER_DISCONNECT = 5

FRAME_MAGIC = 0xE3
FRAME_MAX = 1400   # a frame's body at most (3ds_local.c's FRAME_MAX)
MSG_MAX = 168
RESEND_MS = 100
ACK_DELAY_MS = 30
FRAMES_A_FLUSH = 4  # FlushNode's budget per pass

BEACON_MS = 500
ADVERT_MS = 1000
HEARTBEAT_MS = 100
SILENT_MS = 6000

LOCAL_ADVERT_SIZE = 24
HOST_ID = 1
CLIENT_ID = 2


def now_ms():
    return int(time.monotonic() * 1000)


def seq_before(a, b):
    """SeqBefore (3ds_local.c): a is behind b, in wraparound."""
    return ((a - b) & 0xFFFF) >= 0x8000


class Client:
    """One 3DS: its address, our message stream to it, and its stream to us."""

    def __init__(self, addr):
        self.addr = addr
        self.id = CLIENT_ID  # a session of two: the bridge hosts as 1
        # Its messages to us.
        self.expect = 0
        self.ackDue = False
        self.ackDueMs = 0
        # Ours to it: [seq, type, data, sentMs, firstMs].
        self.out = []
        self.nextSeq = 0
        self.overflowed = False
        self.lastHeard = now_ms()
        # The adapter protocol: it asked, we answered, and its next heartbeat
        # may go once this one's seq comes back in its ack.
        self.accepted = False
        self.slot = 0
        self.heartSeq = None
        self.dataFramesIn = 0


class Bridge:
    def __init__(self, kind=2, loopback=False, verbose=False):
        self.kind = kind
        self.loopback = loopback
        self.verbose = verbose
        self.token = random.getrandbits(32)
        self.child = None
        self.beaconDue = 0
        self.advertDue = 0
        self.heartbeatDue = 0
        self.childAdvert = bytes(LOCAL_ADVERT_SIZE)
        self.stop = threading.Event()
        self.sock = None

    def say(self, *args):
        if self.verbose:
            print("[bridge %7d] %s" % (now_ms() % 10000000,
                                       " ".join(str(a) for a in args)), flush=True)

    # ---- The beacon --------------------------------------------------------

    def sendBeacon(self, now):
        players = 1 + (1 if self.child else 0)
        packet = BEACON_HDR.pack(HOST_ID, FLAG_BRIDGE, players, 5, 0)
        packet += APPDATA.pack(MAGIC, PROTOCOL_VERSION, self.kind,
                               b"\x00\x00", self.token, bytes(32))
        for target in (("255.255.255.255", PORT_3DS),) + ((("127.0.0.1", PORT_3DS),) if self.loopback else ()):
            try:
                self.sock.sendto(packet, target)
            except OSError as e:
                self.say("beacon to %s failed: %s" % (target[0], e))

    # ---- Our messages to the client: the go-back-N stream ------------------

    def queue(self, client, mtype, data):
        if len(data) > MSG_MAX:
            data = data[:MSG_MAX]
        if len(client.out) >= 128:
            if not client.overflowed:
                self.say("queue to %u full" % client.id)
            client.overflowed = True
            return
        client.out.append([client.nextSeq, mtype, data, 0, 0])
        client.nextSeq = (client.nextSeq + 1) & 0xFFFF

    def queueAdvert(self, client):
        hosting, nextSlot = 1, (1 if client.accepted else 0)
        self.queue(client, MSG_ADVERT,
                   bytes([hosting, nextSlot, 1]) + self.childAdvert)

    def frame(self, client, body):
        """One datagram: our name, the frame header (its ack inside), the
        messages, and padding to a multiple of 4."""
        size = FRAME_HDR.size + len(body)
        padded = (size + 3) & ~3
        out = bytes([HOST_ID, 0])
        out += FRAME_HDR.pack(FRAME_MAGIC, PROTOCOL_VERSION, client.expect,
                              len(body), 0)
        out += body
        out += bytes(padded - size)
        return out

    def flush(self, client, now):
        """What's due to the client now, as FlushNode sends it: messages not
        sent yet; when the oldest has waited too long, everything from it
        again; an ack alone if nothing else goes for a while."""
        resend = bool(client.out) and client.out[0][3] \
            and now - client.out[0][3] >= RESEND_MS
        frames = 0
        i = 0
        while frames < FRAMES_A_FLUSH:
            body = bytearray()
            while i < len(client.out):
                m = client.out[i]
                if m[3] and not resend:
                    i += 1
                    continue
                if len(body) + MSG_HDR.size + len(m[2]) > FRAME_MAX:
                    break
                body += MSG_HDR.pack(m[0], m[1], 0, len(m[2])) + m[2]
                m[3] = now
                if not m[4]:
                    m[4] = now
                i += 1
            if not body and not (client.ackDue
                                 and now - client.ackDueMs >= ACK_DELAY_MS):
                break
            self.sock.sendto(self.frame(client, body), client.addr)
            client.ackDue = False
            frames += 1
            if i >= len(client.out):
                break

    # ---- The client's messages ---------------------------------------------

    def onDatagram(self, src, data):
        if len(data) < 3:
            return
        sender = data[0] | (data[1] << 8)
        # A beacon (a 3DS's own, or another bridge's): we are not looking.
        if len(data) >= BEACON_HDR.size + APPDATA.size and data[8:12] == MAGIC:
            return
        if self.child is None and sender == CLIENT_ID:
            self.child = Client(src)
            self.advertDue = 0
            self.heartbeatDue = 0
            self.say("a player joined from %s:%u" % (src[0], src[1]))
            self.queueAdvert(self.child)
        if self.child and src == self.child.addr and sender == self.child.id:
            self.onFrame(self.child, data[2:], now_ms())

    def onFrame(self, client, frame, now):
        if len(frame) < FRAME_HDR.size:
            return
        magic, version, ack, bodyLen, _zero = FRAME_HDR.unpack_from(frame, 0)
        if magic != FRAME_MAGIC or version != PROTOCOL_VERSION \
                or FRAME_HDR.size + bodyLen > len(frame):
            self.say("a frame ignored (magic %02x version %u, %u bytes)",
                     magic, version, len(frame))
            return
        # What it has of ours: gone, and a waiting heartbeat with it.
        while client.out and seq_before(client.out[0][0], ack):
            m = client.out.pop(0)
            client.overflowed = False
            if m[0] == client.heartSeq:
                client.heartSeq = None

        pos = FRAME_HDR.size
        end = FRAME_HDR.size + bodyLen
        while pos + MSG_HDR.size <= end:
            seq, mtype, _zero, mlen = MSG_HDR.unpack_from(frame, pos)
            pos += MSG_HDR.size
            if pos + mlen > end:
                break
            if not client.ackDue:
                client.ackDueMs = now
            client.ackDue = True
            if seq == client.expect:
                client.expect = (client.expect + 1) & 0xFFFF
                self.onMessage(client, mtype, frame[pos:pos + mlen])
            # Older: it had it, its ack was lost. Newer: one before it was
            # lost, and both come again.
            pos += mlen
        client.lastHeard = now
        self.flush(client, now)

    def onMessage(self, client, mtype, data):
        if mtype == MSG_ADVERT:
            if len(data) >= 3 + LOCAL_ADVERT_SIZE and data[2] \
                    and any(data[3:3 + LOCAL_ADVERT_SIZE]):
                self.childAdvert = bytes(data[3:3 + LOCAL_ADVERT_SIZE])
                self.say("the player's broadcast: %s" % self.childAdvert.hex())
        elif mtype == MSG_DATA:
            self.onPeer(client, data)

    # ---- The adapter's protocol (3ds_rfu_adapter.c) -------------------------

    def onPeer(self, client, msg):
        if not msg:
            return
        kind = msg[0]
        if kind == PEER_CONNECT:
            if not client.accepted:
                client.accepted = True
                client.slot = 0
                self.say("PEER_CONNECT: accepted as slot %u" % client.slot)
            self.queue(client, MSG_DATA, bytes([PEER_ACCEPT, client.slot]))
        elif kind == PEER_ACCEPT:
            # Only a child of a host sends that; we host.
            self.say("PEER_ACCEPT? we are the host")
        elif kind == PEER_REJECT:
            self.say("PEER_REJECT")
        elif kind == PEER_DATA:
            if len(msg) < 2:
                return
            # The harness stands for the game on the bridge's other side:
            # its data comes back as ours. The board by the Switch relays it
            # there instead.
            client.dataFramesIn += 1
            self.queue(client, MSG_DATA, bytes([PEER_DATA]) + msg[1:])
        elif kind == PEER_DISCONNECT:
            self.say("PEER_DISCONNECT")
            self.child = None

    # ---- The socket ----------------------------------------------------------

    def bind(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("0.0.0.0", PORT_BRIDGE))
        self.sock.settimeout(0.02)

    def run(self, stop=None, durationMs=0):
        stop = stop or self.stop
        if self.sock is None:
            self.bind()
        self.say("the bridge is up: kind %u, port %u, beacons to %u%s"
                 % (self.kind, PORT_BRIDGE, PORT_3DS,
                    ", loopback too" if self.loopback else ""))
        start = now_ms()
        try:
            while not stop.is_set():
                now = now_ms()
                if durationMs and now - start > durationMs:
                    break
                try:
                    data, src = self.sock.recvfrom(2048)
                except socket.timeout:
                    data = None
                except OSError as e:
                    self.say("recvfrom: %s" % e)
                    data = None
                if data:
                    self.onDatagram(src, data)
                self.tick(now_ms())
        finally:
            self.say("the bridge is down")

    def tick(self, now):
        if self.sock is None:
            return
        if now - self.beaconDue >= BEACON_MS:
            self.beaconDue = now
            self.sendBeacon(now)
        if self.child is None:
            return
        client = self.child
        # The advert, that the player keeps seeing a host.
        if now - self.advertDue >= ADVERT_MS:
            self.advertDue = now
            self.queueAdvert(client)
        # A host's frame with nothing in it, that the link stays alive: the
        # next only when the last was answered, as the adapter sends its own.
        if client.heartSeq is None and now - self.heartbeatDue >= HEARTBEAT_MS:
            self.heartbeatDue = now
            seq = client.nextSeq
            self.queue(client, MSG_DATA, bytes([PEER_DATA, 0]))
            client.heartSeq = seq
        self.flush(client, now)
        if now - client.lastHeard > SILENT_MS:
            self.say("the player is silent: it is gone")
            self.child = None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kind", type=int, default=2,
                        help="the counter the session is for (2: the Direct Corner)")
    parser.add_argument("--loopback", action="store_true",
                        help="beacons to 127.0.0.1 as well, for a test on one machine")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--time", type=float, default=0,
                        help="give up after this many seconds (0: never)")
    args = parser.parse_args(argv)

    bridge = Bridge(kind=args.kind, loopback=args.loopback, verbose=args.verbose)
    try:
        bridge.run(durationMs=int(args.time * 1000))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
