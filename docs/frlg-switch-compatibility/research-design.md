# Research Design: Emerald-3DS ↔ FireRed/LeafGreen (Switch) local trades

Status: **proposal / research phase** · Author: luksgrin · 2026-11-04
Scope: a **normal trade** (party Pokémon) between Emerald running on this 3DS fan port and
FireRed/LeafGreen running on a Nintendo Switch (NSO retro app). Out of scope for the MVP:
link battles, chat, Mystery Gift, record mixing, berry blending.

---

## 1. Motivation and problem statement

On real GBA hardware, Emerald and FireRed/LeafGreen shared the same GBA Wireless Adapter
(RFU) protocol, so they could trade and battle in the Union Room across versions. Both of
our ports preserved the *game-level* protocol but each runs it over a *different emulator
radio*:

| side | game | game-level link layer | transport implementation |
| --- | --- | --- | --- |
| 3DS (ours) | Emerald (port, v0.5.0 wireless beta) | emulated GBA **Wireless Adapter (RFU/STWI)** via `patches/pokeemerald/0055-local-wireless.patch` | **3DS UDS local wireless** (`3ds_port/src/3ds_local.c`, nwm::UDS) |
| Switch | FireRed/LeafGreen (NSO: original GBA ROM in the official emulator) | same GBA **RFU** protocol, ROM unmodified; emulator hooks the GBA SVIs (`swi 0x44`–`0x53`) | **LDN + Pia** (Nintendo peer-to-peer middleware, comm id `0x01006fa0233f8000`, LDN protocol 3, AES-GCM `master_key_12`) |

Both consoles therefore "know how to trade", but **their radios cannot hear each other**:
3DS UDS beacons are Nintendo-proprietary 802.11 frames from the NWM service, while Switch
LDN is standard 802.11 + Pia. No game-side change fixes this; only adding a **third node
(bridge)** that can talk to both sides can.

Prior art proving the Switch leg is fully tractable (retail hardware, no console
modification):

- `tornadus/frlg-ldn-trade` → `MercuryEnigma/…` → `Decryptu/pokeldn` — end-to-end trades,
  Union Room (battles, chat), Mystery Gift, link battles against a retail Switch, with a
  Linux Wi-Fi card in monitor mode or an ESP32 softAP. AGPLv3.
- pokeldn's documentation is effectively a protocol spec for FRLG-on-Switch: RFU framing
  over Pia "Reliable", seat rules, trade state machine, timing pitfalls.

## 2. What each side actually speaks (protocol inventory)

### 2.1 The common language: GBA Wireless Adapter (RFU)

Unmodified Emerald and unmodified FRLG already agree at all game levels because they both
implement the Gen-3 RFU stack:

- STWI command set (`librfu_*`): RESET / SYSTEM_STATUS / GAME_CONFIG broadcast (24 B
  advert incl. activity byte) / SC_START (host) / SP_START (search) / CP_START (connect)
  / DATA_TX / DATA_RX and the async 0x28/0x29 events.
- Above that, `link_rfu_2.c` frames: parent keeps **one child slot per poll**, child
  sends **one command per parent frame**; parent **mirrors each child's command back**
  (the "row one" rule — `MGL_Send` and trade blocks depend on seeing their own
  transmission echoed back); `maxMFrame = 4` (a console drops the link on the **5th
  unanswered parent frame**); seat barrier (READY keys) in Cable Club seats.
- Game features: Union Room adverts/activities (IN_UNION_ROOM, TRADE 0x44, BATTLE 0x41,
  CHAT 0x45, CARD 0x48, EXIT 0x40), trainer card + `struct LinkPlayer` exchange, block
  sends (`SEND_BLOCK_INIT` + fragments, e.g. trade party/mail blocks count 9/19),
  held-keys overworld streaming, battle `LinkBattlerHeader` buffers.

Cross-version behaviour is *already in the ROMs*: `IsPartnerActivityAcceptable`,
`ConvertInternationalString`, `IsTryingToTradeAcrossVersionTooSoon`, etc. handle an
Emerald↔FRLG pairing. **If the bridge relays frames faithfully, the games do the rest.**

### 2.2 The 3DS leg (ours — already built)

