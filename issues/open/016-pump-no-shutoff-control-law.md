# 016 — Pump never shutoff in the field: hysteresis was centered, stop threshold sat above the boiler's own limit

- **Status:** open
- **Type:** bug (control law)
- **Opened:** 2026-09-04
- **Refs:** `firmware/node-bbu/main/control.c`, `params.c`, `docs/DESIGN_NOTE_002_bbu_control_loop.md`, `closed/009`, 012 (field install)

## Context

Field observation at the boiler room (human, 2026-09-04): the node did not
shut off the pump even though the (displayed) threshold **~58 °C** was
reached, and the boiler had already dropped out on its own jacket stat at
**61 °C**.

Root cause — pre-016 `control.c` put the hysteresis band *centered* on the
setpoint (60/3 defaults):

- on threshold = `setpoint − hyst/2` = 58.5 °C
- off threshold = `setpoint + hyst/2` = **61.5 °C**

The boiler's own limit is 61 °C, so the tank top plateaus just below 61.5 —
`TPO ≥ off_threshold` can never fire. Worse, the failure is a **livelock**,
not just a late stop: once the boiler is satisfied and TPO decays while the
pump circulates tank water through the idle jacket, TPO moves *away* from
61.5 forever. There is no reachable stop path once TPO falls below the off
threshold mid-cycle. The dT condition collapsing doesn't rescue it, because
the old stop line required `TPO ≥ off_threshold` too.

## Fix (2026-09-04, in tree — not yet flashed)

Control law revised per the human's diagnosis ("hysteresis should only apply
while the tank is cooling"):

- **Stop:** `min_on && [ TPO ≥ setpoint (charged) || cooling backstop ] && dT ≤ delta`
  (TPO_ONLY forces dT satisfied, as before)
- **Restart:** `TPO ≤ setpoint − hysteresis && min_off` — the full
  hysteresis now lives **below** the setpoint, on the cooling side only
- **Cooling backstop (livelock guard):** while RUNNING, if TPO has been
  **above the restart level this cycle** (peak-tracked) and falls back to
  `setpoint − hysteresis` with dT collapsed, the boiler is no longer
  contributing → stop. Peak tracking prevents a fresh start on a mixed tank
  from self-stopping during boiler warm-up (relay chatter).

Param semantics change (documented in DN002): `tpo_setpoint_c` **is** the
shutoff temperature; `hysteresis_c` is the restart gap below it. Params
persist, so a field unit keeps whatever setpoint it has — if shutoff at 58 is
wanted, set `tpo_setpoint_c = 58` from the local UI, no reflash needed.

## Fix

`firmware/node-bbu/main/{control.c,control.h,params.c,params.h}` (law +
peak tracker + display helpers), `test/test_control.c` (new: stop at
setpoint, restart level, cooling backstop, warm-up no-chatter).

## Jacket-bump residual (accepted, observe — human 2026-09-04)

Cold tank + heat-soaked jacket can record a peak from **jacket flush**, not
burner fire: TPO bumps above the restart level, falls while the boiler is
dragging its jacket down through its stat, tank equalises → backstop stops
the pump once. The burner lights during `min_off`; the **restart resets the
peak**, so the next cycle charges normally to the charged stop. Bound: at
most **one** spurious stop/restart pair per soaked-jacket event, and it
cannot loop (each stop flushes the jacket cooler). The same trace is also
the correct abort if the burner never lights at all. **Accepted; 015 logger
data adjudicates** whether to sustain-arm the peak (hold-above-level timer).
The bump-fall signature is the same signal as 017's boiler-out warning.

## Verify

- [x] Host unit tests pass (`gcc` on `test_control.c`, includes the four
      new 016 cases)
- [x] `idf.py build` clean (C3)
- [ ] **Field:** observe a full cycle at the boiler room — pump must stop
      when TPO reaches the setpoint with dT satisfied; must restart only
      after TPO falls to setpoint − hysteresis; must NOT cycle-chatter
      during boiler warm-up
- [ ] Docs bumped (DN002 done 2026-09-04; STATUS on close)
