# 001 — Control-node hardware definition

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** `docs/ADR_001.txt`, `docs/DESIGN_NOTE_001_ct_binary_only.md`, `hardware/bbu-controller/`, `firmware/node-bbu/`

## Context

Phase 1 needs a clear answer to what the BBU node monitors and controls, and how (sensors, drive, power, enclosure constraints in the boiler room).

## Expected

A short hardware definition good enough to write firmware I/O against and to start a BOM: tank temperature inputs, pump drive, current sense, power, debug/programming, ESP-NOW antenna reality (no WiFi in the room).

## Proposal

Work in chat / sketches; promote settled notes to `docs/` or `hardware/bbu-controller/`. Keep MCU choice swappable (`node-bbu` naming).

Settled enough to write I/O against: ADR 001 (modules, pins, 12 V → 5 V → 3.3 V). Remaining: 5 V relay IN vs GPIO10 (transistor; schematic revision in progress). CT is boolean on this proto (DESIGN_NOTE_001). NTCs not brought up yet.

## Fix

## Verify