- `3ds_port/src/3ds_rfu_adapter.c` emulates the WA adapter, answering STWI commands
  synchronously; between consoles it uses a small PEER protocol over the UDS stream:
  `PEER_CONNECT/ACCEPT/REJECT/DATA/DISCONNECT/BATTLE/BLOCK` with host-side slot frames
  (5-bit length nibbles) and `MAX_IN_FLIGHT = 10` pacing.
- `3ds_port/src/3ds_local.c`: UDS discovery (beacon appdata: kind, build id), reliable
  go-back-N stream (resend 100 ms, MSG_MAX 168 B), session thread.
- Game side gates everything through `src/local_session.c` counter scripts keyed by
  `LOCAL_KIND_*`. Two consoles only link when build ids match (SHA-256 in the beacon).

**Key structural insight:** nothing in the game-side cares *how* PEER messages travel.
A UDP/socket substitute for the UDS layer would let the 3DS peer talk to any external
process — this is the main thing we need to add on the 3DS.

### 2.3 The Switch leg (using pokeldn's reverse engineering)

- **LDN**: the 16–64 B passphrase authenticates the association; for the FRLG NSO
  app, LDN protocol **3** (AES-GCM, `master_key_12`); advertise appdata carry the Pia
  session parameter; FRLG requires the advertised rate set to include 6/9/12 Mbit/s and
  leaves ~3 s after association otherwise; beacons at 11M DSSS, data at HT MCS
  (needs a card that hears OFDM frames).
- **Pia** on top of LDN: Net connection (type 1/2), Session (join/update), RTT probes,
  reliable frames repeated in the next 4 datagrams ("carry-forward"), header nonce is a
  monotonic counter strictly enforced by the console, session-key derived from the LDN
  SSID (+game key).
- **RFU over Pia**: vblank-paced RFU slots carried as Pia reliable payloads; emulator
  quirks: 512 ms phase-locked ack-log metronome, ident-25 backlog stalls, and the SVI
  hooks (`swi 0x45` gRfuLinkStatus, `swi 0x49/0x4a` connect-period hold, `swi 0x4b`,
  `swi 0x51` close-link). The console's own RFU output rate ≈ 25.5 frames/s (host) /
  161.8 datagrams/s (to host) — comfortably narrower than the RFU vblank budget.
- pokeldn's Python host (`RFULeader`, `HostPeerProtocol`, `Reliable`, `HostTransport`)
  already drives a retail Switch through a whole trade; radios are Linux monitor-mode
  cards (ALFA AWUS036ACHM, RTL8821CE, mt7921e) or an ESP32 softAP, with keys.

## 3. Core conclusion: the architecture must be 3 parties

```
┌────────────┐   UDS today;         ┌───────────────────┐   LDN radio + Pia      ┌─────────────┐
│ Emerald    │   UDP (LAN) via      │      BRIDGE       │   it acts as the       │  FRLG NSO   │
│ player,    │ ◄──────────────────► │   PC / RPi /      │ ◄────────────────────► │   player    │
│ 3DS        │   a socket backend   │   ESP32 board     │   link host (keys)     │  on Switch  │
└────────────┘   in 3ds_local.c     └───────────────────┘   for the emulator      └─────────────┘
```

Direct 3DS UDS ↔ Switch LDN over the air is a **non-goal**: they are different
radio/frame/crypto stacks; making a 3DS speak Pia (`master_key_12`, monitor-mode 802.11,
Nintendo session-key derivation) through nwm is a research dead-end compared to relaying
through one extra machine.

**The bridge is transparent at the game level.** It must behave at the RFU level as if it
were the Wireless Adapter + the other console's child slot, so that both unmodified game
state machines exchange real Pokémon data. It must implement, per facing console, the
*role that console's game expects* (leader/host vs guest/joiner), and map frames between
the two RFU links — respecting the mirror/echo rules and the maxMFrame/ack timing rules
on both legs.

### 3.1 Bridge hardware candidates

| option | Switch leg | 3DS leg | notes |
| --- | --- | --- | --- |
| **PC (Linux) + monitor-mode Wi-Fi card** | as pokeldn (proven) | UDP sockets on the same LAN/Wi-Fi | fastest to develop and debug with captures; aligns with pokeldn tooling |
| **Raspberry Pi + ESP32 / Wi-Fi card** | as pokeldn | UDP | portable headless version of the same software |
| **ESP32 board alone (pokeldn firmware + our protocol)** | softAP LDN (firmware already emulates the GBA app session) | station-mode UDP from the 3DS joining the ESP32's network | most user-friendly end state (no PC), but firmware development and ESP32 CPU limits (codecs, timing) are a later workstream |
| Switch console itself running ldn_mitm | only IF the user runs a hacked Switch with a *custom GBA emulator* — not the NSO FRLG app | UDP | not applicable to the stated goal (retail FRLG); keep as a footnote |

