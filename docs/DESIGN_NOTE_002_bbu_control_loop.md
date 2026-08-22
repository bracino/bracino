# Design note 002 — BBU offline control loop (emulate MES-BBU)

**Status:** Implemented. Desk `sim` walk-through passed (2026-08-22). Dummy-load / plant proof still open.  
**Date:** 2026-08-22  
**Applies to:** `firmware/node-bbu` local loop on the v0.08 protoboard  
**Does not apply to:** gateway, MQTT, UI encoder, or a later ACS node

## What we are emulating

The failed Paradigma MES-BBU ran the boiler→buffer pump against a **stratified 1500 L store**. The boiler itself is independent: it fires from **jacket temperature**. When the BBU pump floods that jacket with cooler tank water, the boiler comes on. The controller’s job is to start a loading cycle when the **top** of the tank has gone cold, and to keep the pump on long enough that the thermocline is pushed **down**, not just to reheat the top 20 cm.

The old box had a pump relay and two tank wells. It did **not** have a current sensor. Bracino adds a CT for confirm / reporting (boolean on this prototype — [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md)).

## Sensors

| Name | Well / role | Hardware (assumed) | In NORMAL? |
|------|-------------|--------------------|------------|
| **TPO** | Top of tank (NTC1) | TH1 → ADS1115 A1 | Start / stop |
| **TPU** | Bottom of tank (NTC2) | TH2 → ADS1115 A2 | Stop (thermocline); inversion check |
| **AMB** | Ambient (NTC3) | TH3 → ADS1115 A3 | **Reported only** — never start/stop/fault |
| **CT** | Pump conductor | ZMCT → A0 | Confirm running / not; not amperes |

Correct this table if the physical wells are on different TH pads.

Valid converted range for TPO/TPU: **0–95 °C**. Open, short, or out of that range is a fault, not a temperature. (Hardware rails from bench: open ≈ 13 mV, short ≈ 3283 mV; treat mid ≲ 50 mV or ≳ 3200 mV as unusable before conversion.)

NTC model: **β = 3950**, R25 = 10 kΩ (assumed NRBE 10 k / 3950; 28 °C ≈ 1760 mV matches this). AMB uses the same conversion for the serial print.

## Parameters

Tune on site. Values below are starting guesses (old-controller memory plus new safety timers).

| Parameter | Default | Role |
|-----------|---------|------|
| `tpo_setpoint_c` | 60.0 | Target top-of-tank temperature |
| `hysteresis_c` | 3.0 | Band around the setpoint |
| `min_on_time_s` | 180 | Anti-short-cycle once RUNNING |
| `min_off_time_s` | 60 | Anti-short-cycle once IDLE |
| `ct_confirm_s` | 10 | After command ON, time allowed for CT = present |
| `min_tpo_tpu_delta_c` | 5.0 | Small top−bottom gap means the tank is largely charged |
| `max_run_time_min` | 60 | Longest normal load; **warning only**, pump keeps its state |

Derived, not stored:

- `tpo_on_threshold` = `tpo_setpoint_c − hysteresis_c / 2` → **58.5 °C** with the defaults
- `tpo_off_threshold` = `tpo_setpoint_c + hysteresis_c / 2` → **61.5 °C** with the defaults

A **small** `(TPO − TPU)` means the thermocline has collapsed / the store is full. A **large** delta means hot top, cold bottom — still room to charge.

## Why TPU is in the stop condition

While loading, TPO rises quickly (hot flow enters the top). Stopping on TPO alone reheats a thin layer and drops back to IDLE — the classic short cycle on a tall tank. The old behaviour was: keep pumping until **TPU also comes up**, i.e. a useful volume has been pushed down. Formalised as `(TPO − TPU) ≤ min_tpo_tpu_delta_c` **and** TPO already at/above the off threshold.

The boiler may drop out on its own jacket long before that. The pump can still run; that is fine.

## Modes and states

**Mode** is how the box is being used. **State** is the pump cycle inside a mode that is allowed to run.

| Mode | Who selects | Pump |
|------|-------------|------|
| `NORMAL` | Default | IDLE / RUNNING machine below |
| `TPO_ONLY` | Automatic on a **severe** fault | Same machine, TPU and delta ignored |
| `MANUAL` | User | On/off as commanded; no auto start/stop |
| `TESTING` | User | On/off as commanded; **15 min** then back to `NORMAL` |
| `FAULT` | Automatic on a **critical** fault | Forced **OFF** |

`TPO_ONLY` and `FAULT` return to `NORMAL` when the raising fault is gone. `TESTING` expires to `NORMAL`. `MANUAL` stays until the user leaves it.

Inside `NORMAL` (and `TPO_ONLY`):

```text
IDLE --(TPO ≤ tpo_on_threshold && off-timer expired)──► RUNNING
  ▲                                                     │
  │                                                     │ min_on_time done
  │                                                     │ && TPO ≥ tpo_off_threshold
  │                                                     │ && (TPO − TPU) ≤ min_tpo_tpu_delta_c
  └──────────────────────(command OFF)──────────────────┘
```

In `TPO_ONLY` the stop line is only `min_on_time` done **and** `TPO ≥ tpo_off_threshold`.

Bring-up firmware **boots `MANUAL` / coil OFF** so CT and load tests are not seized. `auto` enters `NORMAL`. Production default can become `NORMAL` once the loop is proven.

GPIO8: idle = 100 ms on / 900 ms off; RUNNING = steady on; any warning / `FAULT` / `TPO_ONLY` / TPO unusable = 300 ms on/off.

The loop must run with USB, gateway, and broker absent.

## Faults

| Condition | Class | Mode |
|-----------|-------|------|
| TPO open, short, or not in 0–95 °C | **Critical** | `FAULT` (pump OFF) |
| CT **present** is not required here as critical | — | — |
| TPU open, short, not in 0–95 °C, or TPU > TPO | **Severe** | `TPO_ONLY` |
| CT = none while RUNNING, after `ct_confirm_s` | **Severe** | `TPO_ONLY` |

No overcurrent / stall class. Protoboard retest (2026-08-22) confirmed DESIGN_NOTE_001: loaded vs not only. The CT **is** used for “commanded ON but no current after `ct_confirm_s`” (severe → `TPO_ONLY`) and “CT present while relay OFF” (**warning only**, no mode change; same blindness the old box had, plus a notice).

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
- Encoder / TFT setpoints (`prog` + NVS first)
- ESP-NOW / MQTT reporting of warnings/faults
- Real BBU pump on the bench sketch until 009 is proven on dummy loads
- CT overcurrent / ampere field (closed: binary only)

## Related

- [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md) — CT boolean
- [ADR_001](ADR_001.txt) — hardware
- Issue [009](../issues/open/009-offline-control-loop.md) — implementation
