# Bench gotchas

Small hardware/toolchain snags that aren't worth an issue note each.

## ESP32-C3 flash erase with `CONFIG_ESPTOOLPY_NO_STUB=y` (2026-09-02)

The node image is built with `CONFIG_ESPTOOLPY_NO_STUB=y` (C3 USB
re-enumeration reliability), so `idf.py` drives the **ROM bootloader
only** — and the C3 ROM does not implement full `erase_flash`.
`idf.py erase_flash` fails with:

```
A fatal error occurred: ESP32-C3 ROM does not support function erase_flash.
```

Erase just what you need via `erase_region` (ROM-supported):

```bash
python -m esptool --chip esp32c3 -p /dev/ttyACM0 --no-stub erase_region 0x9000 0x6000
```

Default single-app layout: NVS at `0x9000`, size `0x6000`. This is the
"factory-fresh NVS" test for node-bbu (boot blob `bbu/boot` + params
blob `p1` both gone → Manual / coil OFF, default params).

A plain `python -m esptool ... erase_flash` (stub mode) also works, but
stub flashing was deliberately disabled in sdkconfig — prefer the region
erase.

## Bench channel: never default to ch 6 (2026-09-02)

The local house AP sits on **channel 6** (heavy ambient traffic — the
2026-08-31 sniff counted 452 frames/10 s). At desk range its traffic
occludes ESP-NOW HELLO/ACK exchanges. Hopping survey (2026-09-02,
~8 SSIDs visible, clustered around 4–7): **ch 6 effectively unusable**;
ch 1, 3, 11 bind quick and reliably even with two HELLO shots per scan
dwell. GW default is now `DEFAULT_CH 1` (bench master; the install's
target WiFi is on ch 1 and unlikely to change). Keep ch 6 out of bench
drills unless testing occlusion deliberately; field DN004 channel choice
must stay off the house AP's channel (re-survey at install).

## `pdMS_TO_TICKS(5)` is 0 at `CONFIG_FREERTOS_HZ=100` (2026-09-03)

`5 * 100 / 1000` truncates to 0. `vTaskDelay(0)` yields but does **not**
block, so a prio-1 task stays Ready and IDLE never runs — TWDT on IDLE
while the dump shows that task at loop top (enc_take_hold return,
2026-09-03 poll-fix soak). Use `vTaskDelay(1)` (1 tick = 10 ms) or
`pdMS_TO_TICKS(10)`. esp_timer periods in microseconds are unaffected.

## Encoder A/B lines: never a GPIO ISR (2026-09-02 / 03)

Symptom: intermittent task-WDT fire — IDLE starved, `ui` listed as the
running task, garbage register dumps. Provoked by touching / turning the
encoder. Root cause: A/B on a GPIO ISR. A bouncing or slow-RC edge
chatters at kHz+; every edge is an interrupt-level entry, IDLE never
runs. `ui` is innocent (asleep in `vTaskDelay` or at the top of its loop
feeding the TWDT). Screen-correlated because handling the board is what
moves the encoder, not the screen itself.

A 1 ms software rate cap inside a *raw* (`gpio_isr_register`) ISR was
not enough: dropped edges still pay the ISR entry cost. Panic trial
2026-09-03 (caps fitted, raw ISR + cap + IO-MUX filter all in) still
starved IDLE while `ui` sat on `jal enc_take_steps`. Serial `enc` used
to report `supp=` (rate-capped edges); it now reports `skip=` (gray-code
invalid samples).

Firmware answer: **poll A/B on the same 5 ms timer as SW.** Mechanical
rotation is tens of edges/s; the gray-code table already maps bounce and
skipped states to 0. No GPIO interrupt means no interrupt-level load.

Hardware still helps: v0.09 10–100 nF A/B/SW to ground against the GPIO
pull-ups (τ ≈ 0.1–1 ms). Don't go much above 100 nF or legitimate fast
rotation starts to round off.

## Build alias auto-default (2026-09-02)

`BRACINO_BUILD_NAME` now defaults to the **git short hash of the tree
being built** (e.g. `g82415e8`; `-m` suffix marks a dirty tree), so a
flashed binary is uniquely identified on the System Data screen without
any build-time ceremony. Fallback `dev` outside git. Named per-deploy
aliases still work: `idf.py -D'BRACINO_BUILD_NAME="b2"' ...`. Pair it
with the UTC stamp on the next System Data row.

## Field reflash without ESP-IDF — standalone esptool (2026-09-02)

`idf.py flash` needs the whole toolchain; a laptop only needs **esptool**
(`pip install esptool`, works on Windows/mac/Linux) + three .bin files
copied out of `firmware/node-bbu/build/` at build time:

- `node-bbu.bin` → offset **0x10000** (the app — flash this alone for a
  normal update; NVS at 0x9000 is untouched, so boot mode/params/identity
  survive)
- `partition-table/partition-table.bin` → 0x8000
- `bootloader/bootloader.bin` → 0x0 (only if bootloader changed)

Full image (matches this project's NO_STUB config — see the erase gotcha
above):

```bash
python -m esptool --chip esp32c3 -p COM5 --no-stub -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 node-bbu.bin
```

App-only update: `write_flash 0x10000 node-bbu.bin`. Serial monitor from
the same laptop: `python -m serial.tools.miniterm COM5 115200`. Copy
fresh .bin files after every firmware change — a stale app bin is the
classic field footgun.
