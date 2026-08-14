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

KiCad v0.06 + pin-mapping CSV exist under `hardware/bbu-controller/`. Next schematic rev: NPN (or N-FET) + base resistor between GPIO10 and relay IN; module VCC remains 5 V. Add that part to the BOM when the symbol is in.

## Fix

## Verify
