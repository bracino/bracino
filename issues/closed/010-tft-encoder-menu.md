# 010 — TFT + encoder bring-up and MES-style menus

- **Status:** closed
- **Type:** task
- **Opened:** 2026-08-25
- **Closed:** 2026-08-28
- **Refs:** `firmware/node-bbu/` (`tft.c`, `enc.c`, `ui.c`), `001`, `009`, `docs/DESIGN_NOTE_002_bbu_control_loop.md`, ADR 001, v0.08 netlist

## Context

v0.08 brings the 1.8″ ST7735S + rotary encoder (with switch) out on dual 1×9 headers. Breadboard validation is done; **UI is on the protoboard** (2026-08-27).

User modes (no “Normal”): **Auto** (TPO/TPU loop), **Manual** (user coil), **Test** (Manual + 15 min → Auto), **Off** (`halt`; coil stays off). FAULT / TPO_ONLY are overlays.

## Expected

Landscape 160×128, 8-row grid (header / 6-row window / footer). Dark theme: black / cyan frame / white / yellow focus / amber edit (edit unused this pass). Font: **Modern DOS 8×16** (CC0; 20 columns, col 0 blank so the first glyph is on glass). Doubled 6×8 was rejected; 12×16 not used.

Hardware SPI2 + DMA via `esp_lcd` panel IO (CS tied GND → `cs_gpio_num = -1`). Encoder is GPIO ISR + gray-code table (C3 has no PCNT); switch stays software debounce. Control loop must keep running if the panel is unplugged or garbled.

Pins (v0.08 netlist): TFT SCK GPIO9, SDA GPIO4, A0/DC GPIO3, RESET GPIO2; ENC A GPIO0, B GPIO1, SW GPIO5. LED always on via R3.

## Proposal

Menus show DESIGN_NOTE_002 names (setpoint, hysteresis, min on/off, TPO−TPU off offset). Do not invent a separate MES “Diff on / Offset off” pair until the control law changes. Language / frost protection out of scope. Setpoint edits stay on serial `prog` for this pass; Control Program can change mode and Manual/Test pump.

C3 has no `pulse_cnt`. Do not add LVGL. Do not use in-tree `esp_lcd_new_panel_st7735` (not in IDF 5.2.3) — keep the proven ST7735 init and send pixels through `esp_lcd_panel_io_spi`.

## Fix

`firmware/node-bbu/main/{tft,enc,ui}.c`. Modes in `control.c`.

Bench path (2026-08-25 → 27): glyphs Y-mirrored then fixed; ISR gray-code encoder; Modern DOS 8×16; MADCTL MY|MV|BGR + software axis flips; col 0 blank left-clip workaround.

Session 2026-08-28 closed the remaining input/draw bugs:

1. **DMA glyph tear** (`27.9` → `7.9`): `esp_lcd` reuses color buffers only after `on_color_trans_done`. Wait on a binary semaphore before recycling `s_glyph` / strips (`fa7659f`).
2. **Per-glyph wait starved input**: full-row 160×16 blit (one DMA per text row) so redraws are fast again (`e454f60`).
3. **Switch bounce / menus → Home / hold dead**: stable-level debounce was not enough while UI blocked. Encoder switch runs on a **5 ms `esp_timer`** with **wall-clock** click (≥25 ms) and hold (~0.8 s); turn ignored while SW down. False mid-press release no longer emits click then hold→Home (`e454f60`).
4. **WARN amber looked deep blue**: MADCTL BGR — Adafruit-order orange `0xFD20` drove blue. Palette is BGR-aware via `COL_RGB565` (`ee3e6c3`).

Working tip commits: UI image through `ee3e6c3` (palette) on top of row-blit/timer (`e454f60`).

## Verify

- [x] Splash then Home on the breadboard TFT (human, 2026-08-25; after glyph + flicker fix)
- [x] Encoder rotate is detected; click enters (2026-08-25)
- [x] ISR gray-code encoder is usable (human, 2026-08-26 — “much better”)
- [x] Landscape rewrite loads and runs (human, 2026-08-26)
- [x] Replace doubled 6×8 with Modern DOS 8×16 (human, 2026-08-27 — readable on protoboard)
- [x] Orientation: upright LTR; col 0 blank as left-clip workaround (2026-08-27)
- [x] Encoder rotate reliable / snappy enough (human, 2026-08-28)
- [x] Switch: one click → one enter; hold ~0.8 s → Home (human, 2026-08-28)
- [x] Menus navigate: Temperatures, Counters, System Data, Control Prog, Diagnostics (human, 2026-08-28)
- [x] Control Program: Manual + pump ON; footer **WARN no CT** in amber (human, 2026-08-28)
- [x] Live temps stable (no disappearing digits) (human, 2026-08-28)
- [ ] Serial `st` with panel unplugged — not re-checked this close (control loop is independent of UI task; light residual)
- [ ] Real BBU pump — still out of scope
