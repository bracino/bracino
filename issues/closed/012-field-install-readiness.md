# 012 — field-install readiness (node-bbu in-plant)

- **Status:** closed 2026-09-03 — node deployed to the plant
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

**HW note 2026-09-02 (human):** PPTCs still out of stock — deploy the
protoboard **without F1**. Accepted risk: node mounted on concrete, no
flammables nearby, only 220 V connection is the contactor coil line (second
relay), 12 V supply mounted elsewhere, breaker box covers severe
over-current. Schematic bump will reflect the absent PPTC.

## Expected

- [x] 009 closed: Auto-start on real wells + pump ON when TPO cold (human). Charge-stop not waited — watch a full cycle when convenient, not a blocker
- [x] CT dispensed with (2026-09-01, 014): firmware `CT_FITTED 0` — A0 ignored, telemetry `ct_state = NOT_FITTED`, no-CT warning gated off (`ct_fitted`); TFT shows `n/f` instead of the amber no-CT WARN; A0 reserved for a later rev. DN001 rev 2 / DN002 updated
- [x] Boot-mode persistence (DN002 boot behavior) implemented — `bbu/boot` NVS blob (mode + Manual coil state), written by a control-module persist callback that only fires on human/commanded paths (serial / UI / PARAM_SET); Auto loop transitions never write; TESTING never persisted; params auto-save on change. Host tests + IDF build pass; **bench-verified 2026-09-02** (incl. erase_region factory-fresh test → Manual/OFF)
- [x] Identity provisioned in NVS (node_type=1, node_id=1) at flash time — confirmed on unit (serial `ident`; `comms` shows node(1,1))
- [x] ESP-NOW client compiled in (011 closed); `comms_enabled` NVS default **off**. Enable from UI/serial only when the field logger is present
- [x] Hardware loose ends: PPTC F1 deliberately omitted (deploy decision, risk rationale above); connector/jumper labels done; sensor runs to tanks done; enclosure done; **schematic v0.09 committed** (encoder caps, PPTC omission reflected)
- [x] TFT field-image menus (2026-09-02, build clean, **bench-walked: usable**):
      - Main: comms status line (disabled/enabled + OK or SCANNING when
        enabled)
      - Temperatures: add AMB; drop Hyst and dT (programmables, not
        sensor readings)
      - Counters: telemetry FIFO depth (0/1 normal; >1 = buffering — no
        comms or gateway down)
      - Control Programming: every DN002 parameter field-editable with
        validation + NVS persistence (mode, manual pump, setpoint,
        hysteresis, min on/off times, min_tpo_tpu_delta_c,
        max_run_time_min, comms enable, sample_period_s 5..120) — parity with the future admin
        panel; build it as the DN003 param_id table so both share one
        validated setter path
        Diagnostics: Include following comms data: channel, gw, anchored, last EPOCH, fifo, fails, rx, tx_ok, fail, retrans, decim ev_sent
      - System Data screen to include firmware revision/build number
      - Note 2026-09-03: `ct_confirm_s` REMOVED from the registry per human
        decision (CT defunct, 014) — id 5 reserved, CONFIG_VER 3, params
        blob v2 (one-time defaults reset). No CT row in Control Prog.
- [x] Serial `halt` + UI confirmed before leaving the pump unattended — bench-confirmed on live NTCs (2026-09-02): reboot in Auto → relay opens, re-closes after min_off

## Deploy readiness (2026-09-02, end of session) — **DEPLOYED 2026-09-03**

**Deployed.** Bench validation: `b749dc3` passed a 5-min desk check (all
menus, param edits) 2026-09-03 — **deploy image cut**: `CONFIG_ESP_TASK_WDT_PANIC`
removed from sdkconfig.defaults + sdkconfig (re-add on demand for a real
bench backtrace; recipe kept as a comment in sdkconfig.defaults). TWDT
itself stays armed at 5 s, log-only. Field image `060c88d` pushed to
origin before install. Field report (human, 2026-09-03): **on the wall,
confirmed controlling the pump**, parameters persisted through the
deploy reflash (params blob v2 migration proven in the field — tweaks
applied on the desk survived), NTC values sane. Comms OFF (no logger
yet). Full charge-stop cycle observation deferred to the logger (015) —
summer, cycles infrequent.

