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

Breadboard sketch: `firmware/node-bbu/` (GPIO10 + ADS1115 A0). Serial `on`/`off`/`s`/`scan`.

Partial bench (2026-08-14): rails OK; I2C 0x48; CT pot 2 CCW — idle vs ~0.15 A fan is a clear gap; amp-level loads are not linear (DESIGN_NOTE_001). Relay at 5 V does not obey GPIO10; 3.3 V on the module is a temporary workaround.

Still needed for a plant checklist: transistor drive + fail-safe OFF at reset; NTC open/short behaviour; **server/gateway down** still correct; no unattended real BBU pump on the bring-up sketch.

## Fix

## Verify
