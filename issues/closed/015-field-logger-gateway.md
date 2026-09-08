# 015 — field logger gateway (next node on the bench)

- **Status:** closed 2026-09-08 — shipped, field-verified (see Fix/Verify below)
- **Type:** design / task
- **Opened:** 2026-09-03
- **Refs:** DESIGN_NOTE_003 (wire law), DESIGN_NOTE_004 (gateway + MQTT contract, addenda), 012 (closed — node on the wall), docs/gotchas.md (RF channel notes)

---

## SESSION LOG 2026-09-05/06 (late-night bench + t520 bring-up)

Stage-B backend **VERIFIED end-to-end** (human on t520, ~01:00):

- mosquitto auth'd, commit-service health `ok:true` (~30 s period),
  gateway `status` LWT + acker `time` topic flowing
- `/var/lib/bracino/commit/telemetry.jsonl` writing (55 lines pre-fake);
  repo `data/` is the local-bench path — t520 live path is the one above
- `fake_publisher` on t520 runs via `docker compose run --rm commit
  python fake_publisher.py --count 5 --host mosquitto` (image already
  contains it; `--host mosquitto` required; README updated `9cc823e`)
- Runbook auth trap fixed: shells don't auto-load `.env` (`390b8d1`)
- GW firmware: `s` now prints UTC wall time + LED-table health as text,
  single evaluator with the LED (`4102806`)
- Tree cleaned: generated gateway `sdkconfig` gitignored (`7268458`)

**Open: sim node cannot find the GW.** Bisect state: backend exonerated;
next step is `s` on GW serial (mode / health / `rx=` counters). Decision
tree: ACTIVE + HELLO=0 → suspect STA modem power-save eating ESP-NOW
frames (new vs bench master, which had no STA); fix ready:
`esp_wifi_set_ps(WIFI_PS_NONE)`. ACTIVE + HELLO climbing → ack direction.
WAIT_BACKEND → chase that leg.

**Confirmed bug — FIX APPLIED IN-TREE 2026-09-07 (rides GW flash):**
`restore_time()` now restores from `epoch_s` alone (u32 uptime-delta
underflow eliminated); `epoch_up` checkpoint dropped; DN004 amended.
See 019 (the 05 Sep field anomaly it explains) and 021 (detection chain).

Still pending: wall-node flash of `a32aca2` + delta 1.5 (016), untracked
docs/CONTEXT + HW_REFS + control_loop_notes.txt gitignore-or-commit
decision, stale 001–008 notebook sweep.

---

## DESIGN SETTLED 2026-09-04 (discussion with human; supersedes the
## open questions below — kept for the record)

