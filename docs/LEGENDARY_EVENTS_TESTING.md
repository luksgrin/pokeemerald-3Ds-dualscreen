# Legendary Events 4–8 (and the Jirachi track) — installation & test guide

Source-level event expansion for Pokémon Emerald 3DS, delivered as patches
`0083` (Jirachi track) and `0084`–`0088` in `patches/pokeemerald/`.
All flows use vanilla maps, self-service harbor logic, and the vanilla ferry
flags — nothing on the Switch side is modified.

- **Shared gate for every event below: beat the Elite Four** (game clear).
- Jirachi is a prerequisite *trapdoor*: it unlocks 4–8's ticket givers and
  Celebi's shrine restore, so do that track first (or already have Jirachi).
- Severity-safe: every giver re-issues on a full bag; every ferry unlock rides
  the vanilla `FLAG_ENABLE_SHIP_*` harbor flow.

---

## Install to the SD card

From the repository root, with your ROM available:

```
python3 -m builder.emerald3ds_builder install --rom /path/to/baserom.gba \
    --payload dist/payload --sd /Volumes/<YOUR_SD>
```

This builds `emerald3ds.pak` from your ROM and copies the staged slim
`Emerald3DS.3dsx` + `.smdh` (build id `fe56048f8fe0dd05`, includes the events
and the re-proven corridor voxel fix) to `3ds/emerald3ds/` on the card.
Boot from the HOMEbrew menu.

Quick sanity check before anything event-related: walk the **Abandoned Ship
hidden-floor corridor** past the sealed west door to the Expedition Hold — its
hull face should show no dark gap (a voxel fix went in for exactly that door).

Save before each event. The Japanese variant build (`Emerald3DS-ja` Quick
Update) is only needed for the separate **Switch NSO trade** tests; all events
here can run on your regular-language ROM.

---

## 0. The Jirachi track (patch 0083) — do this first

*One briefing, one battle, one door — all in Meteor Falls.* Solrock and
Lunatone live in Meteor Falls itself, so you'll be revisiting the same caves.

1. **Briefing** — post-League, talk to the scientist on **Mossdeep Space
   Center 1F**. "Lately, the rocks are the ones talking" — he reports a faint
   signal from deep in Meteor Falls and suggests Steven might know more.
   This starts the quest and reveals **Steven in Meteor Falls B1F 2R** (he is
   hidden until the briefing — the port stages him this way; vanilla Emerald
   would have him there after the League).
2. **The battle** — Steven is the vanilla postgame matchup, at full strength.
   He is mandatory for the track: beat him, and after the shared post-victory
   lines he adds his own hint — the Space Center's signal "ebbs and flows like
   COSMIC POWER", and the sun-and-moon marks on the chamber wall behind him.
