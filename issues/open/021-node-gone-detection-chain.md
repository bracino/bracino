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

- [x] t520: commit-service rebuilt on 027 (2026-09-12, flash-day; again
      same day for the gw_status persistence follow-up).
- [x] Field node-off drill (2026-09-12 17:02–17:08Z): node power removed
      → `!! ALARM: node 1/1 GONE` + push; restore → `ALARM CLEARED` +
      push. Two cycles (boots 3 and 4). Bracketed correctly both times.
- [x] GW LWT drill — **field, first positive test** (2026-09-12
      16:56:46Z): gw power removed → `!! ALARM: gateway DOWN` + push
      within 46 s of last heartbeat; restore → `ALARM CLEARED: gateway
      back online` (16:59:30Z). The Sep-10 silent-failure path now works
      end-to-end (JSON LWT + fixed matcher + ntfy push).
- [x] Comms-off durability verified accidentally during the morning
      flash (comms stayed off across a node power cycle), then re-enabled
      cleanly on-site.
- [x] Push channel (ntfy) live: configured on t520, all drill alarms
      delivered as phone push. NOTE: topic name `bracino_alerts` is
      guessable — rotate to an unguessable one (follow-up).

## Addendum — 2026-09-11: LWT silently failed its first field test (027)

The chain was accidentally field-drilled by the Sep-10/11 record (026):
node_gone/node_back bracketed every outage — but when the gw's MQTT died
at 11:52Z (mosquitto "exceeded timeout"), **no gateway_gone alarm fired**.
Root cause: two-ends drift. This issue specified an *empty* LWT payload;
the firmware grew a JSON habit (LWT = `{"online":false}`, with a
2-byte `msg_len` over-read besides) and the commit-service matcher treated
any non-empty payload as informational. Fix in [027](027-link-instrumentation-lwt-fix.md):
commit-service now alarms on empty OR `online:false`, and publishes
`gateway_back` on recovery. Drill updated — see 027 Verify.
