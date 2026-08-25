# 003 — Hardware test and verification strategy

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** related `001`, `002`, `firmware/node-bbu/`

## Context

Need a repeatable way to prove pump logic and I/O without endangering the plant — bench loads, simulated temps, current-sense checks, fail-safe tests (sensor open/short).

## Expected

A short checklist: bring-up steps, must-pass cases before leaving the loop unattended on the real BBU pump, including **server/gateway down** still correct.

## Proposal

Document under `docs/` once drafted; link from STATUS.

Bring-up sketch: `firmware/node-bbu/` (GPIO10 via Q1 + ADS1115 A0–A3 + GPIO8 heartbeat). Serial `on`/`off`/`r`/`r0`–`r3`/`s`/`s0`–`s3`/`scan`.

Breadboard (2026-08-14 / 15): rails OK; I2C 0x48; CT pot 2 CCW — idle vs ~0.15 A fan is a clear gap; amp-level loads are not linear (DESIGN_NOTE_001). Q1 + 5 V module; coil toggles and holds. NTC at lab **28 °C** ~1760 mV; A3 **open** 13 mV; A3 **short** 3283 mV. CT ~0.13 A → ~170 mV rms; no load → ~37 mV. 18–95 °C fits the divider.

Protoboard (human-reported 2026-08-22): same suite passed — heartbeat on external 5 V, relay switches loads, coil and NTCs reasonable.

Desk `sim` of the loop passed (2026-08-22). Dummy **AC load** on the relay validated (human, 2026-08-25): CT still binary, reliable for running/not. NTC cables built and good; sensors potted. Ice / boil two-point (2026-08-25): ice ~0.4 °C; boil ~100 °C was FAULT on the 95 °C cap. Still needed: **server/gateway down** still correct; plant checklist before an unattended real BBU pump. Do not hang the real pump on this image yet.

## Fix

## Verify