Chosen hypothesis for the research phase: **PC/RPi bridge first** (correctness and
capturable traffic), porting to ESP32 later if it lands cleanly.

### 3.2 Bridge design options (the central research question)

Wiring both legs together through a single bit-pipe is subtle because each leg has its
own RFU role and timing constraints. Three candidate architectures:

**Option A — "RFU bit-pipe" (frame-level relay, bridge as RFU parent for both consoles).**
Bridge presents to each console an RFU network with slot 0 = bridge, so each console is
always the child (slot 1). Bridge relays the *RFU payloads* of console X's frame into
console Y's parent frame (and vice versa), maintaining the parent-frame length-nibble
table. Pros: thinnest possible bridge; games stay authoritative; maximum version
compatibility (everything behaves exactly like hardware) — including union room, trades
and eventually battles all through the same pipe. Cons: the two legs are pacing-coupled —
Emerald's slot rate is driven by its own vblank (60 fps), while the Switch leg must
self-hold off during the console's 512 ms ack stall / quiet gates (maxMFrame rule). The
bridge must decouple the legs and prove that hold-offs of a few hundred ms don't trip
the child's `numChildRecvErrors`/link-loss timers. Also, whichever player picks "Leader"
at the counter, exactly one RFU parent exists (the bridge), but the game-level
leader/joiner identities must map cleanly onto RFU slots — else mpId/seat mismatches.

**Option B — "game-level proxy" (bridge runs the trade state machine on each leg).**
Bridge = pokeldn-style autonomous leader on the Switch leg, and a local emulation of our
3DS adapter game levels on the 3DS leg; it *understands* the trade state machine and
relays game-data blocks (party, mail, LinkPlayer profile) between the two sessions.
Pros: each leg is fully decoupled and paced by the bridge; every leg uses the exact
proven host implementations (pokeldn's `HostTradeEngine` for FRLG, `3ds_rfu_adapter`
partner logic for Emerald). Cons: much more state duplication, must reimplement the
trade flow for a 4-way/second player, does not automatically extend to Union Room (many
activities), and must translate gen-3 identity constraints (species mails, TID records).

**Option C — hybrid (recommended starting hypothesis): frame-relay core (A's data path)
with a "quiet-gate driver" per leg.** The bridge runs one `RFULeader`-class entity per
leg — based on pokeldn's `rfu_leader.py` (which already owns the frame echo table, the
NI/UNI handshake, maxMFrame discipline and hold-off counters for a real console) for the
Switch leg, and a mirror of our `3ds_rfu_adapter` peer semantics for the 3DS leg — and
moves only *slot payloads* between the two legs. Timing coupling is confined to the
payload-drain hyperparameters (max in-flight per leg, replay of stall buffering). This is
the hypothesis to validate first, with Option B as the fallback if cross-leg timing
cannot hold.

## 4. What must change in this repo (workstream W1: 3DS side)

1. **New `3ds_local` backend: socket/UDP transport** (feature flag), as a peer of the
   UDS backend inside `3ds_local.c`'s message/ack/queue semantics (drop-in
   `LocalLink_Init` variant, or a `LOCAL_TRANSPORT_UDP` env/config). All properties
   preserved: reliable stream per peer, advert beacons, peer events, timeouts.
   - If the bridge is an ESP32/RPi hot-spot, the 3DS joins it like any router (internet
     mode in system settings) and talks plain `soc:U` UDP sockets — no keys, no
     user-visible change.
2. **Bypass the build-id gate for bridge peers**: today two consoles only link when
   `SHA-256(build + romfs abi)` matches. A bridge peer should be identified by a
   distinct session kind (e.g. a `LOCAL_KIND_BRIDGE` counterpart), so that pairing with
   a foreign console (the FRLG leg) works at all, and the language restriction does not
   block a bridge trade (e.g. a Japanese-language Emerald build should still accept an
   English LeafGreen behind the bridge).
