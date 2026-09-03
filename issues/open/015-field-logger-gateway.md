# 015 — field logger gateway (next node on the bench)

- **Status:** open
- **Type:** design / task
- **Opened:** 2026-09-03
- **Refs:** DESIGN_NOTE_003 (wire law), DESIGN_NOTE_004 (gateway + MQTT contract, addenda), 012 (closed — node on the wall), docs/gotchas.md (RF channel notes)

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

- [ ] Requirements settled into a short design note or addendum here:
      what "durable" means on the logger (flash-backed queue vs SD vs
      straight-through to a serial/MQTT link), retention, and how
      install-day history is drained (012's capture-while-disabled
      made the node-side side trivial — this is the receive side)
- [ ] **Ack only after durable write** (agreed): a BATCH_ACK the node
      trusts must mean the sample survives a logger power cut
- [ ] Maintenance **SoftAP + GPIO2 status LED** per DN004 addenda
      (field-serviceable without a laptop)
- [ ] Drains install-day history from the node FIFO on first anchor
      (stop-and-wait batch protocol already proven: 3928 samples /
      ~42 s with watermarks monotonic)
- [ ] RF: channel **off the house AP's channel** — ch 6 was unusable at
      bench range (occlusion); 1/3/11 bind reliably. Re-survey at
      install; the node's channel comes from the gateway's HELLO_ACK
- [ ] Pump **charge-stop cycle observation** on the plant — the 012
      deferral lands here: once the logger drains history, a full
      cycle (start / stay-running / charge-stop) is verifiable from
      telemetry
- [ ] Where firmware lives: this logger is the first occupant of
      `firmware/gateway/` or a stepping-stone sibling — decide before
      writing code (ROADMAP: don't grow `firmware/gateway/` proper
      until logger lessons land)

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

## Verify
