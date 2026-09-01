# 014 — Hidden second pump relay; node relay/snubber rework; CT direction bust

- **Status:** open
- **Type:** hardware / design
- **Opened:** 2026-08-31
- **Refs:** DESIGN_NOTE_001, DESIGN_NOTE_002 (`ct_confirm_s`), issue 009 (plant checklist — remains open), issue 012 (field install — blocked on this)

## Context (field session 2026-08-31, boiler room)

Inconclusive first plant hookup:

- Node in **Manual**, pump **ON**: worked (relay closed → pump ran).
- Pump **OFF**: our relay **clicked open** audibly, but the pump **kept running**.
- Manual wire test: pulling our relay wires and touching them together toggled
  the pump on/off as expected → the loop itself is a simple contact closure.

**Discovery:** the plant wiring is more complex than assumed. Our contact does
not drive the pump directly. It closes a **~24 V control line** that energizes
a **hidden second relay near the breaker box**, and *that* relay switches the
pump. The old MES-BBU drove the same 24 V line.

**Human hypothesis:** residual voltage on our RC snubber (across our contacts)
kept the second relay's coil from seeing a proper open — a held-through leak
rather than a welded or parallel path. Plausible: an RC snubber across open
contacts in series with a coil is a **permanent current path**; with a
sensitive AC coil the leak can exceed the coil's drop-out current. Also
consistent with the "would open fully if left open longer" observation.

## Implications

1. **CT confirm-running is a bust.** Pump current (~1.5 A, 230 V side) never
   flows through node wiring. The ZMCT103C cannot see the plant motor on any
   pot seat. DN001's boolean contract still holds on the bench, but the plant
   signal it assumed (a pump conductor through the CT window) does not exist.
   - Firmware is **not** in trouble: CT no-current was already **warning
     only** (2026-08-25 decision, DN002) — stay on the standard TPO/TPU
     algorithm. Installed as-is, the node just always shows the WARN notice.
   - Decision needed: drop run-confirmation entirely (same blindness as the
     old MES-BBU), or find another signal (see options below).
2. **Relay/snubber must be reworked** so our contact presents a clean open to
   the 24 V line. The snubber as fitted is wrong for a coil-in-series load.
3. **Install (012) is blocked** until this is resolved and 009's real-pump
   checklist passes.
4. Open questions about the plant (record answers here when known):
   - Is the 24 V line **AC or DC**? (Drives suppression choice: with DC, the
     snubber cap actually blocks steady-state leak — makes the snubber
     hypothesis weaker and a parallel path more likely. Measure first.)
   - Can the second relay be **identified/accessed** (model, coil
     rating, spare poles)? An accessible coil lets us put suppression where
     it belongs (across the coil, not across our contacts).
   - Does the second relay have an **aux contact or is there a pilot light**
     anywhere? A volt-free aux contact back to a node input would restore a
     true run-confirmation signal.
   - What does the **old MES-BBU** have on its output — any suppression?
     (It drove this line for ~25 years; whatever it did or didn't fit is
     field evidence.)

## Alternative causes to rule out before blaming the snubber

- **Parallel path**: another switch/timer/frost-protection contact feeding
  the same 24 V line. Test: with our contacts open and snubber lifted,
  does the second relay still hold?
- **Welded/sticky contacts** on our relay module (click ≠ clean open).
- **Second relay latching / holding contact** wiring near the breaker box.

## Diagnostic sequence (propose, human executes)

1. With our contacts **open**: measure voltage **across the second relay's
   coil** (or across our open contacts) — ~24 V across our contacts with
   ~0 across the coil ⇒ leak through our snubber; ~24 V across the coil ⇒ a
   parallel path exists.
2. Bench: node drives a dummy 24 V-line-equivalent load **with the snubber
   lifted**; confirm clean open/close behavior on a real small relay coil.
3. Decide suppression: likely **remove the RC snubber** (24 V low-current
   coil is a mild load and the MES-BBU drove it bare for decades); if
   suppression is wanted, fit it **across the coil**, never across our
   contacts (an MOV/TVS across our contacts still leaks).

## Run-confirmation options (decision pending)

| Option | Pros | Cons |
|--------|------|------|
| Drop confirmation (DN002 already warn-only) | Zero wiring, matches old controller | Blind to pump stalls |
| Aux contact on second relay → node input | True confirmation, cheap | Access/pull a wire to the breaker box |
| Thermal evidence (TPO/TPU delta trend) | No wiring | Slow, indirect, weather-dependent |
| Clamp a CT at the breaker box on pump wiring | True current signal | New long run to the node; out of phase-1 scope |

## Fix

TBD after diagnostics: hardware change on the relay/snubber (likely snubber
removal, new protoboard rework note), DN002 parameter note for
`ct_confirm_s` semantics if CT is repurposed or dropped, possible new input
if aux-contact confirmation is chosen.

### Investigation plan (agreed 2026-08-31, human executes)

1. **Bench:** cut the snubber out of the relay circuit on the protoboard.
2. **Bench retest with a dummy load** — verify relay opens/closes cleanly.
   Note: a resistive dummy load cannot reproduce the failure mode (held
   coil). To test the snubber-leak hypothesis on the bench, drive an actual
   small relay coil (24 V AC/DC) with the node contact; with contacts open,
   measure coil voltage — **above drop-out ⇒ hypothesis confirmed**, without
   a boiler-room trip. Cheap to do while the snubber is lifted.
3. **Boiler room round 2, snubberless:** node OFF ⇒ does the pump stop
   immediately? Measure the 24 V line (AC vs DC, and voltage across the
   second relay coil with our contacts open — distinguishes snubber leak
   from a parallel path per the diagnostic sequence above).
4. **Locate the second relay** if accessible (model / coil rating / spare
   poles).

Once those are in hand: decide the relay/contact rework and the
run-confirmation direction. **Initial human direction:** the *next* node
becomes a **boiler monitor** — split-core CT on one pump leg, temps at flue
and water-jacket inlet/outlet, blower + fuel-feed current. That group sits
naturally at the plant and would obviate any pump-current sensing on the
BBU controller (see ROADMAP phase-2 energy accounting).

## Verify

- [ ] 24 V line AC/DC measured and recorded here
- [ ] Second relay identified (or at least its coil measured)
- [ ] Bench test: contacts open ⇒ coil voltage < drop-out (snubber lifted)
- [ ] Plant retest: node OFF ⇒ pump stops immediately
- [ ] Run-confirmation decision recorded (update DN001/DN002 as needed)
