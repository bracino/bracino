# 009 — Offline BBU control loop

- **Status:** open
- **Type:** task
- **Opened:** 2026-08-22
- **Refs:** `firmware/node-bbu/`, `006`, `docs/DESIGN_NOTE_001_ct_binary_only.md`, `docs/CONTEXT/BBU_Module_Technical_Notes_edit_v3.md`

## Context

Protoboard I/O is good enough to write against. `node-bbu` is still a serial bench sketch. The plant needs a local loop that runs with gateway / broker / USB unplugged.

## Expected

On-node: NTC mV → °C with open/short as **fault** (not a temperature); CT as running / not; pump on/off from local sensors only. Serial stay as debug / manual override. No ESP-NOW, MQTT, or webserver in this issue.

## Proposal

Three slices (do not start 2 until 1 prints sane °C on the protoboard):

1. Conversion + faults on the existing `r` / `s` commands.
2. A control task that samples and drives the relay; serial `auto` / `on` / `off`.
3. Bench proof with dummy loads and forced open/short. Real BBU pump is out of scope here.

Must decide before slice 2: control law (MES-style TPO/TPU vs setpoint, vs a simple ΔT), which TH is which, and fail-safe on sensor fault (default proposal: pump **OFF**).

## Fix

## Verify
