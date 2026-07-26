# 001 — Control-node hardware definition

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** `project_slug.md`, `hardware/bbu-controller/`, `firmware/node-bbu/`

## Context

Phase 1 needs a clear answer to what the BBU node monitors and controls, and how (sensors, drive, power, enclosure constraints in the boiler room).

## Expected

A short hardware definition good enough to write firmware I/O against and to start a BOM: tank temperature inputs, pump drive, current sense, power, debug/programming, ESP-NOW antenna reality (no WiFi in the room).

## Proposal

Work in chat / sketches; promote settled notes to `docs/` or `hardware/bbu-controller/`. Keep MCU choice swappable (`node-bbu` naming).

## Fix

## Verify