Panic trial (encoder-provoked, caps on): IDLE starved, `ui` innocent at
`jal enc_take_steps`. A/B GPIO ISR + 1 ms software cap was not enough;
**A/B now polled on the 5 ms timer (no GPIO ISR).** Second dump on that
image (menu-surfing): `ui` had just returned from `enc_take_hold`.
Cause: `pdMS_TO_TICKS(5)==0` at HZ=100 — ui never blocked. **Fix:
`vTaskDelay(1)`.** Re-soak on `5697cef` (2026-09-03): ~10 min menu
abuse + soft reboots, **zero TWDT** — panic trial done, culprit named.

Field-build fixes (2026-09-03, this issue): pump counters saved only on
pump stop never checkpointed the bench's continuous run — "re-zero upon
flash". Now saved every 5 min of runtime + on stop/clear/soft-reboot;
UI DIAG soft-reboot flushes (was serial-only). Clear counts is two-click
confirmed. `ct_confirm_s` was mislabeled as a second "dT min"; later the
same day it was removed from the registry entirely (id 5 reserved).
Home shows UTC clock from the TIME_SYNC anchor (`04 Sep 2026 17:57`).

Bench validation: `b749dc3` passed a 5-min desk check (all menus,
param edits) 2026-09-03 — **deploy image cut**: `CONFIG_ESP_TASK_WDT_PANIC`
removed from sdkconfig.defaults + sdkconfig (re-add on demand for a real
bench backtrace; recipe kept as a comment in sdkconfig.defaults). TWDT
itself stays armed at 5 s, log-only. Deploy checklist: comms stays OFF
(no logger yet), confirm `FW <hash>` on System Data matches the deploy
commit, params were reset to defaults once at the b749dc3 boot (re-apply
tuned values before mounting).

## 2026-09-02 bench session (agent fixes, human walk)

Walked and working: Counters (uptime/pump/starts/FIFO), Diagnostics incl.
soft-reboot two-click confirm, Control Prog (abbrev names, `>val<` edit
preview), serial `reboot`, build alias + UTC stamp. Fixed same day: DIAG
stack overflow (items[16]→[17] — crashed on entry), TWDT mis-subscription
(main task was subscribed via `esp_task_wdt_add(NULL)` in app_main → 5 s
dump spam wrecked scan dwells; mon+ui now subscribe themselves), scan now
two HELLO shots/dwell (300 ms) + 9 s budget, `link_ok` requires bound
(was anchored-only → showed OK while scanning). Persistent counters in
NVS (bbu/stats; save on pump stop/clear/soft-reboot; power blip mid-run
loses current run). GW default channel 6→1 — house AP on ch 6 occludes
HELLO/ACK (bench: ch 1/3/11 bind reliably, 6 unusable; DN004 addenda +
gotchas). Bench-master reflash pending; deploy-channel survey at install.

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
charge-stop cycle on the plant when convenient. **(Cycle watch executed
2026-09-03: deferred to the logger — 015 — since cycles are infrequent
in summer; pump control itself confirmed at install.)**

## Fix

Field image evolved `d45815d` → `060c88d` over 2026-09-02/03 (details
above):
- Encoder A/B GPIO ISR removed — polled on the 5 ms timer (`f0c0274`)
- `vTaskDelay(1)` in ui loop — HZ=100 made `pdMS_TO_TICKS(5)` 0 (`5697cef`)
- Counters: 5-min runtime checkpoint + UI-reboot flush; Clear counts
  two-click confirm; `ct_confirm_s` removed from the wire registry (id 5
  reserved, params blob v2, CONFIG_VER 3); Home UTC clock (`91a5a19`,
  `b749dc3`)
- Deploy image: TWDT panic config stripped, log-only TWDT (`75fc52c`,
  `060c88d`)

## Verify

- Bench: WDT re-soaks clean — ~10 min menu abuse + soft reboots on
  `5697cef`, 5-min desk check (all menus, param edits) on `b749dc3`;
  new node logs boring (zero TWDT lines)
- Bench: System Data hash matched the built commit at every stage;
  `ring`/`tel`/menus walked per the 012 field-image checklist
- **Field (human, 2026-09-03):** node on the wall, confirmed
  controlling the pump; parameters as previously set (v2 blob migration
  + desk tweaks survived the deploy reflash); NTC values sane
- Deferred to 015: full charge-stop cycle observation via logger
  telemetry (summer — infrequent cycles)
- Low-priority bench-hardening items above (serial `st` with TFT
  unplugged; bare-module graceful no-ADS mode): **won't fix for v1** —
  the field unit's TFT is permanently fitted, and printf to a dead USB
  host already degrades gracefully (spin ≤50 ms + drop); noted in
  gotchas
