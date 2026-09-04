# gateway

Phase 1 **field logger-gateway** (issue 015, design settled 2026-09-04):
ESP-NOW (DN003 wire law) ↔ WiFi/MQTT uplink (DN004 shape, reduced scope).

Born from `firmware/bench-espnow-master` — the wire chassis (HELLO/
HELLO_ACK+anchor, stop-and-wait drain, retransmit tolerance) is the same
code field-proven against the real node in issue 013. Evolved, not forked.

## Pass-1 scope (what it does)

- **DN004 state machine** (`net.c` owns it): WAIT_BACKEND → ACTIVE.
  Health gate = WiFi associated ∧ broker connected ∧ time valid ∧
  backend writing (commit-service health topic < 90 s old). N=3 healthy
  checks to enter ACTIVE, one failure to leave. ESP-NOW exists only in
  ACTIVE — from the node's side, gateway loss is just loss.
- **Commit-gated BATCH_ACK**: a drained batch is acked only after the
  commit service's watermark (`bracino/gateway/commit`) covers the
  batch's `end_ms` — never on "broker received it". Retransmitted
  already-committed batches re-ack immediately; anything else waits
  (`s` shows held/pending state). Node reboots void stale commit state
  via `boot_session`.
- **Computed per-sample timestamps** (DN003 sanctions them): linear
  interpolation across the batch's declared span; decimated interior
  points are computed, the last sample lands exactly on `end_ms` so
  watermark coverage is exact. Published as DN004 flat JSON (QoS 0) with
  `capture_ms` + `batch_end_ms`/`batch_count` additions for dedupe.
- **Time**: SNTP primary → MQTT time-set fallback (`bracino/gateway/time`
  from the commit service) → serial `n <unix_s>` → NVS checkpoint
  (`{epoch, uptime}` every 5 min, restored at boot). All sources feed
  the same `settimeofday`; anchors derive from wall time.
- **LED GPIO2** (root-cause-first, 1 Hz): 1 blink no WiFi · 2 no broker ·
  3 no time · 4 no acks · 5 no node seen · solid = all clear with a dark
  blip per received frame.
- **Maintenance SoftAP** (`softap.c`): GPIO27 long-press (pull-up,
  pressed = LOW) ≥3 s; SSID `bracino-gateway01` / `bracinoAdmin`;
  10-min window; overlay (coexists with ACTIVE on the STA channel).
  Pages: `/` status (the LED table as text), `/prov` provisioning
  (SSID/pass/broker → NVS → reboot).
- **Channel is STA-derived** — no constants anywhere. AP changes channel
  → STA roams → ESP-NOW follows → nodes rescan and re-anchor (DN003).

## Pass-1 cuts

Role table/lifecycle, command pipeline, retained per-node status topics,
hourly-liveness MQTT publication, retained `…/config` topic (desc table
is logged only), OTA. These are pass-2 after real data flows.

## Layout

```text
main/main.c     DN003 wire chassis, registry, LED, serial console
main/net.c      WiFi STA + SNTP + MQTT + state machine + NVS + time
main/softap.c   GPIO27 button + maintenance AP + pages
../shared       bracino_schema (wire law — never fork) + bracino_log
```

Serial console (115200): `n <unix_s>` epoch, `b <host> [port]` broker
(NVS, applies immediately), `t` push TIME_SYNC, `s` status, `h` help.

Build: `. ~/projects/share/lib/esp/esp-idf/export.sh && idf.py build`.
Flash/monitor is a human step (WROOM: `/dev/ttyUSB*`, persists through
reset). Bench plan + drills: `issues/open/015`.
