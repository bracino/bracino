# 021 — node-gone detection chain (GW node-status MQTT + commit-service notifier)

- **Status:** open — implemented in-tree (GW net.c/main.c + commit_service.py); awaiting t520 rebuild + field comms-on
- **Type:** firmware / backend
- **Opened:** 2026-09-07
- **Refs:** 019 (the freeze this detects), DN004 (topics)

---

## Gap

A frozen node (019) is invisible: `flagged_unreach` existed on the GW
but surfaced only in serial logs and LED blinks. Commit-service had no
alerting at all. A recurrence would have produced 11 more silent hours.

## Detection contract (human-agreed 2026-09-07)

- **Node comms ON is the field default** — the detection chain *is* the
  argument. comms-off is for deliberate experiments only. The
  `comms_enable` param is NVS-persisted (`nvs_save()` on toggle), so an
  off-stays-off human choice is durable across reflashes (NVS survives;
  never erase-flash). A comms-off node trips the GW's unreach flag too —
  which is correct behavior (you *want* to know a field node is dark).
- Detection latency = node heartbeat period + GW expect_ms window
  (minutes, not seconds). Good enough for the "no hot water by morning"
  failure class (boiler self-protects the safety side).

## Fix (implemented)

**Gateway:**
- On unreach raise: publishes retained
  `bracino/node/<t>/<id>/status` `{"online":false,"silent_s":N,"boot_session":B}`.
- On any frame from a flagged node: publishes `{"online":true,…}`
  (retained → state survives broker restarts; cleared as soon as the
  node says anything).

**commit-service:**
- Subscribes `bracino/node/+/+/status` and `bracino/gateway/status`.
- Node offline → retained `bracino/alarm` `node_gone` + loud log.
- Node back → `bracino/alarm` `node_back` (retained clear).
- GW LWT (empty payload on `bracino/gateway/status`) → `gateway_gone`
  alarm (catches a frozen/rebooted gateway).
- Optional phone push: set `NTFY_URL` env (e.g. `https://ntfy.sh/<topic>`)
  — opt-in, no secrets (an ntfy topic is its own credential).

## Verify

- [ ] t520: rebuild + restart commit-service (`docker compose build
      commit && up -d commit` — still pending from 06 Sep).
- [ ] Bench or field: power off the node with comms ON → within the
      liveness window, GW logs `node silent`, status topic flips
      retained offline, commit-service logs ALARM (and pushes if NTFY_URL
      set). Power back on → node_back clears.
- [ ] GW LWT drill: stop the GW container/process → gateway_gone alarm.
- [ ] Confirm comms-off durability: toggle comms off, power-cycle node,
      param stays off; `comms_enable` reads back correctly.
