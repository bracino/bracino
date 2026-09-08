# 022 — AMB step-down at node reboot (mechanism unknown)

- **Status:** open — phenomenon established on data; mechanism unresolved
- **Type:** forensics / sensor behavior
- **Opened:** 2026-09-08
- **Refs:** 019 (power events), 015 record-hygiene note, DN00x AMB channel

---

## Phenomenon

At every node reboot the AMB reading steps DOWN, then recovers
monotonically over ~10–15 min. Step size scales with prior runtime and
dark time:

| transition (2026-09-08) | prior runtime | dark | ΔAMB |
|---|---|---|---|
| 92 → 99 (05:27Z) | 39 h | 6.3 min | −1.4 °C |
| 152 → 65 (06:54Z) | 38 min | 1.9 min | −1.0 °C |
| 99 → 27 (06:12Z) | 38 min | 1.5 min | −0.6 °C |
| 27 → 26 (06:14Z) | 31 s | ~20 s | −0.2 °C |

## Established on data

- **Not normal room dynamics:** Sep-7 (single 39 h boot session, zero
  reboots) shows max |ΔAMB| 0.10 °C per 6 min typical, 0.80 °C max in
  any 6-min sliding window all day. The 05:27Z −1.4 °C across one dark
  window exceeds anything the room does unaided.
- **AMB-channel-specific:** TPO/TPU across the same transitions move
  only within natural drift (tank cools ~0.2 °C per 6 min at 57 °C;
  observed −0.2 to −0.3 °C). No common ADC/front-end artifact visible.
- **In-session AMB is clean:** zero single-sample outliers >3 °C across
  the whole record (the one isolated low dot in plots sits at a boot
  boundary — same phenomenon).

## Ruled out / doubted

- **NTC divider self-heating** — ruled out by physics (external probe;
  ~100 µW in a 20 kΩ divider at 3.3 V → millikelvins). Agent's first
  theory, retracted 2026-09-08.
- **"Board heats the sensor" enclosure-coupling** — doubted by human:
  AMB thermistor hangs on a wall screw ~5 cm from the enclosure;
  whole circuit ~200 mA. Not fully excluded (boundary-layer halo of a
  warm box in still air), but the −step-then-recover shape would then
  be "probe reads true ambient while dark, halo re-forms on boot" —
  plausible-looking yet unproven.
- **Input RC recharge** — τ math absurd (would need ~farads behind a
  20 kΩ divider to stretch recovery to 10–15 min).

## Open hypotheses

1. Warm-enclosure boundary-layer coupling (see above) — testable.
2. Some power-event-correlated physical effect: the same mains
   disturbance that reboots the node also perturbs the boiler-room
   thermal state (boiler cut/restart). Doesn't naturally explain the
   dark-time *scaling*.
3. Unknown AMB front-end behavior specific to that channel's wiring
   (probe on screw, cable route, connector).

## Next diagnostic (folds into 019's deferred bench-sag work)

Bench, serial attached: let AMB settle (node running N min), power off
D min, power on, log AMB recovery. Sweep (N, D) ∈ {(30, 2), (30, 10),
(120, 10)}. If recovery amplitude scales with D and vanishes when the
enclosure is pre-cooled, hypothesis 1 is confirmed. If AMB steps even
with the probe held away from the enclosure in free air, it's channel
wiring/electronics (hypothesis 3).

## Practical rule until resolved

Discount AMB for ~10–15 min after any boot-session change (see 015
record-hygiene note). Plotter now marks boot-session starts.
