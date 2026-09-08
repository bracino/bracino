# 022 — AMB step-down at node reboot (mechanism unknown)

- **Status:** closed 2026-09-08 — dissolved by human timeline
  reconstruction (see Resolution); no bench tests needed
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

## Resolution (2026-09-08, human)

The boot-session markers (plotter) let the human reconstruct the
morning-of-Sep-8 timeline precisely, and it dissolves the mystery:

- **Every reboot was human-attended.** ~07:30 Rome: first visit with
  the phone (saw wrong FW ID, came back). ~08:30: laptop visit. ~08:45:
  phone + laptop, flash attempts, eventual settle. The "unattended
  05:27Z blip" theory was wrong — the 6.3-min dark gap at 07:27 Rome
  was the human's first power-cycle. No power event is required to
  explain any of it.
- **The AMB steps were room entries.** Door open on every visit; AMB
  drops confirmed repeatedly on entries *without* any reboot. The
  apparent "step scales with dark time" pattern was actually visit-
  duration correlation. (Sep-7's ≤0.8 °C/6-min ceiling simply reflects
  a day with no room entries.)
- **The only real post-boot artifact:** a ~0.3 °C transient in TPO/TPU
  right after boot, gone within ~1 min (snap-20260908-185641 /
  clipboard_2026-09-08_185641.png) — accepted as front-end/board
  warm-up, too small to matter.
- **The reboot-cluster size** (more reboots than the human expected
  from ~3 flash attempts) noted but not investigated — node stable for
  3 days since, and the effort/benefit is poor. If a recurrence ever
  coincides with *no* human presence, revisit via the 019 bench-sag
  work.
- Bench sweep: cancelled (human decision).

Net: no unexplained phenomenon remains. The GW-AMB overlay was added
to the plotter for future correlation work (gw_ambient pre-
2026-09-06T14:49Z is junk — misranged channel, masked in plots).
