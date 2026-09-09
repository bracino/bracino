# 023 — boot_session: random u8 causes collisions; make it an NVS counter

- **Status:** open
- **Type:** bug (schema / firmware)
- **Opened:** 2026-09-09
- **Refs:** `firmware/node-bbu/main/comms.c` (s_boot_session),
  `firmware/shared/bracino_schema/include/espnow_schema.h` (u8 field),
  `firmware/node-bbu/main/ui.c` (TFT boot display), closed/015 (record
  hygiene), closed/016 (adjudication provenance), DN003

## Context

`boot_session` is generated at comms init as `esp_random() & 0xFF`
(comms.c:1152): random per boot, 1..255. Consequences observed in the
Sep-2026 record:

- The field node re-used tag **92** after its Sep-6 reflash while bench
  traffic had already used 92 — pure chance, but it made provenance
  forensics needlessly hard (015 hygiene note had to be amended; the
  016 adjudication initially mis-attributed streams).
- u8 wraps at 255 regardless; boot tags are not meaningful to a human.
- The TFT diagnostic page shows `Boot %4u` — currently a random number,
  useless as a boot *count*.

## Proposal (human, 2026-09-09)

Make boot_session a **monotonic per-node counter**:

- Stored in NVS (params store); **first boot = 1**; every boot
  increments and persists.
- TFT display then shows a true boot count (reboot forensics at a
  glance; pairs with 021 node-gone detection).
- Dedupe and Influx keying already treat boot_session as an opaque
  number (commit_state JSON, Influx tag) — no server change needed.

### Schema width decision (recommended: u8 → u32)

An NVS counter must not silently wrap at 255. Widen
`espnow_schema.h boot_session` to **u32** (+3 bytes on the ESP-NOW
payload) and widen the GW's parse/detect logic (GW uses boot_session
to detect node reboots — see comms.h comment). JSONL/Influx carry it
unchanged as a number. Alternative rejected: u8 NVS counter — wraps
after 255 boots (~1 year at observed reboot frequency), same collision
class we are fixing.

### Known caveat: NVS erase

A full-flash migration with `erase_flash` resets the counter to 1,
which can collide with historical records of low boot ids (and the
commit-service dedupe key is (node, boot_session, capture_ms) — a
reset counter with fresh low capture_ms could be wrongly deduped
against ancient records). Mitigations, in order of preference:

1. Field rule in the flash runbooks: **preserve NVS sectors on app
   flashes** (already the practice, NVS@0x9000 untouched); after any
   deliberate erase, set the boot counter explicitly (make it an RW
   param-style value with a serial setter, e.g. `boot_id N`).
2. Document in DN003: boot_session is a monotonic counter, resets only
   on NVS erase; data forensics should sanity-check boot-id continuity
   against node_ts when in doubt.

## Fix

(filed 2026-09-09; implementation to ride the next routine field-node
reflash batch — no emergency reflash warranted.)

## Verify

- [ ] Bench: flash, reboot ×3 — boot_session increments 1,2,3; survives
      soft reset and power cycle; TFT shows the count.
- [ ] GW: node reboot detection still fires on boot_session change.
- [ ] Server: commit-service dedupe unaffected (spot-check a replayed
      batch produces zero new lines).
- [ ] DN003 amended (u32, monotonic semantics, erase caveat).
