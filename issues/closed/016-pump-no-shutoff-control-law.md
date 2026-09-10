# 016 — Pump never shutoff in the field: hysteresis was centered, stop threshold sat above the boiler's own limit

- **Status:** closed
- **Type:** bug (control law)
- **Opened:** 2026-09-04
- **Closed:** 2026-09-09 (field-verified from the logger record; human satisfied)
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

## Fix (2026-09-04, in tree; field-flashed before boot 237 Sep 5 —
    verified 2026-09-09, see Field adjudication)

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
- [x] **Field:** observe a full cycle at the boiler room — satisfied by
      record adjudication (below) plus a human-attended clean cycle
      2026-09-08 06:17Z on the freshly flashed node
- [x] Docs bumped (DN002 done 2026-09-04; STATUS refresh still queued —
      known stale, tracked on the session pad)

## Field adjudication (2026-09-09, from the Sep 5–8 logger record)

Trusted region: boot 92 (Sep 6→8, continuous) + Sep 8 morning boots.
Discounted: Sep 5 (brownout lacuna, 019) and interleaved bench traffic
(015 hygiene note). Field params: setpoint 58, hysteresis 4.0 (kept —
final value TBD via Influx experiments; 4.0 has no user complaints).

- Charged stops at TPO 57.9–58.0 with ΔT 0.7–1.4 (≤1.5): **~6 cycles,
  all clean**. Restart only at TPO 53.1–54.0 (= setpoint − hysteresis).
- No chatter: shortest run 5.6 min (min_on 180 s).
- Daytime solar hold-off: tank solar-climbs 58→73 °C with the pump
  off, zero relay edges 07:42→00:00; restart only on real cooling.
- **Jacket-bump residual — observed, bound corrected:** the bump-fall
  signature occurred 2 of 3 nights (tank first reaching restart level,
  ~00:00–01:00 UTC). The backstop fires BEFORE the boiler's anti-cycle
  delay expires; a second backstop stop follows; the third cycle
  charges fully. So the bound is **≤2 spurious stop/restart pairs**, not
  1 as predicted. Self-terminating every time; cost ~20 pump-minutes
  and 4 relay transitions/night. **Sustain-arm NOT built — data does
  not demand it** (per the 2026-09-09 architecture discussion: every
  pump-misbehavior degrades to efficiency/wear, never plant damage;
  sustain-arm would only delay the 017 dead-boiler abort).
- Sensor-fault fail-safe exercised live (TPU −99.9 → pump stopped,
  stayed off, resumed on real cooling) — caveated as brownout-region.
- Re-attribution (human): the 57.8→73.2 °C post-stop TPO spike
  (2026-09-05 14:50Z) is the **brownout signature**, not pipe heat
  soak — see 019 addendum. 017 must not warn on these spikes.

## Addendum (2026-09-10): the backstop double-stop also fires mid-cycle, not only overnight

Observed in the post-outage morning cycle (boot 213, 2026-09-10; node
rebooted 05:08 local after the mains outage): the tank coasted down to
the restart level (54.0) with no heat call, pump restarted 08:27 local,
then the backstop fired **twice in the same loading cycle** — stop
08:37 → min_off restart 08:38 (ΔT 1.5), stop 08:53 → restart 08:54
(ΔT 0.7). The tank was hovering at the restart level with the boiler
not yet contributing; the boiler picked up ~1 min after the second
restart and the cycle charged cleanly to the 58.0 setpoint stop (~10:12
local, ΔT collapsed). Same mechanism as the nightly double-stops
(backstop fires before the boiler's anti-cycle delay expires), so the
bound reads better as **≤2 pairs per hover, per loading cycle** — not
just "per night". Still self-terminating; sustain-arm still not
demanded by data. Adjudication caveat: the second stop needed internal
peak_TPO > 54.0 while samples showed max 53.9 — one-tick sampling lag,
not a law violation.

Chart-reading note from the same cycle (human-attributed): a mid-run
TPU sag (56.0→51.5 over ~10 min) with relay ON and TPO *climbing* is
**ACS (DHW) unloading simultaneously at mid-strata** — it cools the
use line, not the tank top. Do not read it as a relay edge; edges come
from `relay_state` only.
