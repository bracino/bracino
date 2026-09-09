# 024 — post-stop TPU plunge: boiler ramp + upper strata left unharvested (efficiency)

- **Status:** open — DEFERRED observation; explicitly no control-algo
  change now (human, 2026-09-09; reaffirmed 2026-09-09 after the
  destratify-extension design discussion: deferred to later in the
  game. The solar-day caution stands — when TPO and TPU are both well
  above boiler-driven ranges, keep the pump off, which the current
  code already does. The guarded design below is on file for when
  this is picked up.)
- **Type:** enhancement (efficiency, future)
- **Opened:** 2026-09-09
- **Refs:** closed/016 (law + hysteresis experiments TBD), 017 (boiler
  dropout trend), DN002, ephemera/tpudrop.png + snap (chart evidence)

## Observation (human, from the Sep 7–9 charts)

While the pump runs and the boiler fires, TPO and TPU track closely
(~1–1.5 °C apart). The pump-start decision usually lands mid-ACS-draw
stair-step, and the stop condition is often reached while the boiler is
STILL cranking — the upward TPO ramp is steep at that moment. After the
pump stops:

- TPO declines gradually (tank top coasting);
- TPU drops precipitously (~5 °C shortly after pump-off) as the ACS
  draw continues for another ~10 minutes, then stabilizes lower.

Pattern visible on every overnight cycle in the Sep 7–9 record
(ephemera/tpudrop.png and snapshots; gray pump spans make it obvious).

## Interpretation (human)

Heat is left on the table twice: the boiler's still-rising ramp output
is not fully harvested at stop, and the upper BBU strata are not pushed
down into the ACS loading zone while the draw continues. Running the
pump a few more minutes past the stop condition — maybe another ~10 —
would catch both. It might cause the boiler to fire again, but it is
already hot, so no problem.

## Options (for whenever this gets picked up)

1. Fixed run-past/tail timer: extend the run N minutes after the stop
   condition fires (simplest; N to be tuned from Influx data).
2. Hold-while-contributing: keep the pump on while the boiler is
   detected still firing (needs a firing signal — jacket/ΔT trend;
   overlaps 017's detection machinery).
3. Fold into the 016 hysteresis experiments: hysteresis changes cycle
   shape too — adjudicate together rather than twice.
4. Do nothing: quantify the loss first from Influx (kWh-equivalent of
   the unharvested ramp) and decide if it matters.

## Verify (when implemented)

- [ ] No chatter regression (016's no-chatter guarantee holds with the
      extension active).
- [ ] TPU plunge after stop measurably reduced in Influx charts.
- [ ] Boiler re-fire behavior observed and bounded (no short-cycling).
