# 002 — BBU BOM and schematic notes

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** related `001`, `hardware/bbu-controller/`

## Context

ADC path for two tank temperatures, current sensor for the 350W pump, solenoid/relay drive, protection, connectors. Protoboard first; KiCad when stable.

## Expected

BOM candidates + schematic notes (even hand-drawn scanned to fixtures/docs) covering sense and drive paths with approximate ranges and isolation if needed.

## Proposal

Draft after 001 boundaries; prefer parts already on the shelf when they meet range/accuracy.

KiCad **v0.08** BOM / netlist / ERC + pin-mapping CSV under `hardware/bbu-controller/`. Protoboard soldered to that schematic. Q1 2N3904 + R1 2 kΩ sit between GPIO10 and relay IN; module VCC is 5 V. TH1–TH3 + R4–R6 (10 kΩ) are the A1–A3 dividers. GPIO8 → D1 → R7 2.2 kΩ. D2 / R8 is the 12 V LED. **J7** is a 2-pin 5 V-only jumper (buck VO ↔ board +5 V), not the earlier 4-pin VIN+VO idea. Agents describe the circuit from those exports, not `.kicad_sch`.

Still on this issue only if the schematic itself moves: USB-blocking holder is mechanical, not a net change. F1 PPTC is drawn, not fitted.

## Fix

## Verify
