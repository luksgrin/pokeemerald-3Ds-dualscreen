# Experiments log — Emerald-3DS ↔ FRLG (Switch)

Companion to `research-design.md` (Appendix A = "Option 0"). Fill in results as you go.
Everything here is recon; no code changes are gated on anything except G1–G3 passing.

## G1 — does the FRLG console host a visible LDN network in trade-Leader mode?

**Cost: ~30 min. Hardware: Switch (any FW) + Mac/phone. No CFW, no keys, no 3DS needed yet.**

1. Switch: open the Game Boy NSO app → FireRed/LeafGreen (save must have the Direct
   Corner / wireless counter unlocked).
2. In-game: Pokémon Center counter (wireless attendants) → choose **Trade** →
   **Leader** ("become the Leader and wait"). Leave it sitting on the waiting screen.
3. Mac: open the Wi‑Fi picker and look for a network whose SSID is a **32-character hex
   string** (e.g. `a3f...0c4`, no spaces). Also try an iPhone/Android Wi‑Fi list —
   phones show hidden/oddball SSIDs more eagerly.

Record:
- [x] Network visible? **N — trade Leader mode at the Direct Corner counter produced no
      32-hex SSID on macOS (2026-11-04). Not yet conclusive: see follow-ups below.**
- [ ] SSID (copy it exactly — we need the exact 32 hex chars including case)
- [ ] Appears closed/locked?
- [ ] Signal/channel if shown
- [ ] **Re-host test:** back out of the room and re-enter Leader mode again — is the
      SSID *different*? (This measures the per-session churn empirically.)

**Follow-ups for the "not visible" result:**

1. **Different scanner:** repeat the Leader-wait scan with a *phone* Wi-Fi list
   (Android if available — its scanner is more complete than macOS in several
   edge cases; iPhone as second choice). Give it a full minute.

   **R1 (2026-11-04):** phone scanner saw no 32-hex SSID either. Two possible
   readings: (a) our "SSID = 32 hex chars" heuristic may simply be wrong for the GBA
   app — it is documented for Pia 6.16–6.41 (hex of the session id), but this app is
   a much newer Pia (header version 15/16) and could name its network anything, e.g.
   the Switch profile name or a game-specific string; (b) the SSID may be hidden
   (empty in beacons). Neither is fatal. **Revised test — the toggle-diff:** while
   the Switch actually enters and then leaves the "あと1人必要 / 1 more person
   needed" wait, diff the scanner's full network list each way. *Any* entry that
   exists only while the wait is up is the console's network, whatever it is named
   (hex, profile-name-like, or a blank/hidden placeholder). If Android is available,
   the open-source **WiFiAnalyzer** app is ideal: hidden-SSID networks show as blank
   entries that pop in and out, with channel and RSSI.

2. **Watch the radio state in-game:** when the console actually hosts, FRLG shows the
   wireless status/signal indicator (the "approach the wireless icon" screen). If the
   Leader-wait screen shows a *searching* indicator and never a hosting/signal one,
   the console is a station, not an AP — i.e. trade-Leader mode never calls
   CreateNetwork. That is a *design-relevant* finding, distinct from "the network is
   there but invisible".
3. **Test the documented console-host mode: Mystery Gift.** Exit, go to the title
   screen's **Mystery Gift** menu → **Wonder Cards** → **Friend** → wait on the
   search screen. Consoles have been *observed hosting* LDN in this exact mode ("a
   console hosting Mystery Gift answers a console child with type 5 within 50–66 ms",
   pokeldn frlg_link). Scan during the wait. If the network appears here but not in
   trade-Leader, the design survives with the role-mapping flipped (Emerald joins the
   MG-style session, or we find which trade activity actually hosts).
4. **Hidden-SSID possibility:** if a phone sees a "hidden network"/"other network"
   entry while macOS shows nothing, try macOS System Settings → Wi-Fi → **Other
   network**, and enter the SSID by hand from the phone's readout.

**R3 (2026-11-04) — the console DOES host: a hidden-SSID network**

While the Switch sat at `あと1人必要` (Direct Corner trade Leader), WiFiAnalyzer on
Android showed a network entry whose SSID field is **empty — labelled "*hidden*", with
a visible BSSID (MAC)**. No name of any kind was visible to the phone.

Reading: very plausibly the console's LDN network, using an **empty SSID field in its
beacons** (or an SSID the phone cannot display). The joinable identity, per Nintendo's
design, travels only in the LDN *advertisement action frames* (which carry
`NetworkId.SessionId`; joiners derive the real SSID from that — standard client tools
cannot decode or display them).

