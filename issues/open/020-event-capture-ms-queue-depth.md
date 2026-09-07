# 020 — EVENT capture_ms + queue depth (node→GW schema amendment)

- **Status:** open — implemented in-tree (node comms.c + gateway main.c + DN003); awaiting field flash + drain-shape verification
- **Type:** schema / firmware
- **Opened:** 2026-09-07
- **Refs:** DN003 (amended), 019 (freeze forensics — the motivating record), 015

---

## Problem

EVENT messages carried no node-side timestamp. Drained events therefore
land in the JSONL at **send pacing** positions, not event times. 05 Sep
evidence: 7 TPO_OPEN events at machine-uniform 19-line intervals across
a 5320-line drain burst — their FIFO order is truthful, their positions
are artifacts, their true times are unknowable.

Second failure: `EVENT_QUEUE_LEN = 8` drops the *newest* offers on
overflow. The probe swap wiggled both probes at once — TPU raise/clear
was dropped (one `fault_flags=4` sample with no matching event) and one
TPO clear went missing (raise with no clear in the record).

## Fix (implemented)

- `TLV_EVENT_CAPTURE_MS` (0x12, u32 LE) appended to every EVENT wire
  payload: node clock_ms **at offer time** (espnow_schema.h, append-only
  — old nodes remain valid, tag simply absent).
- `comms_offer_event()` stamps `now_ms()` into the queue entry.
- `EVENT_QUEUE_LEN` 8 → 24 (RAM cost ~trivial).
- Gateway `handle_event()` parses the trailing TLV, converts to wall
  time with the node's anchor arithmetic (same math as telemetry
  samples), and appends `capture_ms` / `boot_session` / `node_ts` to
  the MQTT event JSON. Old-node events publish with `capture_ms: 0`
  semantics via the no-TLV path (plain event JSON, pacing-positioned).
- commit-service already spreads unknown JSON fields into the JSONL
  event lines, so the new fields flow through with no backend change.
- DN003 amended (wire format section); DN004 amended (drain-pacing note).

## Verify

- [ ] After node reflash + comms-on: raise/clear an NTC fault at the
      probe (bench-safe wiggle) and confirm the JSONL event line carries
      `node_ts` within one sample period of the telemetry neighborhood.
- [ ] Force a queue burst (several probe wiggles in rapid succession):
      confirm no drop (`ev_drop` counter stays flat) up to depth 24.
- [ ] Old-firmware node against new GW: events still flow (no capture
      TLV → plain JSON, no crash).
