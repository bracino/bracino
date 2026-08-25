# node-bbu

Phase 1 BBU (thermal storage) pump control node firmware.

Hardware target is currently ESP32-class (often ESP32-C3 on the bench); the directory name is MCU-neutral on purpose.

See root `README.md`, `AGENTS.md`. Kickoff history (stale OK): `docs/project_slug.md`.

## Bring-up (now)

Local loop lives in `control.c` ([DESIGN_NOTE_002](../../docs/DESIGN_NOTE_002_bbu_control_loop.md)). **Boots Manual** / coil OFF. User modes: **Auto** / **Manual** / **Test** / **Off** (there is no Normal). `auto` runs the TPO/TPU loop. `halt` is Off. A1=TPO, A2=TPU, A3=AMB (printed, never used for control). NTC β=**3950**. CT is loaded / not.

TFT + encoder (breadboard, issue 010): bit-bang ST7735S on GPIO9 SCK / GPIO4 SDA / GPIO3 DC / GPIO2 RESET (CS tied GND). Encoder A/B/SW = GPIO0 / GPIO1 / GPIO5, internal pull-ups. Click enters; hold ~0.8 s returns Home. Home shows `enc N` while you turn; serial `enc` prints A/B/SW.

Pins match schematic v0.08 (`hardware/bbu-controller/bbu_controller_prototype_kicad/` — use the BOM/netlist, not `.kicad_sch`):

| Net | Pin | Notes |
|-----|-----|--------|
| RELAY | GPIO10 | Via Q1 2N3904 (R1 2 kΩ base): GPIO10 **high** = coil ON. Boot holds the pad **low**. On-board WS2812 unused (stays dark). |
| HEART | GPIO8 | Idle 100 ms on / 900 ms off; RUNNING steady; alert 300/300 ms. |
| TFT_SCK | GPIO9 | ST7735S SCK (bit-bang). Also BOOT strap — idle high after reset. |
| TFT_SDA | GPIO4 | ST7735S MOSI |
| TFT_AO | GPIO3 | ST7735S DC (A0) |
| TFT_RESET | GPIO2 | ST7735S RESET; 4.7 kΩ pull-up. CS tied GND. LED always on. |
| ENC_A / ENC_B / ENC_SW | GPIO0 / GPIO1 / GPIO5 | Internal pull-ups. SW to GND. |
| ADC_SDA | GPIO7 | ADS1115 SDA (module 10 kΩ pull-ups) |
| ADC_SCL | GPIO6 | ADS1115 SCL |
| A0 | ADS1115 AIN0 | ZMCT103C `OUT` |
| A1–A3 | ADS1115 AIN1–3 | TH1/R4, TH2/R5, TH3/R6 (10 kΩ NTC + 10 kΩ to GND) |
| I2C addr | 0x48 | Module ADDR pulled low |

Bench (2026-08-15 breadboard, repeated 2026-08-22 on the v0.08 protoboard): coil toggles as commanded and holds; protoboard also switches bench loads. Lab **28 °C**: A1–A3 mid ~1760 mV, rms 0; warming raises mid, cooling lowers it. A3 open → 13 mV; A3 short → 3283 mV. CT ~0.13 A ≈ 170 mV rms vs ~37 mV contacts-closed / no load. Heartbeat confirmed on external 5 V (USB unplugged). Human (2026-08-25): current image flashed and working; dummy AC load on the relay validated (CT still binary, reliable for running/not); NTC cables built; sensors potted. Ice water ~0.4 °C (770 mV); boiling ~100 °C (3083 mV). Conversion range **−5–110 °C** so boil is a temperature, not FAULT. Do not put the real BBU pump on this sketch.

### Build / flash

Shared IDF lives under `~/projects/share` (not vendored here). C3-Zero uses native USB (`/dev/ttyACM0`); the port may vanish for a few seconds after reset.

```bash
. ~/projects/share/lib/esp/esp-idf/export.sh
cd firmware/node-bbu
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

If `set-target` was already run for another chip, `idf.py fullclean` first.

### Serial commands (115200, newline)

| Cmd | Action |
|-----|--------|
| `on` / `off` / `t` | Relay on, off, toggle — forces **MANUAL** |
| `r` | A0 mV; TPO/TPU/AMB as °C and mV |
| `r0`…`r3` | One channel |
| `s` | 64-sample A0 burst: mid / AC rms / p-p — use this for CT tests |
| `s0`…`s3` | Same burst; A1–A3 also print °C from mid |
| `auto` / `manual` / `test` / `halt` | Auto loop / sticky Manual / 15 min then Auto / Off (coil stays off) |
| `sim tpo 55` / `sim tpu 30` / `sim clear` | Inject tank temps for desk proof of start/stop |
| `prog` | Programming mode: `NAME VALUE`, `save`, `default`, `exit` |
| `st` | Mode, cycle, warnings, params |
| `scan` | I2C probe |
| `h` | Help |

CT on this prototype is **on/off only** ([DESIGN_NOTE_001](../../docs/DESIGN_NOTE_001_ct_binary_only.md)). At pot = 2 CCW, 3.3 V: relay off ≈ 0 mV rms; contacts closed / no motor ≈ 37 mV; ~0.15 A fan ≈ 175 mV. A threshold around 80–100 mV is the intended discriminator. On A0, `mid` is bias, not current; it walks after large loads. Missing or unusable CT after `ct_confirm_s` is a **warning only** — the TPO/TPU loop keeps running ([DESIGN_NOTE_002](../../docs/DESIGN_NOTE_002_bbu_control_loop.md)).

Free-text lines (`ambient`, `open`, …) are not commands; the sketch prints `unknown`.

GPIO8: idle flash, steady when the relay is on, rapid if a warning/fault is latched (stuck-on, no-CT, max run time, TPO bad, TPO_ONLY, FAULT). Flash over USB with **J7 out**. Do not plug USB while J7 is in.

`prog` names: `tpo_setpoint_c`, `hysteresis_c`, `min_on_time_s`, `min_off_time_s`, `ct_confirm_s`, `min_tpo_tpu_delta_c`, `max_run_time_min`. `save` writes NVS.

Monitor quit: **Ctrl+]**. If the ACM node vanishes after reset, `ls /dev/ttyACM*` and `idf.py -p … flash` again — do not fullclean.
