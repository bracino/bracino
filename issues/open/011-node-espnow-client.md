# 011 — node-bbu ESP-NOW client (DESIGN_NOTE_003)

- **Status:** open
- **Type:** firmware
- **Opened:** 2026-08-30
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

## Verify
