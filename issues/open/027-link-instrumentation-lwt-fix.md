# 027 — Link-outage instrumentation (node LINK events + gw status JSON) and 021 LWT fix

- **Status:** open — instrumentation + LWT fix implemented, bins built; field flash pending
- **Type:** firmware (node + gateway) / backend (commit-service)
- **Opened:** 2026-09-11
- **Refs:** 026 (the outage pattern), 021 (detection chain), DN003/DN004

## Why

026 established that the Sep-10/11 "node doesn't talk for ~40 min" episodes
are node→gw ESP-NOW delivery outages with full ring backfill — charts stay
honest, but nobody knows *which half of the link* died. Theory-1 (gw health
gate tears down ESP-NOW on a single failed 1 Hz check; node scan backoff
amplifies a blip into a 10-minute nap) fits the record. Fixing before
measuring risks fixing the wrong leg. This issue is **measurement only** —
the fix decision (exit-hysteresis in net.c vs "hold your horses" LINK_STANDBY
ACK vs both) is explicitly deferred until an outage produces its verdict.

## Changes implemented

**Shared schema** (`firmware/shared/bracino_schema/include/espnow_schema.h`):
- `EVENT_LINK_SCAN 0x06` — value: consec_fail u8, last_ch u8, tx_fail u32le,
  retrans u32le. Offered at the unreachable→scan transition.
- `EVENT_LINK_BIND 0x07` — value: channel u8, scan_fails u8 (failed full
  scan cycles before bind = nap depth). Offered on successful bind.
- Both ride the normal event queue → during an outage they sit queued and
  arrive in the backfill dump (post-mortem, zero site presence).

**node-bbu comms.c:** the two offer points; counters captured before reset.

**gateway main.c:** decodes both events to JSON
(`{"event":"LINK_SCAN","consec_fail":N,"last_ch":N,"tx_fail":N,"retrans":N}`,
`{"event":"LINK_BIND","channel":N,"scan_fails":N}`) with capture_ms/boot
stamp; unknown tags still fall through to UNKNOWN_xx (old-gw compatibility).

**gateway net.c (fw `gw-016`):**
- Publishes retained `bracino/gateway/status` JSON on WAIT_BACKEND entry
  and ACTIVE entry **and every 30 s**, carrying
  `mode`, `legs {wifi,broker,time,backend}`, `rssi_dbm`, `channel`,
  `uptime_s`. RSSI history is the one quantity unreconstructable after an
  outage (marginal STA RSSI is the leading suspect for the gw wifi-leg
  stalls seen in mosquitto logs).
- **021 LWT fix:** LWT `msg_len` was 18 for a 16-char literal
  (`{"online":false}`) — a 2-byte over-read AND, fatally, a payload the
  commit-service matcher treated as "informational". Now 16.

**commit-service:**
- `bracino/gateway/status`: empty payload **or** JSON `online:false` →
  `gateway_gone` alarm (previously only empty matched — the actual LWT was
  silently dropped on Sep-10 11:52Z); `online:true` heartbeats are
  informational, logged on mode/legs change only.
- Recovery → `gateway_back` (mirrors node_gone/node_back).

## Flash manifest

See `ephemera/field_flash_recipe_2026-09-12.md` (bins, md5s, app-only
flash commands, drills). This flash folds the pending 023 field job into
the same visit (new bin carries 023's monotonic boot_id).

## Verify

- [ ] Field: both devices app-only flashed, NVS preserved, TFT boot number
      continues per 023; commit log shows `gw status:` heartbeats.
- [ ] On-site LWT drill: gw power-off → `gateway DOWN` alarm within ~40 s;
      power-on → `gateway_back`.
- [ ] On-site node-gone drill per 021 verify list.
- [ ] Next natural outage: JSONL shows LINK_SCAN/LINK_BIND events + gw
      WAIT_BACKEND status with failing legs → adjudicate 026 theory-1.

## Addendum 2026-09-12 (post-flash, t520 session)

Field flash done (node e4837ef-m / gw gw-016, see session pad). The gw
status heartbeats were log-on-change-only — no rssi_dbm time series
existed for 026 adjudication. Fixed: commit-service now persists one
`kind:"gw_status"` JSONL record per 30 s heartbeat (plus influx
measurement `gw_status`, tag `mode`). Contract noted in DN004 presence
section. Deployed with the 854247c t520 rebuild same day.

## Addendum — 2026-09-12 drills: LINK events' first field capture

Site drills (see 021 verify + session pad) exercised the 027
instrumentation for real. Node→gw leg, gw-off window 16:56:00–16:59:31Z:

- `LINK_SCAN` fired at 16:56:03Z (consec_fail=3, tx_fail=3, retrans=0)
  — fail-fast after ~45 s of NAKs, as designed.
- `LINK_BIND` at 16:59:44Z with scan_fails=5 — rebinding ~13 s after
  the gw returned, nap depth ~3.5 min total. Node streamed normally
  within one 15 s slot after bind; ring backfilled cleanly.
- Node serial during USB window (17:07–17:12Z): tx_ok=139 fail=0
  acks=16 retrans=1 — link essentially lossless at close range.
- Gw serial at gw boot: wifi assoc rssi -83, then HELLO/LINK_BIND/
  CONFIG_DESC/batch-acks all normal. One `time anomaly: mqtt disagrees
  by 55506 ms` at gw boot (watch item, adjacent to 019 time domains).
- 026 readout from drills: short gw outages produce fail-fast → scan →
  quick rebind (~2–3.5 min total), NOT the 10-min-cap nap. The 40-min
  wedges remain unexplained by these drills; gw-side rx quality
  (raw/drop/last_rssi) in the status JSON is the next instrumentation
  step (gw-017 candidate), plus the next natural outage.

## Deferred (deliberately, until data picks)

- net.c exit-hysteresis (N consecutive unhealthy checks to leave ACTIVE).
- DN004 "standby" semantics: keep ESP-NOW up in WAIT_BACKEND with a
  HELLO_ACK flag so the node knows the gw is alive-but-not-safe
  (human proposal 2026-09-11; good for real backend outages too).
- Alarm flap-suppression in 025 for the 11:27/15:29-style clusters.