3. Keep 3DS↔3DS wireless exactly as-is (UDS unchanged, feature still shipping).
4. Session UX: a new counter/intent or an extension of the existing Direct Corner /
   Union Room scripts that also advertises to bridge peers; a bottom-screen "looking for
   bridge" state; HOME/sleep behaviours already handled by `3ds_platform.c` lift cleanly
   (sockets need `acInit`-derived stateless sleep hooks).

Note: to touch any of this we must work on the wireless branch
(`fix/ja-pc-shop-window-crash` / `upstream/main`, patch set through
`0055-local-wireless`); `main` HEAD does not contain the wireless layer.

**Status (2026-11-04, branch `feature/frlg-bridge-trade`):** item 1 is in —
`3ds_local.c` builds with `LOCAL_LINK_UDP` (`make LOCAL_LINK_BACKEND=udp` in
`3ds_port/`): same message streams and session machine, radio = one UDP socket; the
build-id gate is waived for bridge-flagged beacons (half of item 2; kind matching
stays strict). The B1 rig (below, §5) runs against it: `tools/frlg_bridge/bridge.py`
hosts the session and speaks the adapter's protocol, and `make verify-local-udp`
exercises the join, the PEER_* exchange and a multi-frame burst on loopback —
no radio hardware needed.

## 5. Workstream W2: the bridge

Software components (initially Python for velocity, mirroring pokeldn's layering):

| component | owns | base in pokeldn |
| --- | --- | --- |
| `LdnHost` (transport) | LDN radio AP, association, UDP/12345-ish peer datagrams, beacon appdata with FRLG protocol 3 + session key | `ldn/transport.py`, `host_pia.py` |
| `PiaPeer` | Pia handshake to the FRLG console, nonces, RTT, orderly session, carry-forward framing | `ldn/host_pia.py`, `ldn/reliable.py` |
| `RfuLegSwitch` | parent RFU toward the `FRLG` console: slot framing, echo table, maxMFrame discipline, quiet gates, NI/UNI events | `gba/rfu_leader.py`, `host_session.py` |
| `RfuLegEmerald` | the 3DS peer protocol + the client side of our adapter assumptions (PEER_CONNECT/ACCEPT/DATA/DISCONNECT, host slot frames) | inverse of our `3ds_rfu_adapter.c` |
| `BridgeCore` | maps child slot payloads between the legs, mpId conversion, activity negotiation passthrough, config, logging/captures | new |

Functionality schedule:

- **B1 test rig**: bridge + 3DS only (the bridge fakes a second Emerald via our
  PEER/adapter protocol). This validates the UDP backend, pacing and the adapter logic,
  without any Switch — e.g. bridge ↔ one real 3DS carrying a trade, or two bridges
  simulating two 3DSes in an offline harness.
- **B2 bridge + Switch**: reproduce pokeldn's Direct Corner trade as is on the Switch
  leg (bridge pretends to be an autonomous trainer, exactly as pokeldn) while the 3DS
  leg is idle. Verifies radio/keys/session establishment, plus reproduces known quirks
  (rate sets, quiet gate, ack metronome) on *our* hardware.
- **B3 bridge joins the two**: a trade where the FRLG leg is a real retail FRLG player
  and the Emerald leg is a real 3DS player, with the bridge acting as the frame relay
  (Option C). Success = a completed trade and both saves persisted, on retail hardware.
- **B4**: repeat B3 in all role combinations (Emerald as leader vs joiner of the
  counter, and FireRed vs LeafGreen).
- **B5**: Union Room (broadcast activity adverts through the bridge, NI/UNI handshake
  selection of the no-parent-NI rule, chat discipline), if the trade MVP is solid.

## 6. The research questions this design must resolve (in priority order)

1. **Timing decoupling (biggest risk).** Emerald's RFU slot rate (a parent at vblank
   rate) vs the Switch console's 512 ms phase-locked ack stall, quiet gates and idler
   behaviours. Does a hold-off of ~0.5–1 s by the Switch leg break the 3DS leg
   permanently or degrade it (child `numChildRecvErrors`, link-loss timers)? We need
   a concrete state machine with measured hold-off budgets per leg.
2. **Roles and multiplayer IDs under a transparent bridge.** The bridge owns RFU slot 0
   on both legs. Does the game-level Union Room / Cable Club leader / guest
   negotiation still resolve correctly when both consoles are RFU children? All flows
   (leader on 3DS ↔ joiner FRLG and reversed) need a walk-through matrix —
   work item: produce the RFU path for each flow from the `link_rfu_2` decomp.