Impact on Option 0 (3DS as ordinary station joining the console's network):

- The 3DS cannot learn the SSID: homebrew cannot sniff raw frames, the SSID is absent
  from beacons, and manual configuration needs the SSID string. **Unless the SSID
  happens to be *stable* for this app**, a direct 3DS-side join is blocked in
  practice.
- **Tests queued to resolve this:**
  - T1 (toggle-diff): exit/re-enter trade-Leader and confirm the *hidden* entry
    appears/disappears with the game state (rules out a neighbour's network).
  - T2 (identity churn): re-host N times; record whether the **BSSID and channel stay
    the same** across sessions. A stable BSSID raises (does not prove) the odds that
    the SSID is also session-stable for this title; a churning BSSID effectively
    confirms per-session identity rotation.
- If T1/T2 kill the direct-join path, the architecture regresses to the §3 bridge
  ladder — and specifically to the **ESP32-only bridge** (small board, no PC, no
  console mods): the bridge hosts a *visible, stable* LDN network for the trade-joiner
  Switch side (the console-as-joiner flow is the most thoroughly proven path in
  pokeldn's work: Direct Corner → Join Group → named group), and serves the 3DS over
  plain UDP.
**R4 (2026-11-04) — several hidden entries, not just one**

WiFiAnalyzer shows multiple hidden-SSID entries — as expected: neighbours keep hidden
networks constantly. The game's entry must be isolated compositionally, by toggling
the game state and watching which entry follows. Updated T1 procedure:

1. Record the *full* hidden-entries snapshot (BSSID list) with the Switch NOT in the
   wait state (back out of the Wireless Club entirely — the entry that persists with
   a closed game is baseline).
2. Sit the Switch in Broker Leader wait (`あと1人必要`). Take a second snapshot.
3. The **new BSSID that exists only in step 2** is the game's network. Record its
   channel, RSSI, and BSSID. (WiFiAnalyzer list view often flags new entries; leave
   the Switch-wait snapshot up for a couple of minutes before diffing.)
4. Repeat step 1–3 *after closing/backing out of the game entirely* — if the same
   BSSID survives the console powering off, it's not the game's.
5. Then T2 (identity churn): repeat the wait three times; note whether the game's
   BSSID/channel changes between sessions.

**R5 (2026-11-04) — G1 CONFIRMED: the game's network is the hidden, Nintendo-OUI one**

While the console waited at `あと1人必要`, one hidden-SSID entry in WiFiAnalyzer showed
manufacturer "**NINTENDO CO LTD**" — and it **disappeared when the Switch was powered
off**. That resolves the ambiguity of several hidden entries: the game's hosted LDN
network is identified with high confidence.

- **G1 verdict: PASS with a caveat** — the FRLG NSO app (JP build) *does* host an LDN
  network in trade-Leader mode, but the network's SSID is not visible to client
  scanners (empty/hidden SSID in beacons). Per Nintendo's LDN join design, joiners
  obtain the real SSID from the LDN advertisement action frames; no tool the phone or
  Mac can run decodes these.
- Consequence for the architecture (to be confirmed by the churn test): a standard
  client — including the 3DS — cannot complete association without the SSID, and
  pokeldn's beacon research suggests the SSID is derived from the per-session
  `NetworkId.SessionId`. If identity rotates per session (test T2), the
  "3DS joins the console directly" variant is dead for standard clients, and the
  minimal-hardware design must include a tiny bridge (e.g. an ESP32 board):
  the bridge hosts a **visible, stable** LDN network for the Switch (console-as-trade-
  joiner is pokeldn's most thoroughly proven flow), and relays to the 3DS over UDP.
  This loses the strict "two consoles only" constraint, but keeps: no PC required,
  no console mods, and a ~$6 board powered from any USB charger (pokeldn even
  demonstrates fully offline board-hosted sessions for Mystery Gift).
- **Remaining queries:** moved to R6 below; the churn test has since been run.
- Language note: the user additionally has a **Japanese build of the Emerald port**,
  so the JP-FRLG ↔ JP-Emerald pairing stays available; the international-ROM option
  (R2) remains the friendlier target.

**R6 (2026-11-04) — T2 churn result: BSSID stable, channel rotates**

Across re-hosts of the Leader-wait state the game's network kept the **same BSSID**
(Nintendo OUI — consistent with the BSSID being the console's own radio MAC), while
the **channel (and thus the MHz listing) changed between sessions** (RSSI moved with
it, as expected for a moved AP).

Reading: session-level identity is re-randomised per hosting (classic LDN
`CreateNetwork`), consistent with pokeldn's finding that the SSID derives from the
per-session `NetworkId.SessionId`. The BSSID tells us *where* the network is, but we
cannot see *what it is called* — and the name is the credential a standard client
needs.

**Status of the direct-join design ("Option 0")**: parked, pending two probes:
1. Visibility sweep in other hosting modes: Mystery Gift → Wonder Cards → Friend
   wait; Colosseum leader wait. If any mode produces a *visible, named* network, the
   direct-join design revives (the SSID is then readable from beacons; it must also
   be *stable* to be configurable into a 3DS profile).
2. One packet capture (monitor-mode device) that reveals SSID(s) across two
   re-hosts — settling stability definitively.
If both fail, adopt the **ESP32-only bridge** as the primary design: the bridge hosts
a visible, stable network for the Switch (trade-joiner flow, proven by pokeldn) and
relays to the 3DS over UDP.

**R2 (2026-11-04) — new project-level fact: the test game is the Japanese FRLG ROM**

The user's NSO cartridge is the Japanese build (menu state `あと1人必要` = "one
more person needed"). This matters beyond translation:

- **Gen III JP ↔ international play is not possible at the ROM level** — neither over
  link cable nor the Wireless Adapter on original hardware. The international ROMs
  skip Japanese candidates in the Union Room search (`union_room.c:3726`), the link
  groups are language-partitioned, and JP/invalid-language links were refused on real
  GBA-era hardware. So a Japanese-language FireRed/LeafGreen on the Switch cannot
  trade with an international-language Emerald, on the 3DS port or on real GBA
  hardware. This is a game-level gate, not something the bridge can neutralise.
- Implications for the plan:
  - **Preferred:** obtain an international (US/EU) FireRed/LeafGreen build on the
    same Switch — e.g. a second Switch user/eShop account in a different region, or
    changing the Nintendo Account's country setting (subject to its once-a-year
    policy; eShop balance must be zero). The US/EU NSO app bundles the international
    FRLG ROMs, which also matches the user's language preference. pokeldn's protocol
    data was captured on international (French) cartridges with all twelve
    `0x0A`-revision cartridges mapped, so the Switch leg stays as designed.
  - **Alternative:** keep the JP FRLG and run a **Japanese build of the Emerald
    port** on the 3DS (JP Emerald ↔ JP FRLG is hardware-legal). Trade screens are
    largely icon-driven, but the on-screen text will be Japanese; the user reads
    Japanese poorly, so this is the fallback.
  - On the 3DS side there is a related wart: the port's wireless beta links only
    identical builds ("builds in different languages don't, for now") — irrelevant
    for Option 0 (the FRLG leg is external) but it constrains future multi-console
    3DS scenarios and proves the build-id gate must become bridge-aware anyway.

**Stretch (does the work of G1+half of G2 in one shot):** on the Mac, try to *join* it.
The passphrase candidates are the two 64-hex "Game Boy – Nintendo Switch Online" keys
from kinnay's LDN-Passphrases wiki (first try `4ab9f376ea4ae5ca366bdbf91bf08faf5590b7a3552669b7f077e726c43b3837`,
then `1e6b72909cfb216713a3f9e852b1ff95b912950f26ee880416563dbc47f0b84f`).
- [ ] Join succeeded? (If yes: you have just completed a real association with a console-hosted
      LDN network, from an ordinary client — the core mechanism of the whole design.)
- [ ] If the join fails: which error? ("wrong password" vs "connection failed/timed out"
      tells us association-with-PSK works but the key/app pairing is off.)

**If no network appears in trade Leader mode:** try the documented console-hosting mode
(Mystery Gift → periodically re-check), and note that in the worst case the FRLG app only
hosts in some modes — that would be an architecture problem, not a blocker we can code
around.

## G2 — can a 3DS install/join a 32-hex-SSID + 64-hex-PSK network profile?

**Cost: ~20 min. Needs G1's SSID live (or any AP you can shape to those parameters).**

1. 3DS System Settings → Internet Settings → new connection.
2. Enter the SSID (32 hex chars) exactly; then the 64-hex passphrase.
3. Try to connect (it may be fine even before the in-game parts matter — the point is
   that association+auth succeed at all).

Record:
- [ ] Settings UI accepts a 64-hex passphrase for WPA2? (If it forces "8–63 characters"
      only, note that — it redirects us to the sysconf homebrew route, which is fine
      since the port already requires CFW.)
- [ ] Association succeeded? Failure text?

## G3 — transport rehearsal without a Switch

**Cost: 1 day. Optional but highly de-risking.**

Point a router/AP you control (one whose SSID we set to 32-hex and PSK to 64-hex) at the
3DS profile from G2, then build a tiny UDP toy (later) to confirm sockets-on-association
behavior and jitter. This replaces the in-game parts entirely, so it can run in parallel
with all the hard prototyping work.

---

## B1 — the bridge leg on loopback (UDP build ↔ bridge.py)

**Cost: 1 day. Hardware: none (a Mac). Validates W1's UDP backend and the adapter
protocol, before any radio work.**

`3ds_port/src/3ds_local.c` grew a `LOCAL_LINK_UDP` build: the message streams and the
session state machine are unchanged, but the radio is a UDP socket — the bridge hosts,
the 3DS joins as player 2. `tools/frlg_bridge/bridge.py` is the harness (and, later,
the exact bytes the ESP32 board speaks toward the 3DS). The host test
(`make verify-local-udp`, from `3ds_port/`) compiles the real `3ds_local.c` against
libctru doubles (`tests/local_host/`) and drives the full counter flow against the
bridge on loopback.

Record:
- [x] Test passes — join → advert seen (hosting, player 1) → `PEER_CONNECT` answered
      `PEER_ACCEPT(slot 0)` → `PEER_DATA` echoed → 30-message burst (40 B each,
      split over frames, go-back-N acks both ways) → clean leave
      (**PASS ×3 consecutive runs**, 2026-11-04).
- [x] Wire format pinned by `_Static_assert` in the C (AppData 44 B, beacon 52 B);
      Python mirrors it byte for byte (structs in `bridge.py`'s header).
- [x] Bug caught by writing the second side (worth recording): the C `SendFrame`
      built a frame at buffer offset 0 and `UdpSendTo` sent it as-is, while
      `ReadFrames` expected a `[u16 senderId]` prefix on every data datagram — the
      first real datagram would have been misparsed. Fixed by giving `UdpSendTo` a
      wire scratch that prepends the sender (the beacon carries its id inside).
- [x] Portability notes: `uint32_t` is `unsigned long` on devkitARM but `unsigned int`
      on the mac — log arguments now cast to `(unsigned long)`; the counters' constants
      header (game tree, made by the wireless patch) is included through
      `__has_include`, with `tests/local_host/constants_stub.h` standing in until the
      tree is bootstrapped.

Still open on this leg (not blocking B2): the bridge advertises itself with the 24-byte
broadcast block the *game* shows (today the harness answers with a zero block); the
build-id gate is waived only for bridge-flagged beacons (kind matching stays strict,
so sessions still pair counter-to-counter).

### Merged build (both radios in one 3dsx) — 2026-11-04

`LOCAL_LINK_BACKEND=both` (now the Makefile default) compiles the air and the wire
into one build; `LOCAL_LINK_NO_UDS` (the `udp` value) keeps the wire-only test build.
What the merged build does:

- **Hosting** is always on the air (UDS) — the wire's `HostSession` exists only in the
  wire-only build.
- **Searching** (`JoinNearby`) scans the air first, then listens on the socket
  (100 ms), until the budget runs out; either can win.
- **A lone air host migrates**: `UdpHearBeacons` (each thread pass, while hosting
  alone) keeps the freshest bridge beacon; `MergeAlone` looks over the air first and,
  if it had nothing, moves to a fresh bridge session (`MergeToWire`), generation
  unchanged.
- The air-scan fallback in `EnsureIdle` treats the two radios independently: the
  session fails only if neither comes up.

Host-side verification: `make verify-local-udp` now syntax-checks the uds and both
builds (declarations-only UDS in `tests/local_host/3ds.h`) and runs the wire-only
build against the bridge (PASS). The air half of the merged build needs a real 3DS —
that is B1.

### ESP32 firmware, 3DS leg — 2026-11-04

`tools/frlg_bridge/esp32/` is the board's firmware as an ESP-IDF project
(v5.x, `idf.py set-target esp32`): `main/bridge.c` is the bridge's state
machine as platform-free C99 — a port of `bridge.py`'s wire, byte for byte
(beacon 52 B, go-back-N flush with a 4-frame budget, PEER_CONNECT→ACCEPT
slot 0, PEER_DATA echo, heartbeat re-armed by its own ack, the 6 s
silence); `main/wifi.c` is the board's softAP (`EM3DS-xxxxxx`, WPA2,
channel 1); `main/main.c` the entry. The classic WROOM-32E on purpose: it
has `esp_wifi_set_vendor_ie` and `esp_wifi_80211_tx` for the Switch leg.

Verified before the board exists: `make verify-local-esp32bridge` compiles
`main/bridge.c` for the host (with `test/bridge_host.c`, none of the
board's Wi-Fi in it) and runs the game side's own host test against it —
join, advert, PEER_* exchange, a 30-message burst, clean leave. **PASS**
(gcc, `-Wall -Wextra -Werror` on both sides). What remains for the board:
the softAP, and the Switch leg.

---

## Results / notes

(grow this section as you collect data — include builds/firmware used, date, and raw
values, not conclusions)
