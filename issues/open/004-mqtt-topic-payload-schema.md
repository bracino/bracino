# 004 — MQTT topic and payload schema

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** `firmware/gateway/`, `server/`, `docs/`

## Context

Topic tree and payloads will touch every firmware image and Node-RED/Influx path. Renames later are expensive. Retained messages + LWT are desired.

## Expected

Versioned sketch in `docs/` (topics, retained vs live, JSON or CBOR decision, units, node id scheme) good enough for gateway + first Influx write.

## Proposal

Always design assuming control autonomy: no topic must be subscribed for the pump to run. Commands are non-critical and validated on-node.

## Fix

## Verify
