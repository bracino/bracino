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
