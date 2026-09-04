# Design note 002 — BBU offline control loop (emulate MES-BBU)

**Status:** Implemented. Desk `sim` walk-through passed (2026-08-22). Dummy AC load validated (2026-08-25). Thermometer / plant proof still open.  
**Date:** 2026-08-22 (CT warn-only 2026-08-25; boot-mode persistence 2026-08-30; CT dropped from circuit 2026-09-01; **control law revised 2026-09-04 — issue 016: hysteresis on the cooling side only, cooling backstop**)  
**Applies to:** `firmware/node-bbu` local loop on the v0.08 protoboard  
**Does not apply to:** gateway, MQTT, or a later ACS node. Local TFT/encoder is on-node I/O (not the network path).

## What we are emulating

The failed Paradigma MES-BBU ran the boiler→buffer pump against a **stratified 1500 L store**. The boiler itself is independent: it fires from **jacket temperature**. When the BBU pump floods that jacket with cooler tank water, the boiler comes on. The controller’s job is to start a loading cycle when the **top** of the tank has gone cold, and to keep the pump on long enough that the thermocline is pushed **down**, not just to reheat the top 20 cm.

The old box had a pump relay and two tank wells. It did **not** have a current sensor. Bracino added a CT for confirm / reporting (boolean on this prototype — [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md)) — but the plant wiring makes it useless: the node's contact only closes a 230 VAC contactor coil (ABB ECB24-40 in the breaker panel; issue 014), so **as of 2026-09-01 the CT and snubber are dropped from the circuit** and the node is deliberately as blind to pump current as the MES-BBU was. Run-confirmation returns via the future caldaia monitor node (phase 2).

## Sensors

| Name | Well / role | Hardware (assumed) | In Auto? |
|------|-------------|--------------------|------------|
| **TPO** | Top of tank (NTC1) | TH1 → ADS1115 A1 | Start / stop |
| **TPU** | Bottom of tank (NTC2) | TH2 → ADS1115 A2 | Stop (thermocline); inversion check |
| **AMB** | Ambient (NTC3) | TH3 → ADS1115 A3 | **Reported only** — never start/stop/fault |
| **CT** | Pump conductor | ZMCT → A0 | **Dropped 2026-09-01** (issue 014): pump current never crosses node wiring; A0 ignored (`CT_FITTED 0`) |

Correct this table if the physical wells are on different TH pads.

Valid converted range for TPO/TPU: **−5–110 °C**. Open, short, or out of that range is a fault, not a temperature. Tank **operating** range is still ~18–95 °C; 95 °C is not a sensor ceiling. (Hardware rails from bench: open ≈ 13 mV, short ≈ 3283 mV; treat mid ≲ 50 mV or ≳ 3200 mV as unusable before conversion.)

NTC model: **β = 3950**, R25 = 10 kΩ (NRBE 10 k / 3950). Two-point check on potted sensors (human, 2026-08-25):

| Bath | Typical mid | Firmware °C | Notes |
|------|-------------|-------------|--------|
| Ice water | 760–776 mV | 0.2–0.6 °C | All three wells; β is fine at the cold end |
| Room | ~1500 mV | ~21 °C | While the other well was in the boil |
| Rolling boil | 3053–3083 mV | 94.8 °C then **FAULT** on the old 0–95 cap | 3083 mV **is** ~100 °C; not a short (short ≈ 3283 mV) |

AMB uses the same conversion for the serial print. No offset applied.

## Parameters

Tune on site. Values below are starting guesses (old-controller memory plus new safety timers).

| Parameter | Default | Role |
|-----------|---------|------|
| `tpo_setpoint_c` | 60.0 | **Pump-stop threshold.** Shutoff as soon as the tank top reaches it (with dT satisfied) |
| `hysteresis_c` | 3.0 | **Restart gap below the setpoint** — applies only while the tank is cooling (016) |
| `min_on_time_s` | 180 | Anti-short-cycle once RUNNING |
| `min_off_time_s` | 60 | Anti-short-cycle once IDLE |
| `min_tpo_tpu_delta_c` | 5.0 | Small top−bottom gap means the tank is largely charged |
| `max_run_time_min` | 60 | Longest normal load; **warning only**, pump keeps its state |

Derived, not stored (revised 2026-09-04, issue 016):

