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

Settled enough to write I/O against: ADR 001 + KiCad **v0.08** (modules, pins, Q1 on relay IN, TH1–TH3 on A1–A3, GPIO8 LED, 12 V LED, J7 5 V-only jumper). CT is boolean (DESIGN_NOTE_001). Protoboard soldered and I/O-checked (heartbeat on external 5 V, relay switches loads, NTCs reasonable).

Human (2026-08-25): USB-blocking jumper holder **fabricated**; NTC cables **built and tested good**; sensors potted, curing. Remaining hardware: connector / jumper labels, PPTC when stock arrives. UI (TFT + encoder) on breadboard (010), not on this protoboard yet. Temperature calibration deferred until potting is done.

## Fix

## Verify
