# 010 — TFT + encoder bring-up and MES-style menus

- **Status:** open
- **Type:** task
- **Opened:** 2026-08-25
- **Refs:** `firmware/node-bbu/` (`tft.c`, `enc.c`, `ui.c`), `001`, `009`, `docs/DESIGN_NOTE_002_bbu_control_loop.md`, ADR 001, v0.08 netlist

## Context

v0.08 brings the 1.8″ ST7735S + rotary encoder (with switch) out on dual 1×9 headers. Human has them on a breadboard for validation before protoboard.

User modes (no “Normal”): **Auto** (TPO/TPU loop), **Manual** (user coil), **Test** (Manual + 15 min → Auto), **Off** (`halt`; coil stays off). FAULT / TPO_ONLY are overlays.

## Expected

Bit-bang TFT (CS tied GND) and encoder A/B/SW with internal pull-ups. Splash, then a rough MES-style tree: Home, Selection, Temperatures, Counters, System Data, Control Program, Diagnostics. Control loop must keep running if the panel is unplugged or garbled.

Pins (v0.08 netlist): TFT SCK GPIO9, SDA GPIO4, A0/DC GPIO3, RESET GPIO2; ENC A GPIO0, B GPIO1, SW GPIO5. LED always on via R3.

## Proposal

Menus show DESIGN_NOTE_002 names (setpoint, hysteresis, min on/off, TPO−TPU off offset). Do not invent a separate MES “Diff on / Offset off” pair until the control law changes. Language / frost protection out of scope. Setpoint edits stay on serial `prog` for this pass; Control Program can change mode and Manual/Test pump.

## Fix

`firmware/node-bbu/main/{tft,enc,ui}.c`. Modes in `control.c`.

## Verify

- [ ] Splash then Home on the breadboard TFT
- [ ] Encoder rotate moves `>` ; click enters; hold ~0.8 s returns Home
- [ ] Control Program cycles Auto / Manual / Test / Off; pump toggle only in Manual/Test
- [ ] Loop still ticks with the panel disconnected (serial `st`)
- [ ] Real BBU pump — still out of scope