- `tpo_off_threshold` = `tpo_setpoint_c` → **60 °C** with the defaults — pump **stops** once TPO reaches the setpoint (and dT is satisfied)
- `tpo_on_threshold` = `tpo_setpoint_c − hysteresis_c` → **57 °C** with the defaults — pump may **restart** only after the tank has cooled a full hysteresis below the stop point

A **small** `(TPO − TPU)` means the thermocline has collapsed / the store is full. A **large** delta means hot top, cold bottom — still room to charge.

## Why TPU is in the stop condition

While loading, TPO rises quickly (hot flow enters the top). Stopping on TPO alone reheats a thin layer and drops back to IDLE — the classic short cycle on a tall tank. The old behaviour was: keep pumping until **TPU also comes up**, i.e. a useful volume has been pushed down. Formalised as `(TPO − TPU) ≤ min_tpo_tpu_delta_c` **and** TPO already at/above the setpoint.

The boiler may drop out on its own jacket long before that. **But not for long (016):** once the boiler is satisfied and TPO decays while the pump circulates through the idle jacket, the pump is no longer pushing heat *down* — it is equalising and cooling the store. When dT has collapsed **and** TPO has fallen back to the restart level (`setpoint − hysteresis`) after having been above it this cycle (peak-tracked), the loop stops: the boiler is demonstrably not contributing. Peak tracking prevents a fresh start on a mixed tank from self-stopping during boiler warm-up. Without this backstop the charged-stop condition becomes unreachable once TPO falls below the setpoint mid-cycle — the field failure that prompted 016 (pump ran on with the boiler satisfied at 61 °C because the old `setpoint + hyst/2` off-threshold, 61.5 °C, sat *above the boiler's own limit*).

## Modes and states

**Mode** is how the box is being used. **State** is the pump cycle inside a mode that is allowed to run.

**User modes** (what the operator picks — serial `auto` / `manual` / `test` / `halt`, or Control Program on the TFT):

| Mode | Who selects | Pump |
|------|-------------|------|
| **Auto** | User | IDLE / RUNNING machine below |
| **Manual** | User | On/off as commanded; no auto start/stop |
| **Test** | User | On/off as commanded; **15 min** then back to Auto |
| **Off** | User | Forced **OFF** until the mode is changed. No frost protection yet |

There is **no** mode called Normal. Serial `auto` enters Auto. `on` / `off` / `t` still force **Manual** coil commands (bench). Off mode is `halt`.

**Internal overlays** (not user-selectable):

| Overlay | When | Pump |
|---------|------|------|
| `TPO_ONLY` (display **Auto***) | TPU unusable / inverted while Auto | Same machine, TPU and delta ignored |
| `FAULT` (display **Fault**) | TPO unusable | Forced **OFF**. `user_mode` is remembered |

`FAULT` returns to the **remembered user mode** when TPO is good again (Off stays Off). `TPO_ONLY` returns to Auto when TPU is good. Test expires to Auto. Manual stays until the user leaves it.

Inside Auto (and Auto*):

```text
IDLE --(TPO ≤ setpoint−hysteresis && off-timer expired)──► RUNNING
  ▲                                                        │
  │                                                        │ min_on_time done
  │                                                        │ && [ TPO ≥ setpoint (charged)
  │                                                        │      || TPO fell back to
  │                                                        │        setpoint−hysteresis after
  │                                                        │        peaking above it (016
  │                                                        │        boiler-gone backstop) ]
  │                                                        │ && (TPO − TPU) ≤ min_tpo_tpu_delta_c
  └──────────────────────(command OFF)────────────────────┘
```

In `TPO_ONLY` dT is forced satisfied: the stop line is `min_on` done, `TPO ≥ setpoint`, **or** the cooling backstop (TPO fell back to `setpoint − hysteresis` after peaking above it).

Bring-up firmware boots **Manual / coil OFF** (factory default — see Boot
behavior, below). `auto` enters Auto.

## Boot behavior (power-on / power-loss)

The node **boots in its last known user mode**, not a fixed default:

- **Persisted on every human action** (TFT menu or serial): `user_mode`,
  plus the commanded coil state when in **Manual** — Manual is the
  operator's explicit pump decision, and a power blip must not silently
  change it. Auto relay transitions are loop-owned and **never written**
  to NVS (wear stays human-rate).
- **Restored at boot:** the persisted mode resumes (and the Manual coil
  state with it). Auto re-derives its IDLE/RUNNING state from sensor
  readings — the cycle is not persisted, only the mode.
- **TEST is never persisted** (it is intrinsically transient): a reboot
  during Test comes up **Manual / coil OFF**.
- **All tunable parameters in the Parameters table persist to NVS on
  change** (setpoint, hysteresis, min on/off times,
  `min_tpo_tpu_delta_c`, `max_run_time_min`) and are restored at boot.
  They are field-adjustable from the local UI (Control Programming menu)
  and later over `PARAM_SET` — one validated setter path for both
  (DESIGN_NOTE_003 Parameters).
- **Off (`halt`) persists like Auto/Manual:** an operator who stopped the
  pump deliberately must not find it running after a power blip.
- **Factory-fresh NVS** boots **Manual / coil OFF**.

Field sequence: first boot → Manual/OFF; operator selects Auto on the
UI; every subsequent power loss reboots straight into Auto.

GPIO8: idle = 100 ms on / 900 ms off; RUNNING = steady on; any warning (including no-CT) / `FAULT` / `TPO_ONLY` / TPO unusable = 300 ms on/off.

The loop must run with USB, gateway, and broker absent.

## Faults

| Condition | Class | Mode |
|-----------|-------|------|
| TPO open, short, or not in −5–110 °C | **Critical** | `FAULT` (pump OFF) |
| TPU open, short, not in −5–110 °C, or TPU > TPO | **Severe** | `TPO_ONLY` |
| CT = none while RUNNING | **Warning** | stay on the standard algorithm (no `TPO_ONLY`) — CT-fitted images only; warn window fixed at 10 s (was the `ct_confirm_s` parameter, removed 2026-09-03 when the CT left the design — id 5 reserved) |
| CT sample unusable / not a clean running-vs-not | **Warning** | same — do not change start/stop — CT-fitted images only |
| CT present while relay OFF | **Warning** | no mode change — CT-fitted images only |
| CT not fitted (2026-09-01+, `ct_fitted = false`) | — | no CT warnings can fire; node runs blind to pump current by design (MES-BBU parity) |

No overcurrent / stall class. Protoboard retest (2026-08-22) and dummy AC load (2026-08-25) confirmed DESIGN_NOTE_001: loaded vs not only; magnitude is not judged. The old MES-BBU had **no** current sensor and ran the TPO/TPU loop blind to pump current. While a CT was fitted, missing or unreasonable CT was a **notice**, not a control-law change. From 2026-09-01 the node ships **without** a CT (issue 014): the contact only closes a 230 VAC contactor coil, so run-confirmation is impossible on this node — blindness is deliberate, MES-BBU parity, and `warn_noct` is gated off by `ct_fitted`. `TPO_ONLY` is **TPU faults only**.

`max_run_time_min` exceeded is a **warning only**.

TPU > TPO is severe only if `TPU > TPO + 1 °C` (mixed-tank slack).

## Plant picture (not firmware)

```text
  boiler jacket ──pump──► TPO (hot in at top)
       ▲                    │  stratified 1500 L
       └────────── TPU ─────┘  (cool out at bottom)
```

Cool TPU water into the jacket is what makes the boiler fire. No network in this picture.

## Out of scope for the first loop

- AMB in the start/stop decision (print only)
- Encoder / TFT **edits** of setpoints (`prog` + NVS first; menus are live/read for now except mode and Manual/Test pump) — **promoted into the field-deployable image**: full parameter editing from the local UI, parity with the future admin panel (issue 012)
- ESP-NOW / MQTT reporting of warnings/faults
- Real BBU pump on the bench sketch until 009 has a plant checklist
- CT overcurrent / ampere field (closed: binary only; no-CT is warn only)

## Related

- [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md) — CT boolean
- [DESIGN_NOTE_003](DESIGN_NOTE_003_espnow_node_schema.md) — ESP-NOW wire law (supervisory client, issue 011)
- [ADR_001](ADR_001.txt) — hardware
- Issue [009](../issues/open/009-offline-control-loop.md) — implementation
- Issue [012](../issues/open/012-field-install-readiness.md) — field-image checklist
