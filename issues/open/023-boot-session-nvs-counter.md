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

## Proposal (human, 2026-09-09; refined same day — see below)

Make boot_session a **monotonic per-node counter**:

- Stored in NVS (params store); **first boot = 1**; every boot
  increments and persists.
- TFT display then shows a true boot count (reboot forensics at a
  glance; pairs with 021 node-gone detection).
- Dedupe and Influx keying already treat boot_session as an opaque
  number (commit_state JSON, Influx tag) — no server change needed.

### Width: u8 stays (human decision, 2026-09-09)

The node only reboots on reflash or power outage; 255 reboots is many
years, and a wrap is benign anyway — the commit-service dedupe cursor
only compares against the *latest* stored session, and Influx keys on
record timestamps, so a 2031 boot tag 6 cannot clobber or be deduped
against 2026's boot 6. The poison in the current scheme is *random
reuse within weeks* (esp_random), which the counter eliminates. No
schema-width change, no GW parse change, no ESP-NOW payload growth.

### Continuity across reflashes

- **App-only flash (the common re-flash):** NVS preserved (standard
  practice, NVS@0x9000 untouched) → counter continues automatically.
  No action needed.
- **erase_flash migration (rare, deliberate):** counter lost. Repair
  via a serial **RW param `boot_id N`** set during the existing
  post-flash recipe (which already sets params over serial, e.g.
  min_tpo_tpu_delta_c): set to previous boot id + 1, per the flash
  manifest. Chosen over the originally suggested build-time header
  line: a header bakes the value at build time (stale if the image is
  reflashed later) and drags in generated-header machinery; the serial
  value is always read from the TFT/manifest at flash time and rides
  the existing recipe.
- Semantics of the setter: writes the NVS counter for the NEXT boot
  (running boot keeps its id); document in the runbook + DN003.

### Known caveat: NVS erase

Without the reseed step, a post-erase node starts at boot 1, which can
collide with historical low-id records. Forensics rule of thumb (DN003
note): sanity-check boot-id continuity against node_ts when in doubt.

## Fix

(filed 2026-09-09; implementation to ride the next routine field-node
reflash batch — no emergency reflash warranted.)

## Verify

- [ ] Bench: fresh NVS → boot id 1; reboot ×3 — increments 2,3,4;
      survives soft reset and power cycle; TFT shows the count.
- [ ] Bench: erase_flash → boots at 1; serial `boot_id 6` → next boot
      is 6 (continuity restore path).
- [ ] GW: node reboot detection still fires on boot_session change.
- [ ] Server: commit-service dedupe unaffected (spot-check a replayed
      batch produces zero new lines).
- [ ] DN003 amended (monotonic counter semantics, setter, erase
      caveat); flash runbook gains the post-erase `boot_id N` step.
