# 010 — TFT + encoder bring-up and MES-style menus

- **Status:** open
- **Type:** task
- **Opened:** 2026-08-25
- **Refs:** `firmware/node-bbu/` (`tft.c`, `enc.c`, `ui.c`), `001`, `009`, `docs/DESIGN_NOTE_002_bbu_control_loop.md`, ADR 001, v0.08 netlist

## Context

v0.08 brings the 1.8″ ST7735S + rotary encoder (with switch) out on dual 1×9 headers. Human has them on a breadboard for validation before protoboard.

User modes (no “Normal”): **Auto** (TPO/TPU loop), **Manual** (user coil), **Test** (Manual + 15 min → Auto), **Off** (`halt`; coil stays off). FAULT / TPO_ONLY are overlays.

## Expected

Landscape 160×128, 8-row grid (header / 6-row window / footer). Dark theme: black / cyan frame / white / yellow focus / amber edit (edit unused this pass). Font: a **real** 12×16 bitmap (13 columns) — doubled 6×8 was rejected on the bench.

Hardware SPI2 + DMA via `esp_lcd` panel IO (CS tied GND → `cs_gpio_num = -1`). Encoder is GPIO ISR + gray-code table (C3 has no PCNT); switch stays software debounce. Control loop must keep running if the panel is unplugged or garbled.

Pins (v0.08 netlist): TFT SCK GPIO9, SDA GPIO4, A0/DC GPIO3, RESET GPIO2; ENC A GPIO0, B GPIO1, SW GPIO5. LED always on via R3.

## Proposal

Menus show DESIGN_NOTE_002 names (setpoint, hysteresis, min on/off, TPO−TPU off offset). Do not invent a separate MES “Diff on / Offset off” pair until the control law changes. Language / frost protection out of scope. Setpoint edits stay on serial `prog` for this pass; Control Program can change mode and Manual/Test pump.

C3 has no `pulse_cnt`. Do not add LVGL. Do not use in-tree `esp_lcd_new_panel_st7735` (not in IDF 5.2.3) — keep the proven ST7735 init and send pixels through `esp_lcd_panel_io_spi`.

## Fix

`firmware/node-bbu/main/{tft,enc,ui}.c`. Modes in `control.c`.

Bench 2026-08-25: glyphs were Y-mirrored (ST7735 MY vs char RAM order); 2 Hz dark bar was full-row `fill_rect` at 500 ms; Home had only one cursor item so rotation looked dead. Glyphs sent bottom-first; live lines are cached (no row wipe). Serial `enc` prints A/B/SW. Gamma tables added. Human: LED brighter with **R3 = 100 Ω** (was 220). Old A-edge poll was bouncy; module is fine.

Human 2026-08-26: `c9ca914` loads and runs. ISR quadrature encoder is **much better**. Doubled 6×8 font is **unacceptable** — replace before more menu polish.

## Verify

- [x] Splash then Home on the breadboard TFT (human, 2026-08-25; after glyph + flicker fix)
- [x] Encoder rotate is detected; click enters (2026-08-25)
- [x] ISR gray-code encoder is usable (human, 2026-08-26 — “much better”)
- [x] Landscape rewrite loads and runs (human, 2026-08-26)
- [ ] Replace doubled 6×8 with a real 12×16 bitmap (human: current font unacceptable)
- [ ] MADCTL/gaps if the image is shifted or mirrored
- [ ] System Data (8 items) scrolls the 6-row window
- [ ] Control Program cycles Auto / Manual / Test / Off; pump toggle only in Manual/Test
- [ ] Loop still ticks with the panel disconnected (serial `st`)
- [ ] Real BBU pump — still out of scope