3. **Can `soc:U` UDP keep pace per RFU slot on the 3DS**: LAN latency in the same room
   is tiny, and packet size per slot is small (~6–14 B); the concern is pacing/jitter of
   a per-stream socket under the local-session thread, not throughput. Measure RTT and
   jitter with 100 packets/s on the target build before designing the hold-off logic.
4. **Advert compatibility**: Emerald advertises GAME_CONFIG broadcasts with Emerald's
   activity bytes; FRLG uses its own set (the ROM code maps across versions). The
   bridge must not normalise anything: be bit-faithful on the 24-byte advert in both
   directions.
5. **Version signatures must survive verbatim.** Emerald only refuses a partner when it
   can't tell what version it is; e.g. `IsTryingToTradeAcrossVersionTooSoon` needs the
   partner's `versionSignature` (carried in trainer-profile blocks). The relay must
   preserve block contents bit-exactly, never synthesise identities of its own.
6. **Which emulator/firmware revisions behave how**: LDN/Pia internals differ across
   Switch firmware updates (pokeldn had to pin specific behaviors); fingerprint the
   firmware on our test console before building the Switch leg, and re-verify after
   updates.
7. **Licence strategy**: fork-and-extend vs clean-room re-implementation of the
   AGPL-licensed pieces — see §7.

## 7. Legal / licensing notes

- `pokeldn` and its documentation are **AGPLv3**. Two compliant ways to use it:
  1. fork/extend the AGPL project (bridge stays its own repo, links to our README);
  2. **clean-room re-implementation** of from-scratch pieces of the Pia/LDN host from
     the protocol documentation published under AGPL. Both are acceptable; the choice
     sets the bridge's licence. Reusing their capture-derived behavior constants is
     safest under (1).
- Keys handling: Switch `prod.keys` are used only to decrypt the console's own
  local-wireless traffic (as pokeldn documents); the tooling must require keys from the
  user's own console, and must never bundle or distribute them.

## 8. MVP (definition of done)

- [ ] On a shared Wi-Fi network, a retail Switch running FireRed **or** LeafGreen
      (Direct Corner "Trade" or Union Room trade board) completes a full trade session
      with a real player on the 3DS running Emerald (this port).
- [ ] Both saves verify the received Pokémon in-game after reboot: correct species,
      moves, OT/TID, stats, and game-appropriate restrictions applied by the receiving
      game (no corrupted or illegal mons).
- [ ] Disconnect paths: through-the-menu exit, HOME button, sleep, out-of-range —
      neither console hard-freezes; the 3DS cleanly returns to overworld.
- [ ] The 3DS↔3DS UDS wireless feature remains intact (same patch set) because changes
      are additive.

Next steps (in order):
1. Read `3ds_port/src/3ds_local.c` + `3ds_rfu_adapter.c` and `RFULeader` (pokeldn) with
   the W1/W2 items above in hand and refine the open questions of §6 into concrete
   testable hypotheses.
2. Prototype the UDP backend of `3ds_local` (W1) & the bridge's `RfuLegEmerald`
   re-packaging against the offline test harness (B1).
3. Set up PC/Linux Wi-Fi rig (or verified card table) + Switch FRLG and capture a real
   Direct Corner trade (verify our card works along the captured B2).
4. Write up the protocol-analysis note that resolves each §6 open question in this
   folder, then merge into an implementation design document.

---

## Appendix A dated 2026-11-04 — constraint: stock Switch only ("Option 0")

Constraint added after review: the user cannot modify the Switch, and wants hardware
limited to the two consoles. All bridge options that require either a CFW Switch
(`ldn_mitm`, sysmodule) or a third node (PC monitor-mode card, ESP32) are therefore
demoted to fallbacks. One architecture survives.

### A.1 The architecture: the 3DS joins the FRLG console's own network (no bridge)

```
Switch (STOCK)                                       3DS (Emerald port + LDN transport backend)
  FRLG NSO app, Direct Corner trade "Leader"          Emerald agent, "Joiner"
      │                                                       │
      └── hosts a standard WPA2-PSK 802.11 network            └── associates as an ordinary Wi-Fi STATION
          (SSID = hex(session id), PSK = the game's                 to that network; runs a Pia client
           passphrase constant); speaks Pia over UDP                + emulated RFU child frames
          inside the association, carrying the GBA
          ROM's RFU traffic

      ◄────────────── RFU trade protocol, exactly as on GBA hardware ──────────────►
```

