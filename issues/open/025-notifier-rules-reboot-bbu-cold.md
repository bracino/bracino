# 025 — notifier rules: node-reboot push + BBU-cold (TPO threshold)

- **Status:** open — design direction agreed (commit-service); awaiting
  go for implementation
- **Type:** enhancement (supervision / notification)
- **Opened:** 2026-09-09
- **Refs:** 021 (node-gone chain + ntfy push — the existing pattern),
  023 (monotonic boot_session — makes reboot detection reliable),
  017 (proper boiler-dropout detection later), server/README

## Want (human, 2026-09-09)

Phone notification when:
1. a node reboots;
2. the BBU gets cold — TPO < 53 °C.

## Where it belongs: commit-service (recommendation, agreed direction)

NOT Influx. Reasons:

- The commit-service is already the supervision point: 021's alarm
  path (`bracino/alarm` retained + `notify_push` via NTFY_URL) lives
  there, and it sees every telemetry record in real time off MQTT.
- Influx stays a pure projection by design (server/README stage C:
  "always rebuildable", non-authoritative, never gates anything).
  Alerting from Influx would make notifications depend on the
  projection and drag Influx Tasks/notification-endpoint infra into
  the stack for rules that are trivial over the MQTT stream.
- Edge-trigger/re-arm state belongs next to the existing alarm state.

## Rules

1. **Node reboot:** detect boot_session change per (node_type, node_id)
   → push "node rebooted, bootid=N" (informational, not alarm-grade).
   Reliable only AFTER 023 (monotonic counter) — with the current
   random u8 tags, a change is noisy (collisions) and an increment is
   not distinguishable from a wrap/coin flip. Implement together with
   or after 023.
2. **BBU cold:** edge-triggered threshold on node telemetry TPO:
   notify when TPO ≤ threshold (default 53), re-arm when TPO ≥
   threshold + re-arm gap (e.g. +2 °C) so a hovering tank does not
   spam. Threshold via env (server/.env, e.g. `TPO_COLD_C=53`,
   unset = rule disabled — matches the opt-in style of NTFY_URL).
   Note: this is the crude fixed-threshold cousin of 017 (boiler
   dropout from trends) — fine as a stopgap; 017 supersedes it later.

## Fix

(implemented after human go; commit-service only + .env keys + README.)

## Verify

- [ ] Bench: fake_publisher boot_session change → push received; no
      push on same-session traffic.
- [ ] Bench: TPO driven below threshold → one push; hover around
      threshold → no spam; re-arm above threshold+gap → re-armed.
- [ ] Ack/commit path unaffected (rules fail open, never block).
- [ ] server/README documents the env keys.
