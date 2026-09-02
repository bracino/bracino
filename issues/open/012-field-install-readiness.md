# 012 — field-install readiness (node-bbu in-plant)

- **Status:** open
- **Type:** process / hardware
- **Opened:** 2026-08-30
- **Refs:** issue 009, issue 011, DESIGN_NOTE_002, docs/STATUS.md

## Context

In-plant deadline: **2026-08-31 EOD**. Node must control the pump
autonomously from first power-on, boot into its last known mode, and
require no reflash when the gateway/backend arrive later.

**Update 2026-08-31:** deadline missed — first plant hookup inconclusive
(hidden second pump relay; relay/snubber rework needed; CT run-confirmation
bust). See [014](../closed/014-hidden-second-relay-snubber.md).

**Update 2026-09-01:** 014 **closed** — contactor identified (ABB ECB24-40,
230 VAC coil line), snubber lifted ⇒ pump switches correctly from the node,
CT dropped from the circuit (`CT_FITTED 0`, telemetry `NOT_FITTED`). Install
unblocks here, pending the field-image pass below. 009 closed 2026-09-01:
Auto-start on real wells observed; charge-stop not waited (desk-proven).

## Expected

- [x] 009 closed: Auto-start on real wells + pump ON when TPO cold (human). Charge-stop not waited — watch a full cycle when convenient, not a blocker
- [x] CT dispensed with (2026-09-01, 014): firmware `CT_FITTED 0` — A0 ignored, telemetry `ct_state = NOT_FITTED`, no-CT warning gated off (`ct_fitted`); TFT shows `n/f` instead of the amber no-CT WARN; A0 reserved for a later rev. DN001 rev 2 / DN002 updated
- [x] Boot-mode persistence (DN002 boot behavior) implemented — `bbu/boot` NVS blob (mode + Manual coil state), written by a control-module persist callback that only fires on human/commanded paths (serial / UI / PARAM_SET); Auto loop transitions never write; TESTING never persisted; params auto-save on change. Host tests + IDF build pass (2026-09-02, not yet bench-flashed)
- [ ] Identity provisioned in NVS (node_type=1, node_id=1) at flash time
- [x] ESP-NOW client compiled in (011 closed); `comms_enabled` NVS default **off**. Enable from UI/serial only when the field logger is present
- [ ] Hardware loose ends: PPTC (F1) if stock arrived, connector/jumper
      labels, sensor runs to tank, enclosure serviceable (USB reachable —
      no OTA transport yet, so a node bug means physical reflash);
      **schematic bump (v0.09+) reflects snubber + CT removal** (014)
- [x] TFT field-image menus (2026-09-02, build clean, **not yet bench-walked**):
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
        Diagnostics: Include following comms data: channel, gw, anchored, last EPOCH, fifo, fails, rx, tx_ok, fail, retrans, decim ev_sent
      - System Data screen to include firmware revision/build number
      - Note: with `CT_FITTED 0` the no-CT warning never fires; `ct_confirm_s`
        stays listed (registry stability) but is inert
- [ ] Serial `halt` + UI confirmed before leaving the pump unattended

## Low-priority bench hardening (2026-08-31, from bare-module flash test)

Flashing this image on a bare C3 super-mini (no ADS/TFT) shows the loop
lives but (a) I2C reads fail every tick and the loop settles in FAULT —
expected there — and (b) a task watchdog fired with idle starvation, dump
caught `mon` in the USB-CDC printf spin. Not a permanent hang: IDF's
USB-CDC TX spins ≤50 ms per call then drops (vfs_usb_serial_jtag.c
TX_FLUSH_TIMEOUT_US). Likely the 010 leftover "serial `st` with TFT
unplugged" case. Items:

- [ ] Close the 010 leftover: verify serial `st` + watchdog behavior with
      TFT unplugged on the protoboard (idle starvation → fix or document)
- [ ] Optional graceful missing-peripheral mode for bare-module bench
      tests: one ADS1115 probe at boot; absent ⇒ skip I2C sampling,
      temps report FAULT with a single startup notice instead of
      per-tick errors
- Field-relevant bounds confirmed: printf to a dead USB host degrades
  (spin ≤50 ms + drop), so the headless field node won't hang on console
  writes — keep verbose boot prints out of the field image anyway

## Proposal

Sequence: 009 is closed (Auto-start on real wells). Flash the install image with
identity + boot persistence, decide comms flag, verify boot → Manual/OFF →
Auto → power-cycle → Auto on the bench before mounting. Watch a full
charge-stop cycle on the plant when convenient.

## Fix

## Verify