Key simplification vs the §3 bridge design: **there is no bridge and no frame-relay.**
FRLG hosts ⇒ RFU parent; Emerald joins ⇒ RFU child; each speaks its native hardware role.
Only two prerequisites to add on the 3DS: a Pia/LDN *client* transport, and an alternative
RFU framing backend that matches the Switch emulator wire format.

### A.2 Facts this rests on (verified in sources)

- The Switch emulator maps the GBA ROM's RFU traffic onto LDN/Pia; the resulting
  wire format (RFU frames, seats, block sends over Pia reliable sessions) is documented in
  pokeldn's FRLG pages — a defined format to reimplement client-side.
- Console-hosted LDN sessions definitely exist (Mystery Gift host mode is documented
  against retail hardware); whether Direct Corner trade "Leader" hosts the same way is
  gate E1 below.
- Association is standard WPA2: `SSID = hex(NetworkId.SessionId)` (random per host
  session), PSK = the game's LDN passphrase constant, then UDP/IP over the network.
- The app's passphrases are pure-ASCII 64-hex strings (Game Boy NSO keys, kinnay
  NintendoClients wiki LDN-Passphrases: two candidates; which of them applies to the
  FRLG app is one test). 64-hex is exactly the PSK form the system settings' "64
  hexadecimal characters" entry supports, so installable by settings UI and, on a CFW
  3DS (which this port already requires), by homebrew config writes.
- The FRLG app is LDN protocol 3, AES-GCM under `master_key_12`; the Pia session key
  derives locally — no keys source beyond decrypting one's own console's traffic is
  needed client-side.

### A.3 Gates (empirical, cheap → costly)

- **G1 (minutes):** put the Switch in FRLG Direct Corner trade Leader; scan from any
  laptop/phone. Confirms trade mode hosts, and captures the SSID/advert shape.
  Fallback: Mystery Gift host mode (documented).
- **G2 (minutes):** create a 3DS network profile with SSID = 32-char hex and
  PSK = 64-char hex; validate that config install (settings UI or sysconf homebrew)
  yields a successful association. Also settle which of the two passphrase constants
  applies.
- **G3 (days, no Switch needed):** replay an LDN-like beacon (router whose SSID/rates
  we control) → 3DS associates and exchanges UDP. Validates the entire transport chain
  before trade logic exists.
- Then the main build: Pia client (session, RTT, strict nonce counter, carry-forward
  retransmission, GCM session key) + RFU-over-Pia framing + timing tolerance
  (512 ms ack metronome, `maxMFrame`/quiet gates) as a second `LocalLink` backend
  beside UDS.

### A.4 Relative confidence

| layer | confidence |
| --- | --- |
| Switch-side proof (RFU over LDN+Pia, retail) | proven by prior art |
| FRLG console hosting LDN | proven (MG); trade mode = G1 |
| 3DS standard WPA2 association | proven (internet mode) |
| Passphrase/profile install | likely soluble (G2) |
| SSID churn per session | solvable (config rewrite; UX cost) |
| Timing tolerance & client implementation | engineering-heavy; the doc's open questions §6.1–6.3 still apply |

Failure ladder (in order): stock-Switch-direct (this appendix) → PC/ESP32 bridge (§3) →
`ldn_mitm` CFW-Switch bridge (§3.4's footnote, needs a modded Switch, not applicable) →
research dead ends documented: raw 802.11 sysmodule on the 3DS (needs radio-firmware
reversing, undemonstrated), FRLG online/GTS channel (server-side, can't accept a 3DS
peer, can't MITM without CFW Switch).

---

### Sources

- pokeldn (README + `docs/`: frlg.md, frlg_link.md, ldn.md, frlg_host.md, hardware pages)
  <https://github.com/Decryptu/pokeldn>, <https://decryptu.github.io/pokeldn/>
- tornadus/frlg-ldn-trade: <https://github.com/tornadus/frlg-ldn-trade>
- Our upstream workspace: `patches/pokeemerald/0055-local-wireless.patch`,
  `3ds_port/src/3ds_local.c`, `3ds_port/src/3ds_rfu_adapter.c`, `CHANGELOG.md` (v0.5.0)
  on `upstream/main` / `fix/ja-pc-shop-window-crash`.
- pret/pokefirered (incl. switch port symbols), pret/pokeemerald.
