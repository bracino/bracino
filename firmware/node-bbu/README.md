# node-bbu

Phase 1 BBU (thermal storage) pump control node firmware.

Hardware target is currently ESP32-class (often ESP32-C3 on the bench); the directory name is MCU-neutral on purpose.

See root `README.md`, `AGENTS.md`. Kickoff history (stale OK): `docs/project_slug.md`.

## Breadboard bring-up (now)

What is in this tree today is a **bench sketch**, not the control loop. It toggles the pump relay and reads ADS1115 A0–A3 (ZMCT103C + three NTC dividers).

Pins match schematic v0.08 (`hardware/bbu-controller/bbu_controller_prototype_kicad/` — use the BOM/netlist, not `.kicad_sch`):

| Net | Pin | Notes |
|-----|-----|--------|
| RELAY | GPIO10 | Via Q1 2N3904 (R1 2 kΩ base): GPIO10 **high** = coil ON. Boot holds the pad **low**. On-board WS2812 unused (stays dark). |
| HEART | GPIO8 | ~1 Hz LED (D1 + R7 2.2 kΩ to GND). |
| ADC_SDA | GPIO7 | ADS1115 SDA (module 10 kΩ pull-ups) |
| ADC_SCL | GPIO6 | ADS1115 SCL |
| A0 | ADS1115 AIN0 | ZMCT103C `OUT` |
| A1–A3 | ADS1115 AIN1–3 | TH1/R4, TH2/R5, TH3/R6 (10 kΩ NTC + 10 kΩ to GND) |
| I2C addr | 0x48 | Module ADDR pulled low |

Bench (2026-08-15 breadboard, repeated 2026-08-22 on the v0.08 protoboard): coil toggles as commanded and holds; protoboard also switches bench loads. Lab **28 °C**: A1–A3 mid ~1760 mV, rms 0; warming raises mid, cooling lowers it. A3 open → 13 mV; A3 short → 3283 mV. CT ~0.13 A ≈ 170 mV rms vs ~37 mV contacts-closed / no load. Heartbeat confirmed on external 5 V (USB unplugged). Do not put the real BBU pump on this sketch.

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
| `on` / `off` / `t` | Relay on, off, toggle (GPIO10 high = ON) |
| `r` | One sample of A0–A3 (mV + raw counts) |
| `r0`…`r3` | One sample of that channel |
| `s` | 64-sample A0 burst: mid / AC rms / peak-to-peak — use this for the pot |
| `s0`…`s3` | Same burst on that channel (`mid` on A1–A3 is the NTC tap) |
| `scan` | I2C probe |
| `h` | Help |

CT on this prototype is **on/off only** ([DESIGN_NOTE_001](../../docs/DESIGN_NOTE_001_ct_binary_only.md)). At pot = 2 CCW, 3.3 V: relay off ≈ 0 mV rms; contacts closed / no motor ≈ 37 mV; ~0.15 A fan ≈ 175 mV. A threshold around 80–100 mV is the intended discriminator. On A0, `mid` is bias, not current; it walks after large loads.

Free-text lines (`ambient`, `open`, …) are not commands; the sketch prints `unknown`.

GPIO8 blinks with or without USB. Flash over USB with **J7 out** (buck VO isolated from the 5 V rail). Do not plug USB while J7 is in. A mechanical USB-blocking holder is not built yet.

Monitor quit: **Ctrl+]**. If the ACM node vanishes after reset, `ls /dev/ttyACM*` and `idf.py -p … flash` again — do not fullclean.
