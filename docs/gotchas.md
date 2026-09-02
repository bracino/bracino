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
