# The FRLG bridge: the leg toward the Switch's GBA games

A Pokémon Emerald build of this port can trade with FireRed and LeafGreen
the way a Wireless Adapter does — the games speak the same wireless
protocol (the GBA's RFU, carried by `3ds_rfu_adapter.c`); only the radio
under it differs. The 3DS speaks its local wireless (UDS), the Switch's
official GBA app speaks the console's own local wireless (LDN, behind Pia),
and neither radio hears the other. A bridge sits between them: to the 3DS
it is the session's host over a plain UDP socket; to the Switch it is a
standard LDN network the console joins to trade (the console-as-joiner
flow is the reliable one; the Switch cannot join a hidden network the 3DS
would host, and the 3DS cannot host LDN).

Today this directory holds the 3DS-side harness only: `bridge.py` hosts a
session a build of `3ds_port/src/3ds_local.c` with the wire in can find
(the merged build finds it by itself once alone), and speaks the adapter's
side of a trade. The Switch leg (a small board hosting the LDN
network, relaying to this one) is its own program and comes later; the wire
between the two legs is the adapter's message stream, and the board
carries these very bytes.

## The build

The usual build (`make`, in `3ds_port/`; `LOCAL_LINK_BACKEND ?= both`) has
both radios: a session is hosted on the air (UDS, consoles nearby as
always), a search hears the air and then the wire, and a lone host moves to
a bridge it hears. `make LOCAL_LINK_BACKEND=udp` builds the wire alone —
beacons instead of scans, datagrams instead of the bind's frames, no play
with unmodified consoles — which is the test build until the merged one has
been through a real 3DS. Everything else — the message streams, the
counters, the adapter — is the same file, `3ds_port/src/3ds_local.c`.

On a development machine without a 3DS, the whole layer is checked by

    cd 3ds_port && make verify-local-udp PYTHON=python3

which builds `3ds_local.c` for the host (libctru doubled in
`tests/local_host/`, the wire-only build, the air's builds syntax-checked)
and runs `tests/local_udp_test.c` against this bridge on loopback: the
join, the advert, the PEER_* exchange, a burst long enough to split over
frames, and the leave.

## Usage

    python3 bridge.py --kind 2 --verbose

`--kind` is the counter the session is for (the game meets a session only
at its own counter): 1 Union Room, 2 Direct Corner (the trades), 3 Game
Corner, 4 Berry Blender, 5 Contest, 6 Battle Tower. `--loopback` also
beacons to 127.0.0.1, for tests on one machine. The bridge gives up on a
player silent for 6 s and looks for a new one; Ctrl-C ends it.

The 3DS runs a build with the wire in it (`both`, or `udp` for the test)
and starts a session at a counter; the bridge's beacons (every 500 ms, to
port 58325) are what its search finds there. It joins as player 2, the
bridge hosts as player 1, and the game plays as at any counter. In the
merged build a lone host at the counter finds the bridge by itself: it
leaves its empty session and joins (see MergeAlone in `3ds_local.c`), so
the bridge can come up before or after the game does.

## The wire

All fields little-endian. Two datagrams, told apart by what they carry.

Beacon (52 bytes) — a host's voice, UDP port 58325:

    <HHBBH>  senderId (1), flags, players (host counts itself),
             maxPlayers (5), 0
    <4sBB2sI32s>  "EM3L", protocol version (1), kind, 0, session token,
             build id (32B; the bridge's is all zero)

Flags bit 0: a bridge — its build id is not held against it, so any build
of the game joins.

Data (a frame behind its sender's id) — between players:

    <H>      senderId
    <BBHHH>  frame header: magic 0xE3, version, ack (what I have of yours:
             every message I hold with a smaller sequence is mine now),
             bodyLen, 0
    messages, padded with zeros to a multiple of 4

A message:

    <HBBH>   seq, type, 0, len; then len bytes

Type 1 is data: the adapter's own message, between the two games. The
adapter's protocol (`3ds_rfu_adapter.c`, RFU's session protocol):

    1  CONNECT  (no payload) — the child asks to join the host's game
    2  ACCEPT   <u8 slot> — the host answers
    3  REJECT
    4  DATA     <u8 count> + the frame of link data — the game's bytes
    5  DISCONNECT

Type 2 is the advert, what a searching game sees of a host:
`<hosting u8> <nextSlot u8> <hasAdvert u8>` + the 24 bytes of the Wireless
Adapter's broadcast data. A host sends it on join and again every second,
that a game's list of hosts keeps it (the adapter forgets a host 4 s after
its last word).

Between two players the messages ride the go-back-N streams of
`3ds_local.c`: numbered, answered by the ack in every frame's header, sent
again (all of them, from the oldest) 100 ms after the oldest went
unanswered; an ack alone rides out 30 ms after a frame with nothing else
to send. A frame carries at most 1400 bytes, a message at most 168.

## The board, when it comes

The ESP32-class board runs two halves: this one, toward the 3DS (a beacon,
and the streams of one player); and an LDN half, toward the Switch — an
advertised network the console joins at the Trade Center. Between the
halves, the adapter's messages go as they are. What the board must make of
the game side — the advert the Switch's game shows its leader, the
CONNECT the console sends — is the FRLG side of the same protocol, and is
worked out in `docs/frlg-switch-compatibility/`.
