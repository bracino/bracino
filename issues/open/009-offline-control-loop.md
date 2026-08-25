# 009 — Offline BBU control loop

- **Status:** open
- **Type:** task
- **Opened:** 2026-08-22
- **Refs:** `firmware/node-bbu/`, `006`, `docs/DESIGN_NOTE_002_bbu_control_loop.md`, `docs/DESIGN_NOTE_001_ct_binary_only.md`

## Context

Protoboard I/O is good enough to write against. The plant needs a local loop that runs with gateway / broker / USB unplugged.

Control law: [DESIGN_NOTE_002](../../docs/DESIGN_NOTE_002_bbu_control_loop.md). TPO = TH1/A1, TPU = TH2/A2, AMB = TH3/A3 (print only). CT is confirm-running only, and **warning only** (2026-08-25): no-current / unusable CT does **not** enter `TPO_ONLY` — stay on the standard TPO/TPU algorithm, same blindness as the old MES-BBU, plus a notice.

## Expected

On-node: NTC mV → °C with open/short as **fault**; CT as running / not; **Auto** IDLE/RUNNING; user modes Auto / Manual / Test / Off; overlays `FAULT` / `TPO_ONLY`. Serial debug / mode override. No ESP-NOW, MQTT, or webserver.

## Proposal

`control.c` implements the note. Boots MANUAL. `auto` / `sim` for desk proof. Human (2026-08-25): current image flashed and working; dummy AC load validated.

## Fix

`firmware/node-bbu/main/{control,ntc,params}.c`. Host tests in `firmware/node-bbu/test/test_control.c`.

## Verify

- [x] Host unit tests (`gcc` on `test_control.c`)
- [x] Desk `sim` walk-through on the protoboard (human, 2026-08-22): start, stay RUNNING with hot top / cold bottom, stop when charged, FAULT on bad TPO
- [x] Dummy AC load on the relay (human, 2026-08-25; not the real BBU pump). CT still binary, reliable for that
- [ ] °C vs a thermometer (sensors potted, curing; cal deferred)
- [ ] Real BBU pump — out of scope until the above
