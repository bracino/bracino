# 005 — ESP-NOW payload between node-bbu and gateway

- **Status:** open
- **Type:** design
- **Opened:** 2026-07-26
- **Refs:** related `004`, `firmware/node-bbu/`, `firmware/gateway/`

## Context

Boiler room has no usable WiFi; control node speaks ESP-NOW to the gateway. Need a compact, versionable payload for telemetry and optional non-critical command echo.

## Expected

Struct/layout doc in `docs/`: endianness, version field, fields for temps, pump state, current, faults, uptime; gateway mapping onto MQTT (004).

## Proposal

Prefer fixed binary with explicit version over ad-hoc strings; keep critical loop off this path.

## Fix

## Verify
