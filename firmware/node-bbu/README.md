# node-bbu

Phase 1 BBU (thermal storage) pump control node firmware.

Hardware target is currently ESP32-class (often ESP32-C3 on the bench); the directory name is MCU-neutral on purpose.

See root `README.md`, `AGENTS.md`. Kickoff history (stale OK): `docs/project_slug.md`.

## Breadboard bring-up (now)

What is in this tree today is a **bench sketch**, not the control loop. It toggles the pump relay and reads ADS1115 channel A0 (ZMCT103C) so the CT pot can be set before NTCs go on.

Pins match schematic v0.06 (`hardware/bbu-controller/bbu_controller_prototype_kicad/`):

| Net | Pin | Notes |
|-----|-----|--------|
| RELAY | GPIO10 | **Today (v0.06 / no transistor):** active-low into module `IN`. **After the NPN:** invert — GPIO10 high = coil on. Also the unused on-board WS2812. |
| ADC_SDA | GPIO7 | ADS1115 SDA (module 10 kΩ pull-ups) |
| ADC_SCL | GPIO6 | ADS1115 SCL |
| A0 | ADS1115 AIN0 | ZMCT103C `OUT` |
| I2C addr | 0x48 | Module ADDR pulled low |

Relay is **forced GPIO10 high at boot**. That is OFF only while IN is driven directly (and only if the 3.3 V module workaround is in use). After the transistor, high = ON — invert `relay_set()` before any AC load. Do not put the real BBU pump on this sketch.

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
| `on` / `off` / `t` | Relay on, off, toggle |
| `r` | One A0 sample (mV + raw counts) |
| `s` | 64-sample burst: mid / AC rms / peak-to-peak — use this for the pot |
| `scan` | I2C probe |
| `h` | Help |

CT on this prototype is **on/off only** ([DESIGN_NOTE_001](../../docs/DESIGN_NOTE_001_ct_binary_only.md)). At pot = 2 CCW, 3.3 V: relay off ≈ 0 mV rms; contacts closed / no motor ≈ 37 mV; ~0.15 A fan ≈ 175 mV. A threshold around 80–100 mV is the intended discriminator. `mid` is bias, not current; it walks after large loads.

Suggested next bench steps: transistor on relay IN (then invert GPIO) → NTCs on A1–A3.