3. **The door** — the marks puzzle. Interact with the bulge in the wall with
   the meteorite marks (the inscription reads: *"The sun goes first. The moon
   walks last. Let either wield the cosmos, and the way will open."*).
   The arrangement the door demands:
   - **Solrock must lead the party** (slot 1),
   - **Lunatone must be the last member** (slot 6),
   - **either of the two** must know **COSMIC POWER** (both learn it by
     level-up; party order: Solrock first, then fill, Lunatone sixth).
   Pre-League the marks only show the hint; post-League, with the alignment
   right, the stone rumbles, splits open, and reveals a **hidden chamber**.
   Wrong arrangement → only the marks' text; rearrange and try again.
4. **The chamber** — through the new warp inside the split rock: a small
   cave with a **White Rock**. Interact: "A faint warmth pulses from within,
   like a heartbeat…" → **Jirachi battle (level 40)**.
5. **Outcomes**:
   - **Caught** → the star enters your party/saving and the downstream
     cascade starts: the Space Center scientist pivots to recognizing
     Jirachi and becomes Deoxys's ticket-giver, and the Celebi shrine's
     Sweet-Scent restore works.
   - **Fled** → "still warm… it may yet answer again" — talk to the rock
     again to retry.
   - **KO'd (won with a defeat)** → the rock "has gone cold" and Jirachi is
     **gone for good** for this save. Save *before* entering the rock's
     dialogue if you fear fainting it by crit, or you accept the risk.
6. **After** — the rock stays readable (caught: "peaceful feeling remains";
   defeated: "gone now") and the marks read "the way lies open".

Verification checklist for the track: hidden Steven pre-briefing, visible
after; door only opens post-League with correct alignment; the alignment
re-checks live at the moment you touch the marks; caught flag is what every
downstream event keys on.

---

## 1. Deoxys · Aurora Ticket (patch 0084)

**Requires: Jirachi caught, and Jirachi in the party when you talk.**

1. Talk to the Space Center scientist (same NPC as the briefing). Without
   Jirachi in the party he asks you to bring it; with Jirachi in the party he
   relates the resonance and hands you the **Aurora Ticket**, unlocking the
   **Birth Island** ferry.
2. **Lilycove Harbor** — the harbor's self-service flow recognizes the ticket
   → ferry to **Birth Island**.
3. Walk to the triangle and press it once — **Deoxys, level 30** appears.
   Form follows your Emerald version (normal form).
4. Repeat-visit check: the scientist then gives only his *reminder* line, no
   duplicate ticket.

## 2. Mew · Expedition Hold + Old Sea Map (patch 0085)

**Requires: game clear only.** No Jirachi involvement.

1. Post-League, the **sealed door in the Abandoned Ship's hidden-floor
   corridor** unlocks (the script opens it on game clear — it bakes in
   closed until then). Enter the new **Expedition Hold**.
2. Pick up the **Old Sea Map** as an item in the hold (vanilla find-item
   flow).
3. Show it to **Old Salt at Lilycove Harbor**: he recognizes it and unlocks
   the **Faraway Island** ferry.
4. Faraway Island → chase Mew through the woods → **Mew, level 30**.
5. Re-talk to Old Salt: he goes *idle* — no duplicated unlock.
   Note the ordering interplay with Event 7 below: he prioritizes the map
   over the Mystic Ticket if you carry the map before reading the Mt. Pyre
   inscription; the plain path is the Mystic line first, or come back after
   the map is recognized.

## 3. Latias/Latios · Eon Ticket (patch 0086)

**Requires: the roaming Eon Pokémon caught or present in your party/box when
mom's check runs.** Check detail: the check is `CheckRoamingEonInParty`, so
the Eon needs to *currently* be in the party (box placement passes through
mom's newly-added postgame branch only if the check looks at the box — test
both).

1. Post-League, talk to **Mom at home** with the roaming Lati(s) available:
   a letter arrives → its contents → the **Eon Ticket** + Southern Island
   ferry unlock.
2. If the Eon is *not* in the party/box, Mom falls through to the **vanilla
   dialogue** (match-call or heal — nothing event-y should leak).
3. **Lilycove Harbor** → ferry to **Southern Island** → the opposite-color
   Lati waits in the clearing, **level 50** — battle or catch.
4. Re-talk to Mom: her *Eon reminder* line, no second ticket.

## 4. Lugia + Ho-Oh · Mystic Ticket (patch 0087)

**Requires: read the new worn inscription on Mt. Pyre Summit (post-League).**

1. **Mt. Pyre Summit** — interact with the inscription; the first read sets
   the flag (later reads cycle the worn text).
2. **Old Salt at Lilycove Harbor** → legend, record → the **Mystic Ticket**
   and the **Navel Rock** ferry unlock.
3. Navel Rock splits: descend the metal path to **Lugia level 70**, climb the
   rainbow path to **Ho-Oh level 70** — both capturable on one visit.
4. Re-talk to Old Salt: *idle*, no duplicate.

## 5. Celebi · Sacred Shrine (patch 0088)

**Requires: Jirachi caught and in the party, and the shrine's restore to have
been performed.** The shrine door requires game clear too.

1. **Victory Road B1F**: pre-League the shrine door at the far wall is covered
   by an entrance cover; post-League it stands open, warping to the hidden
   **shrine room**.
2. First visit: the altar is **burnt** ("the altar is damaged" text). Nothing
   happens on its own.
3. With **Jirachi in the party**, use **SWEET SCENT from the field menu**
   (Pokémon menu, not a bag item). The effect is intercepted: instead of a
   wild encounter, the burnt altar glows and **blooms** — flowers appear
   around it (six altar-side tiles restore). Without Jirachi in the party,
   Sweet Scent here does nothing event-y — verify that too.
4. Interact with the bloomed altar: a gentle breeze, something approaches →
   **Celebi, level 40 battle**.
5. **Retryable** — after a flee, a defeat, or you teleporting out, the altar
   can be re-tried: the restore flag stays set, just poke the altar again.
   (This differs from Jirachi, who is gone-for-good on a KO.)
6. After catching: the altar is at peace, single line, no re-battle.
7. Save + reset and re-enter: the room's on-load script re-applies the
   **restored tiles**, so the flowers persist across sessions.

---

## Quick cross-check table

| # | Event | Where the ticket/contact | Grantee requires | Battle site | Battle level |
|---|-------|--------------------------|------------------|-------------|--------------|
| — | Jirachi | Space Center briefing → Meteor Falls | League only | Jirachi Chamber, White Rock | 40 |
| 4 | Deoxys | Space Center scientist | Jirachi caught, in party | Birth Island | 30 |
| 5 | Mew | Old Sea Map in Expedition Hold → Old Salt | League + map found | Faraway Island | 30 |
| 6 | Lati(s) Eon | Mom at home | Roaming Eon obtained/present | Southern Island | 50 |
| 7 | Lugia + Ho-Oh | Mt. Pyre Summit → Old Salt | League + inscription | Navel Rock (both paths) | 70 |
| 8 | Celebi | SWEET SCENT at the Victory Road shrine | Jirachi in party | Shrine room | 40 |

All ticket givers hold a **full bag**: the item lines re-issue on re-talk, no
softlock, no duplicate once the given-flag is set.

Keep a note of any hardware misbehavior by map + step — everything here is
source-level and re-derives quickly.
