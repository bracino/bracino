# Design note 001 — Prototype CT is on/off only

**Status:** Accepted for the module prototype  
**Date:** 2026-08-14 (protoboard confirmed 2026-08-22)  
**Applies to:** breadboard / protoboard BBU node (ZMCT103C @ 3.3 V → ADS1115 A0)  
**Does not apply to:** a later discrete or 5 V analog front-end, if one is designed

## Decision

The phase-1 prototype **does not report pump current in amperes**. Firmware and any later MQTT/ESP-NOW field for this node should treat A0 as a boolean: **current present** vs **not**. Magnitude, inrush, and stall are out of scope on this analog path.

## Why

Bench work (2026-08-14) with a known AC fan (~0.12–0.15 A) and a hair dryer (0.84–7.8 A), one load conductor through the CT window, pot at 2 turns CCW from the fully-clockwise stop, ZMCT and ADS1115 both on 3.3 V:

| Condition | Typical A0 rms | Notes |
|-----------|----------------|--------|
| Relay off | ~0–1 mV | ADC/I2C quiet |
| Relay on, no load current | ~37 mV | 50 Hz pickup on the closed AC loop |
| ~0.15 A fan | ~175 mV | Clear gap vs idle; usable as “running” |
| 0.84 A dryer (motor-ish) | ~166 mV | **Below** the fan |
| ~2 A dryer (motor + low heat) | ~157 mV | Still below the fan; reproduced after a 4 A run |
| ~4 A dryer (high heat) | ~214 mV | `mid` sags (~766 mV) then crawls; not a proportional step |

The module is non-monotonic above a fraction of an amp. Extra pot gain only lifts the idle floor and hits the same output ceiling sooner (full-CW dryer table earlier the same day sat at 683 mV rms from 4 A and 7.8 A). The ADS1115 FSR (±4.096 V) never clipped; the ZMCT op-amp / bias network on 3.3 V is the limit. `mid` (bias) walks after large loads and is not a current reading.

Repeated on the **v0.08 protoboard** (2026-08-22), relay ON, same pot seat, `s` n=64. Human clamp-meter labels in the serial log:

| Load (clamp) | Typical A0 rms | Typical pp | mid |
|--------------|----------------|------------|-----|
| Contacts closed, no motor | 37–38 mV | 106 mV | ~912–920 mV |
| 0.13 A | 176–180 mV | ~508 mV | ~919 mV |
| 0.86 A | 175–177 mV | ~520 mV | 920 mV |
| 2.0 A | 168 mV | ~471 mV | ~918 mV |
| 3.6 A | 240–242 mV | ~732 mV | 928 mV |
| 4.1 A | 233–236 mV | ~709 mV | 893–914 mV (walks) |
| 7.8 A | 289–295 mV | ~910 mV | 922–929 mV |
| Back to no motor | 37–38 mV | 106 mV | ~915–920 mV |

Loaded vs not is a wide, repeatable gap (≈38 mV vs ≥168 mV). Above ~0.13 A the rms is **not** a usable ampere scale: 2.0 A sits **below** 0.13 A and 0.86 A; 3.6 A reads a bit above 4.1 A; `mid` walks. There is some rise at 4 A+ vs the 0.13–2 A cluster, but not enough to call overcurrent or stall. **Stay boolean.** The 80–100 mV rms threshold is unchanged.

The 3.3 V rail was a deliberate match to the ADC (A0 must stay ≤ 3.6 V). A 5 V ZMCT supply might recover some swing; it was not adopted for this prototype. Even then, half-wave / phase-controlled heater modes on the dryer already invert indicated vs clamp-meter amps — this board is not a wattmeter.

## Firmware contract (prototype)

- Sample A0 as AC rms around the instantaneous mid (existing bring-up `s` burst).
- Compare to a threshold in the idle-vs-fan gap. At the 2-CCW pot seat, **80–100 mV rms** sits between ~37 mV (contacts closed, no motor) and ~175 mV (fan running).
- After a large load, wait until `mid` stops walking before deciding “off”.
- Do **not** publish a calibrated ampere value from this path. A later discrete front-end can add a real current field without changing the boolean.

The real BBU pump is a plain induction motor (~350 W ≈ 1.5 A). Expect the fan-like side of the curve (clear on/off), not a linear amp reading.

## Related

- ADR 001 already flagged 3.3 V CT swing as a risk; this note records the measured outcome.
- Relay drive is a separate hardware path (Q1 2N3904 on GPIO10 → module IN in v0.08). It does not change this current-sense decision.
