# 012 — field-install readiness (node-bbu in-plant)

- **Status:** open
- **Type:** process / hardware
- **Opened:** 2026-08-30
- **Refs:** issue 009, issue 011, DESIGN_NOTE_002, docs/STATUS.md

## Context

In-plant deadline: **2026-08-31 EOD**. Node must control the pump
autonomously from first power-on, boot into its last known mode, and
require no reflash when the gateway/backend arrive later.

## Expected

- [ ] 009 plant checklist executed on the real pump (loop proven)
- [ ] Boot-mode persistence (DN002 boot behavior) implemented
- [ ] Identity provisioned in NVS (node_type=1, node_id=1) at flash time
- [ ] ESP-NOW client compiled in; comms-enabled-vs-flag decision made (011)
- [ ] Hardware loose ends: PPTC (F1) if stock arrived, connector/jumper
      labels, sensor runs to tank, enclosure serviceable (USB reachable —
      no OTA transport yet, so a node bug means physical reflash)
- [ ] TFT field-image menus:
      - Main: comms status line (disabled/enabled + OK or SCANNING when
        enabled)
      - Temperatures: add AMB; drop Hyst and dT (programmables, not
        sensor readings)
      - Counters: telemetry FIFO depth (0/1 normal; >1 = buffering — no
        comms or gateway down)
      - Control Programming: every DN002 parameter field-editable with
        validation + NVS persistence (mode, manual pump, setpoint,
        hysteresis, min on/off times, ct_confirm_s, min_tpo_tpu_delta_c,
        max_run_time_min, comms enable) — parity with the future admin
        panel; build it as the DN003 param_id table so both share one
        validated setter path
- [ ] Serial `halt` + UI confirmed before leaving the pump unattended

## Proposal

Sequence: close 009 on the plant, then flash the install image with
identity + boot persistence, decide comms flag, verify boot → Manual/OFF →
Auto → power-cycle → Auto on the bench before mounting.

## Fix

## Verify
