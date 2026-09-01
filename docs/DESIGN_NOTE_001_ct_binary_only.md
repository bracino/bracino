# Design note 001 — Prototype CT is on/off only

**Status:** Accepted for the module prototype. **Amended 2026-09-01: the CT is dropped from the circuit entirely for field revs** (snubber too) — see *Revision 2* below. The bench boolean contract stands as bench history only.
**Date:** 2026-08-14 (protoboard confirmed 2026-08-22; dropped from circuit 2026-09-01)  
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

Dummy **AC load** on the v0.08 protoboard relay (human, 2026-08-25): still binary only, and **reliable for that**. No ampere field. Control-loop use of this boolean is warn-only (no `TPO_ONLY`) — [DESIGN_NOTE_002](DESIGN_NOTE_002_bbu_control_loop.md).

The 3.3 V rail was a deliberate match to the ADC (A0 must stay ≤ 3.6 V). A 5 V ZMCT supply might recover some swing; it was not adopted for this prototype. Even then, half-wave / phase-controlled heater modes on the dryer already invert indicated vs clamp-meter amps — this board is not a wattmeter.

## Firmware contract (prototype)

- Sample A0 as AC rms around the instantaneous mid (existing bring-up `s` burst).
- Compare to a threshold in the idle-vs-fan gap. At the 2-CCW pot seat, **80–100 mV rms** sits between ~37 mV (contacts closed, no motor) and ~175 mV (fan running).
- After a large load, wait until `mid` stops walking before deciding “off”.
- Do **not** publish a calibrated ampere value from this path. A later discrete front-end can add a real current field without changing the boolean.

The real BBU pump is a plain induction motor (~350 W ≈ 1.5 A). Expect the fan-like side of the curve (clear on/off), not a linear amp reading.

## Plant finding (2026-08-31)

First boiler-room hookup revealed the node's contact does **not** switch the pump directly: it closes a control line to a hidden second relay in the breaker panel, which switches the pump. Pump current therefore **never flows through node wiring**, and the CT cannot see the plant motor on this node at all. The boolean-only contract above still stands as bench evidence, but the assumed plant signal is gone.

**Resolved 2026-09-01 (boiler room + bench):** the hidden relay is an **ABB ECB24-40** contactor in the breaker panel; the line our contact closes is **230 VAC** (the earlier ~24 V note was wrong), i.e. the coil current is so small the ZMCT sees nothing — the node would show `NO_CURRENT_WARN` forever. With the RC snubber lifted from our contacts, the pump switches correctly from the node (the earlier stuck-ON failure was the snubber leak holding the contactor coil, as suspected). Decision: **both the CT and the snubber are left out of the circuit**; the next schematic bump reflects that; firmware ignores A0 and reports `ct_state = NOT_FITTED`. A0 stays reserved for repurposing in a later rev. Run-confirmation returns with the future **caldaia monitor node** (phase 2): multiple CTs on pump / fuel auger / blower plus extra thermals, which also restores true pump-current sensing the BBU node lost. Tracked in issues/open/014 (now closed) and issues/open/012.

## Revision 2 (2026-09-01) — CT dropped from the circuit

- The ZMCT103C + potential divider come **off the board** (v0.09+); **A0 is unconnected / reserved**.
- Firmware: `CT_FITTED 0` in `main.c` — the monitor skips A0 sampling, telemetry reports `ct_state = 3 (NOT_FITTED)` (DN003), and the no-CT warning can never fire (`ct_fitted` gate in the control loop).
- The snubber across the relay contacts is also removed (plant-proven above): across open contacts in series with a coil it is a permanent leak path. If suppression is ever needed, it goes **across the coil**, never across our contacts.
- The boolean bench contract (thresholds, tables above) remains the record of what the CT *could* do on the bench; it is **not** a field capability of this node any more.

## Related

- ADR 001 already flagged 3.3 V CT swing as a risk; this note records the measured outcome.
- 014 — hidden second pump relay; CT plant signal unavailable.
- Relay drive is a separate hardware path (Q1 2N3904 on GPIO10 → module IN in v0.08). It does not change this current-sense decision.
