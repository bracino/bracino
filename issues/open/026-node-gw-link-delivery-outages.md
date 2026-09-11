# 026 — Node→gateway ESP-NOW delivery outages; ring backfill masks them in charts

**Opened:** 2026-09-11 · **Status:** open, root cause unknown
**Data:** Sep-10/11 record (`ephemera/remote_data/telemetry.jsonl`, 2026-09-10 09:13Z → 09-11 16:37Z)

## Symptom (human report)

Gaps in JSONL continuity (e.g. at line 556) where the node "doesn't talk" for
~40 min at a time, then dumps a burst of data. Charts look fine — user
correctly suspected NOT brownout.

## What the record actually shows

The node **never stopped sampling and never rebooted** — the gaps are
*delivery* outages on the node→gw ESP-NOW path, backfilled from the comms
ring on re-link:

- `boot_session` 213 for the whole record; `capture_ms` strictly monotonic.
- Zero sample loss: every dump replays complete 15 s cadence with true
  `node_ts` (e.g. 10:47:38→11:25:53 = 151/151 records).
- No brownout signature: no −99.9s, no glitch temps, no post-stop spikes.
- Control law kept running mid-outage (charged stop 10:51:28Z, TPO 57.9,
  recorded in the dump with correct capture_ms). Charts are honest because
  Influx indexes by node_ts.

## Outage windows (UTC)

| Window | Duration | Notes |
|---|---|---|
| 09-10 10:47:38→11:25:53 | 38 m | gw ambient + MQTT live throughout |
| 09-10 11:27:25→12:36:01 | 69 m | includes gw MQTT keepalive death 11:52:08→12:33:38 ("exceeded timeout") — gw ambient LOST there (no ambient backfill) |
| 09-10 21:12→21:15 | 2.5 m | |
| 09-10 22:01→22:05 | 4 m | flapped |
| 09-10 23:25→09-11 00:31 | 66 m | flappy start 23:25–23:34 (3 short dumps), then long stall |
| 09-11 03:19, 03:40, 03:47 | 3–4 m each | short rebinds |
| 09-11 03:55→04:23 | 28 m | gw MQTT alive throughout |
| 09-11 15:24→15:32 | 8 m | also gw MQTT timeout 15:24:49 (7 s reconnect) |
| 09-11 15:46→16:24 | 38 m | gw MQTT alive throughout |

commit-service GONE/CLEARED alarms bracket every window exactly; the 021
detection chain works in the field.

## Corroborating server evidence (t520)

- No container restarts in window (mosquitto Up 5 d, commit Up 2 d).
- gw MQTT client (`ESP32_c69F28`) has a history of "exceeded timeout"
  drops (09-10 02:37/02:44/03:21/03:22/11:53, 09-11 15:24) — gw-side
  WiFi/MQTT stalls are a recurring separate phenomenon.
- Most outages happen with the gw MQTT-connected and ambient flowing ⇒
  failure is node→gw ESP-NOW, not broker/commit.

## Pattern

- Two populations: **short** (3–8 min, consistent with a normal
  unreachable→scan→rebind cycle) and **long** (28–69 min — far too long
  for a healthy gw to be findable by the node's HELLO scan at ~7 s/attempt).
- Short events sometimes flap just before long ones.
- A ~15:24–15:25 UTC gw ambient hiccup appears on BOTH days — watch for a
  periodic LAN/AP event at that time.

## Open questions / next steps

1. Root cause of long stalls: node in SCANNING and not hearing gw, or
   gw not hearing node (channel drift, AP channel change, adjacent-channel
   leakage, SoftAP interference — see esp-serial-bench skill gotchas).
2. Bench/field: run `comms st` on node + gw serial TLOG during/after an
   outage; look for `channel REVERTED`, NAK bursts, scan attempts, and
   whether tx_fail/retrans counters spike. Check AP channel-log stability.
3. Decide whether the commit-service alarm text should distinguish
   "node silent" (ESP-NOW) from "gw silent" (WiFi/MQTT) — the 11:52 event
   was a GW outage but only the node alarm exists today.
4. 025 notifier: the 11:27/15:29 flap clusters would push repeat alarms;
   consider a flap-suppression rule.

## Addendum — 2026-09-11: instrumented, awaiting the next outage (027)

Human field context confirmed the visit story (dead dashboard, site normal
on arrival = post-hoc backfill). Measurement instrumentation implemented
in [027](027-link-instrumentation-lwt-fix.md): node LINK_SCAN/LINK_BIND
events (ring-backed, arrive in the dump), gw WAIT_BACKEND/ACTIVE status
JSON with failing legs + RSSI/channel history (fw gw-016), and the 021
LWT fix (commit-service was dropping the actual LWT payload — folded into
027). Flash recipe: `ephemera/field_flash_recipe_2026-09-12.md`.

Closed questions from the addendum discussion:
- **AP channel:** fixed ch1 @ 20 MHz for years (human-confirmed) — channel
  drift theory retired; node's `SCAN_PRIOR_CH = 1,6,11` already tries ch1
  first.
- **Radio power-save:** gw runs `WIFI_PS_NONE` — eliminated as suspect.
- Which health leg flapped (wifi vs broker vs time vs backend) is exactly
  what the new gw status JSON will name. The fix decision
  (exit-hysteresis vs standby-ACK vs both) is deferred until then.
