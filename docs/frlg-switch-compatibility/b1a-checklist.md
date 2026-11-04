# B1a — the 3DS leg, the Mac as bridge

The first run of the trade flow on real hardware: a real 3DS running the
merged build meets `bridge.py` on the Mac, over the home Wi-Fi. No board,
no Switch — the harness stands for the other game. Pass, and every
3DS-side unknown is retired: whatever fails afterward is the board's to
answer for.

## Before

- **The build** (`both`, the Makefile default), wherever devkitARM lives
  (the Windows box): `make` in `3ds_port/`, then the `.3dsx` to the 3DS's
  SD card, started from the homebrew launcher.
- **The Mac and the 3DS on the same Wi-Fi** (the Archer C9 as a plain
  access point is fine — this leg is ordinary UDP broadcast on port 58325).
- **The bridge** on the Mac, from the repo root:

      python3 tools/frlg_bridge/bridge.py --kind 2 --verbose

  `--kind 2` is the Direct Corner, the trades.

## The flow

1. **Joiner first.** With the bridge already running, start the game on
   the 3DS and enter the Direct Corner. Within a few seconds the Mac
   prints `a player joined from …`, the game joins as player 2, and the
   trade UI moves past its search — the room, the other player, the
   counter.
2. **Then the migration.** Restart the game's session first — enter the
   Direct Corner with *no* bridge running (it hosts alone on the air) —
   and start `bridge.py` only afterward. Within about 1.5–3 s the game
   leaves its empty session and joins the bridge's; the Mac's log shows
   the join, the game's UI follows. This is the merged build's
   `MergeAlone` doing its work: the bridge may come up before or after
   the game does.
3. **The exchange.** Let it sit at the counter for a minute. The Mac's
   log carries the adapter's traffic (CONNECT→ACCEPT, heartbeats); the
   game's screen stays in the room, no timeout, no drop. Then leave the
   counter cleanly — the Mac prints the leave, and re-entering finds the
   bridge again.

## If it fails

- **Nothing on the Mac at all** (the game searches forever, the log is
  silent): the access point is keeping its clients apart (client
  isolation) or dropping broadcast. Rehearse B1b instead — the phone's
  hotspot, which is the board's topology anyway.
- **The join, then silence:** the game's screen stuck at the search or
  the door — check that no other bridge is running (`--loopback` of an
  old test, a second Python), and that the Mac's firewall allows Python
  to receive on 58326.
- **A drop after 6 s:** that is the bridge's silence timer expiring —
  the game's datagrams stopped reaching the Mac. Same suspects as above.

Record the run in `experiments.md` (what build, what radio path, times).
