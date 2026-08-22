# 009 — Offline BBU control loop

- **Status:** open
- **Type:** task
- **Opened:** 2026-08-22
- **Refs:** `firmware/node-bbu/`, `006`, `docs/DESIGN_NOTE_002_bbu_control_loop.md`, `docs/DESIGN_NOTE_001_ct_binary_only.md`

## Context

Protoboard I/O is good enough to write against. The plant needs a local loop that runs with gateway / broker / USB unplugged.

Control law: [DESIGN_NOTE_002](../../docs/DESIGN_NOTE_002_bbu_control_loop.md). TPO = TH1/A1, TPU = TH2/A2, AMB = TH3/A3 (print only). CT is confirm-running only.

## Expected

On-node: NTC mV → °C with open/short as **fault**; CT as running / not; `NORMAL` IDLE/RUNNING; `FAULT` / `TPO_ONLY` / `MANUAL` / `TESTING`. Serial debug / mode override. No ESP-NOW, MQTT, or webserver.

## Proposal

`control.c` implements the note. Boots MANUAL. `auto` / `sim` for desk proof.

## Fix

`firmware/node-bbu/main/{control,ntc,params}.c`. Host tests in `firmware/node-bbu/test/test_control.c`.

## Verify

- [x] Host unit tests (`gcc` on `test_control.c`)
- [x] Desk `sim` walk-through on the protoboard (human, 2026-08-22): start, stay RUNNING with hot top / cold bottom, stop when charged, FAULT on bad TPO
- [ ] °C vs a thermometer
- [ ] Dummy AC load on the relay (not the real BBU pump)
- [ ] Real BBU pump — out of scope until the above
