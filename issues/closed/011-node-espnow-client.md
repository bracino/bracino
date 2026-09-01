# 011 — node-bbu ESP-NOW client (DESIGN_NOTE_003)

- **Status:** closed
- **Type:** firmware
- **Opened:** 2026-08-30
- **Closed:** 2026-09-01 (wire contract evidenced via 013)
- **Refs:** `firmware/node-bbu/`, DESIGN_NOTE_003, DESIGN_NOTE_004, issue 006

## Context

node-bbu must ship with the full DN003 client compiled in — idle until a
gateway exists — so an installed node never needs a field reflash when the
gateway/backend arrive. Control-loop isolation per DN003's non-blocking
radio contract.

## Expected

Envelope TX/RX, HELLO/HELLO_ACK + TIME_SYNC anchoring, channel-scan
discovery (non-blocking, bounded), unified FIFO + stop-and-wait
TELEMETRY_BATCH/BATCH_ACK, EVENT emission, PARAM_GET/SET/ACK +
CONFIG_GET/DESC, NVS-persisted identity (node_type=1, node_id) and channel
cache. Version / unknown-TLV skip paths implemented, not stubbed.

## Proposal

Bench-test against gateway firmware skeleton (issue 006) on a second ESP —
the whole DN003 wire contract is verifiable without MQTT. NVS
`comms_enabled` flag (default off) as a de-risk for the 2026-08-31
installation deadline; toggled from the Control Programming menu. Node
keeps buffering until first TIME_SYNC (DN003 epoch-less bring-up
invariant) — no telemetry leaves the node pre-sync.

## Fix

Implemented 2026-08-31 (bench-ready, build-clean for both targets):

- `firmware/shared/bracino_schema/` — `espnow_schema.h` canonical registry
  (envelope, msg types, TLV tags, event ids, BBU telemetry v1, BBU param
  ids), included by node and bench-master from one physical file.
- `main/comms.{h,c}` — the DN003 client: unified FIFO (default 4096
  samples, `ring <n>` bench knob; graceful degradation to 32 on OOM),
  stop-and-wait + BATCH_ACK watermark trim, 2 s retransmit, 3-fail ⇒
  rescan, channel scan cached→1/6/11→1..13 with DN003 dwell/budget/rest,
  TIME_SYNC anchoring + epoch-less TX invariant (no telemetry before
  non-zero epoch), HEARTBEAT 2 s, EVENT (FAULT_RAISED/CLEARED diffed in
  monitor task, PARAM_CHANGED on local setter), PARAM_GET/SET with
  monotonic-admin_seq replay guard (EXPIRED; TTL wall-clock deferred to
  DN005), CONFIG_DESC with fragmentation ({idx,total} + MORE_FRAGMENTS,
  2 s reassembly timeout). Identity + comms_enabled in NVS ("comms" ns).
- DN003 param_id table in `params.{c,h}` — one validated setter path;
  ids 8/9/10 (user_mode, manual_relay, comms_enable) route to control
  via hooks. Serial `prog` emits PARAM_CHANGED (source=LOCAL_UI).
- `main.c` — 1 Hz capture hook (cadence-gated, default 15 s, `tel <s>`
  knob), wire-mode mapping, fault-bit mapping (open/short by rail side,
  range_fault→OPEN), quiet CT burst in the monitor path.
- Bench counterpart: `firmware/bench-espnow-master/` per issue 013.

Deferred to 012 field-image pass: UI-menu parity for comms status and
param editing (serial covers both today).

## Verify

Desk pair vs throwaway bench master (issue 013, closed 2026-09-01):
HELLO→ACK+sync, CONFIG_DESC, stop-and-wait BATCH_ACK, overnight 11 h
accumulation + drain, 20% frame-loss retransmit, unreachable→rescan in
tens of ms on the cached channel. Wire defects found during bring-up
(11 B vs 12 B sample, scan bind-race, pinned peer channel, 10 s rest
before first scan) all fixed. `comms_enabled` NVS default off — radio
never in the pump path.

UI-menu parity remains 012.
