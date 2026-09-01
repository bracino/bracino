# Roadmap

Living plan for **phase 1** (BBU controller + WiFi gateway). Later phases stay as ideas until phase 1 earns them. Reflect reality in [`STATUS.md`](STATUS.md); track execution in [`issues/`](../issues/README.md).

## Phase 1 — Prototype (active)

Goal: protoboard control node + gateway; **correct, reliable BBU pump logic** replacing the failed MES-BBU; supervisory telemetry when the LAN path is up.

Preferred order when something has to wait:

1. Hardware shape / sensing / drive path clear enough to write firmware against  
2. Autonomous control loop on `node-bbu` (fail-safe sensor behavior)  
3. ESP-NOW link control ↔ gateway  
4. MQTT telemetry + LWT via gateway  
5. Minimal server compose (Mosquitto → Influx/Grafana path; Node-RED as needed)  
6. Convenience (remote non-critical commands, nicer dashboards, OTA)  

### Design debts to close early

- [x] Control-node monitoring/control hardware definition (what + how) — ADR 001; Q1 2N3904 on relay IN (v0.08)
- [x] BOM / schematic notes (module proto) — KiCad v0.08 on disk; protoboard soldered
- [ ] Hardware test / verification strategy (plant checklist still open; protoboard I/O matches breadboard)
- [x] **ESP-NOW payload** between `node-bbu` and gateway — [DESIGN_NOTE_003](DESIGN_NOTE_003_espnow_node_schema.md) (wire law, 2026-08-30)
- [x] **MQTT topic + payload schema** — [DESIGN_NOTE_004](DESIGN_NOTE_004_gateway_design.md) (2026-08-30; issue 004 closes against it)
- [x] Offline BBU control law written — [DESIGN_NOTE_002](DESIGN_NOTE_002_bbu_control_loop.md)
- [x] v0 firmware: implement that loop on `node-bbu` (boots MANUAL; `auto` / `sim`)
- [x] Desk `sim` walk-through of start / stay-running / stop / FAULT (human, 2026-08-22)
- [x] Dummy AC load on the relay (human, 2026-08-25); CT still binary, reliable for that
- [x] Ice / boil two-point on potted NTCs (human, 2026-08-25); β=3950 holds; 95 °C cap was too tight
- [ ] Gateway still empty (contracts settled: DN003 wire law, DN004 gateway design; bench harness in 013)
- [x] TFT + encoder protoboard (010 closed 2026-08-28): menus navigate; rotate/click/hold OK; BGR palette
- [ ] Commit service: MQTT → Influx with per-node commit watermarks (DESIGN_NOTE_005 stub in DN004; replaces Node-RED in the telemetry path)
- [ ] Influx backup/retention (NAS + cloud) — policy before years of data matter

### Implementation milestones (checklist)

- [x] Repo scaffold, docs, issues notebook
- [x] Documented MQTT + ESP-NOW contracts under `docs/` (DN003 + DN004, 2026-08-30)
- [x] `node-bbu` builds under ESP-IDF; bring-up I/O (relay + A0–A3), not the loop
- [x] Flash inverted `relay_set()`; coil toggles and holds. GPIO10 backfeed not DVM’d this pass
- [x] GPIO8 heartbeat; external 5 V (USB unplugged); coil commanded off stays off
- [x] KiCad v0.08 exports (GPIO8 LED, 12 V LED, 5 V-only J7 jumper); protoboard soldered and I/O-checked
- [x] NTCs + 10 kΩ dividers wired on A1–A3 (breadboard, then protoboard)
- [x] Firmware samples A1–A3 (`r` / `s1`–`s3`)
- [x] NTC open/short on A3 (open ≈ 13 mV, short ≈ 3283 mV, 28 °C ≈ 1760 mV)
- [x] NTC °C conversion; treat near-0 / near-rail as fault (β=3950)
- [x] Pump on/off logic on desk `sim` (setpoint / hysteresis / TPU hold)
- [x] Same loop on dummy AC load (human, 2026-08-25); no real BBU pump yet
- [x] Ice / boil two-point on the potted NTCs (human, 2026-08-25)
- [ ] Field install (012 — node runs the pump; ESP-NOW client compiled in per 011, comms-enabled flag per DN003)
- [x] TFT + encoder UI on protoboard (010 closed 2026-08-28)
- [x] 2-ESP bench wire test: node client vs bench gateway harness (011 + 013 closed 2026-09-01) — discovery, live telemetry, overnight drain, decimation, 20% frame loss, unreachable→rescan in tens of ms
- [ ] Gateway builds; ESP-NOW bring-up with control node
- [ ] MQTT publish path + retained state / LWT
- [ ] Commit service (DN005): MQTT → Influx with commit watermarks; chain-health + notifications
- [ ] `server/docker-compose.yml` + Mosquitto config (persistence on) in git
- [ ] Influx write path + one Grafana dashboard (events stream + annotations per DN006)
- [ ] Admin panel (descriptor-driven; DN003 PARAM_SET path)
- [ ] Data-integrity failure-mode matrix (DN006)
- [ ] Node-RED optional views (demoted from telemetry path by DN004 commit service)
- [ ] Forcible “server down” test: pump loop still correct

## Phase 2+ (not scheduled — do not pre-build empty trees)

Recreate directories when work actually starts.

- **Harden** — PCB (`hardware/bbu-controller`); ACS pump loop node (new `firmware/…` when named)
- **Energy accounting** — monitor-only board for fuel-in vs BTU-delivered.
  **This is also the accepted fix for the BBU node's run-confirmation blindness** (decided 2026-09-01, issue 014 closed): BBU v0.09+ ships without CT or snubber — the node's contact only closes a 230 VAC contactor coil, so it cannot see pump current (MES-BBU parity, DN001 rev 2). Sketched as a **boiler monitor** node: split-core CTs on pump, fuel auger and blower legs, flue + water-jacket inlet/outlet temps — pump degradation, fuel-usage spikes, anomalous temps, and true pump run-confirmation all come from that node.
- **Further variants** — TBD from real usefulness

## Non-goals (for now)

- Cloud control plane or WAN-required heating
- Home Assistant as the primary stack
- Nested git repos / submodules
- Phase 2/3 firmware directories “for symmetry”
- Putting Node-RED or MQTT in the pump safety path

## Naming

| Use | Not |
|-----|-----|
| `firmware/node-bbu` | `node-c3-bbu` (MCU may change) |
| `firmware/gateway` | `gateway-wroom` (module-neutral, like `node-bbu`) |

## Convention stability

When MQTT topics, ESP-NOW layouts, or compose service names change, prefer one commit (or tightly stacked commits) across **firmware + `server/` + `docs/`**. Update STATUS when user-visible capability moves; keep closed issues for root-cause lore.