The logger is a **streaming logger-gateway**: it does NOT own a durable
store. The node's FIFO ring (capture-while-disabled, days of retention,
decimation) IS the outage buffer — the DN004 stance ("write through or
withdraw, never accumulate") applies from birth. This dissolves the
durable-store question instead of solving it.

### Architecture

- **Born as `firmware/gateway/`** (human-agreed). Base =
  `firmware/bench-espnow-master` wire code (HELLO/CONFIG/DRAIN,
  stop-and-wait, retransmit — all field-proven against the real node),
  evolved, not forked.
- **Data path:** ESP-NOW batch → decode + anchor-translate → publish
  DN004 JSON to `bracino/node/<t>/<id>/telemetry` (QoS 0) → acker script
  appends JSONL + fsync → publishes `bracino/gateway/commit` watermark.
- **BATCH_ACK = ack-on-commit-watermark** (human decision): a drained
  frame is acked only after the acker's watermark covers its `end_ms`.
  DN003 semantics intact end-to-end; the node trims only on truth.
  No node-side changes, ever.
- **VM side (DN005 skeleton):** mosquitto (persistence on) + Python
  acker/commit service — subscribes telemetry+events, writes JSONL,
  fsync, publishes watermark + `bracino/gateway/health`. The JSONL file
  is the interim artifact AND the future Influx input schema. Dedupe key
  (node_type, node_id, boot_session, capture_ms) — retransmitted batches
  after a lost ack rewrite nothing.
- **Time:** SNTP primary (STA up ⇒ NTP), MQTT time-set fallback
  (`bracino/gateway/time` published periodically by the acker script),
  serial `n <unix_s>` bench fallback, NVS `{epoch_at_boot, uptime_s}`
  checkpoint so a gateway reboot doesn't skew `gw_ts` between sources.
  Hardcoded-epoch idea dropped.
- **Channel: STA-derived, never hardcoded** (human correction — see
  discussion): ESP-NOW follows the STA's associated channel; nodes
  rescan and re-anchor on any AP channel change (DN003 already owns
  this: channel comes from HELLO_ACK, rescan-on-unreachable is proven).
  AP replaced ⇒ SSID/password change ⇒ softAP provisioning workflow.
- **Status LED GPIO2** (active-high assumed, polarity #define):
  evaluated 1 Hz, root-cause-first priority scan, first hit wins:
  1 blink = no WiFi · 2 = WiFi but no broker · 3 = no valid time ·
  4 = no acks (broker up, watermark stale) · 5 = no node seen ·
  solid ON = all clear, with a brief flicker overlay on every received
  frame ("solid + flickering = alive and flowing; solid + silent = node
  side"). No-EPOCH is a *symptom* of no-WiFi post-boot, hence the
  human's swap: root causes report before symptoms.
- **Maintenance SoftAP, pass 1** (human pull): button long-press (~3 s)
  on **GPIO27, pull-up enabled, pressed = LOW**; never auto-started;
  10-min window; overlay, not a state (coexists with ACTIVE on the STA
  channel by construction). SSID `bracino-gateway01`, WPA2
  `bracinoAdmin` (in firmware source for now — LAN maintenance only).
  Pages: provisioning form (SSID/pass → NVS → STA restart) + status page
  (the LED table as text: WiFi/RSSI, broker, time source+validity,
  watermark age, node last-seen, counters). No ESP-NOW channel control
  on the web page (serial only would be the escape hatch — dropped as
  unnecessary once the channel became STA-derived).
- **Scope cuts for pass 1:** role table/lifecycle registry (single-node,
  MAC learned on first HELLO), command pipeline, CONFIG cache
  forwarding, per-sample liveness publication to MQTT (liveness → LED +
  logs first).

### Bench plan (race: human breadboards node, agent builds gateway+VM)

- **Bench node = spare ESP32 + ADS1115 + NTCs, NO TFT, NO relay** —
  stock node-bbu firmware degrades gracefully (tft.c guards every draw
  on `s_io == NULL`; relay is a bare GPIO write; encoder polled). Flash,
  `sim` mode, `comms on` — it is the wall unit's wire behavior in
  synthetic form. No throwaway firmware.
- **Drills:** (1) happy drain: FIFO → JSONL, watermarks monotonic,
  node trims. (2) Kill acker mid-drain: watermarks stall → gateway
  stops acking → node retransmits on timer → restart acker → resume,
  zero loss, zero dup lines. (3) Kill mosquitto: gateway withdraws
  (DN004), node buffers. (4) Kill WiFi: node accumulates; decimation
  under sustained outage.
- **Then field:** mount gateway on the boiler-room exterior wall (sees
  the roof Nano on ch 1, hears nodes through the thin wall), re-anchor,
  **drain install-day history** → first real charge-stop cycle data →
  016 adjudication, 017 tuning data.
- **Pass 2 (after real data flows):** OTA (rollback, from VM), SoftAP
  hardening, command pipeline, commit service grows into real DN005
  (Influx write replaces/appends JSONL).

---

## ORIGINAL SCOPING (2026-09-03) — superseded above, kept for the record

## Context

The BBU node is **installed and controlling the pump** (012 closed
2026-09-03), running the deploy image `060c88d` with `comms_enabled`
off — capture-while-disabled means it has been recording install-day
history into its FIFO ring since it went up. Next step per ROADMAP: a
**field logger gateway** that receives DN003 telemetry and makes it
durable, so install-day history can be drained and pump cycles become
observable. The bench master (`firmware/bench-espnow-master/`) stays a
throwaway harness; this issue scopes the first *real* gateway-class
firmware.

## Expected

- [x] Requirements settled into a short design note or addendum here:
      what "durable" means on the logger (flash-backed queue vs SD vs
      straight-through to a serial/MQTT link), retention, and how
      install-day history is drained (012's capture-while-disabled
      made the node-side side trivial — this is the receive side)
      → settled 2026-09-04/05 in "DESIGN SETTLED" below + DN004/005
- [x] **Ack only after durable write** (agreed): a BATCH_ACK the node
      trusts must mean the sample survives a logger power cut
      → ack-on-commit-watermark, gated on fsync'd JSONL; drills 1–3
      in 013 (zero loss, zero dup after acker/broker/WiFi kills)
- [x] Maintenance **SoftAP + GPIO2 status LED** per DN004 addenda
      (field-serviceable without a laptop)
      → shipped; human confirms LED patterns behave as designed
- [x] Drains install-day history from the node FIFO on first anchor
      (stop-and-wait batch protocol already proven: 3928 samples /
      ~42 s with watermarks monotonic)
      → confirmed in the field record (install-day history present in
      JSONL, clock freeze and all — see 019)
- [x] RF: channel **off the house AP's channel** — ch 6 was unusable at
      bench range (occlusion); 1/3/11 bind reliably. Re-survey at
      install; the node's channel comes from the gateway's HELLO_ACK
      → human verdict 2026-09-08: ch 6 unusable (crowding), 1 and 11
      bind well; channel is STA-derived so no hardcoding concern
- [x] Pump **charge-stop cycle observation** on the plant — the 012
      deferral lands here: once the logger drains history, a full
      cycle (start / stay-running / charge-stop) is verifiable from
      telemetry
      → ~a dozen full charge/stop cycles in the 2.5-day record
      (Sep-6 → Sep-8); 016 adjudication can proceed
- [x] Where firmware lives: this logger is the first occupant of
      `firmware/gateway/` or a stepping-stone sibling — decide before
      writing code (ROADMAP: don't grow `firmware/gateway/` proper
      until logger lessons land)
      → `firmware/gateway/`, decided and shipped

## Open questions

- Durable-store choice and capacity (node ring is ~4k samples ≈ 11 h at
  15 s; logger should hold days, not hours)
- Forwarding path: straight MQTT (broker not deployed yet) vs
  serial/USB drain to a laptop first
- DN004 addenda conformance: which counters the logger must expose to
  make chain health visible end-to-end (commit service is DN005's job,
  later)

## Proposal

Design pass first (fill the Expected boxes above in this issue), then
bench bring-up against the installed node at desk range before any
second device goes near the breaker box.

## Fix

Field logger gateway shipped end-to-end: `firmware/gateway/` (ESP-NOW
master, NVS-backed hold queue, fsync-gated BATCH_ACK, maintenance
SoftAP + GPIO2 LED, restore_time from NVS epoch) + t520 stage-B stack
(mosquitto auth'd, commit-service writing /var/lib/bracino/commit JSONL
with 30-day rotation, watermark + health topics, daily NAS mirror).
Deployed at the plant 2026-09-05; continuous multi-day capture since.

## Verify

- [x] Install-day history drained on first anchor (present in JSONL).
- [x] Multi-day continuous capture: 2.5-day record Sep-6 → Sep-8 shows
      solar charge cycles, nighttime ACS stairstep, AMB door-open
      sensitivity, ~a dozen pump charge/stop cycles.
- [x] LED root-cause pattern observed matching DN004 addenda.
- [x] RF: 1/11 reliable at plant; ch 6 ruled out (human verdict).
- [x] Ack chain survives GW power cut (node buffers; DN003 semantics
      held across the 019 freeze window and daily GW cycles).
- [x] t520 chain health green (issue 021 carries the residual drills:
      LWT, node-gone end-to-end).

Closed 2026-09-08. Residual supervision-chain drills live in 021;
Influx projection (stage C) tracked via server/README + 008.

## Record hygiene note (2026-09-08)

The captured JSONL is NOT a single continuous plant record — bench
traffic is interleaved with it:

- Bench bring-up sessions land under random `boot_session` tags **92**
  and **170** (node_id also 1): `mode: MANUAL`, relay off, room-
temperature probes (TPO/TPU ~26–28 °C), fresh boots (capture_ms ≈ 0
at stream start). Fault-injection events from those drills are in the
event stream too — notably `FAULT_RAISED fault_id=2` at
gw_ts 2026-09-06T16:23:30Z plus rapid F0 raise/clear chatter. The F2
was never cleared in-record; it is a bench drill artifact, NOT a plant
fault.
- Any plot that connects samples across `boot_session` changes paints
  vertical lies where the streams interleave (seen 2026-09-08 on the
  Sep-6 18:00 region).
- AMB readings dip at boot-session boundaries — CORRECTED 2026-09-08:
  those were room-entry (door) events, not a sensor artifact; every
  "post-boot" reboot was human-attended (022 closed). What remains at
  boot: a ~0.3 °C, <1 min TPO/TPU warm-up transient — negligible.