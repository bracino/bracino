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

- [ ] Control-node monitoring/control hardware definition (what + how)
- [ ] BOM / schematic notes (ADC, current sense, solenoid drive)
- [ ] Hardware test / verification strategy
- [ ] **MQTT topic + payload schema** (stabilize before many nodes care)
- [ ] **ESP-NOW payload** between `node-bbu` and gateway
- [ ] v0 firmware skeletons: `firmware/node-bbu`, `firmware/gateway-wroom`
- [ ] Node-RED flow structure + first useful views
- [ ] InfluxDB backup/retention (NAS + cloud) — policy before years of data matter

### Implementation milestones (checklist)

- [ ] Repo scaffold, docs, issues notebook (this pass)
- [ ] Documented MQTT + ESP-NOW contracts under `docs/`
- [ ] `node-bbu` builds under ESP-IDF; local loop stub + I/O hooks
- [ ] Pump differential logic proven on bench (even before PCB)
- [ ] Gateway builds; ESP-NOW bring-up with control node
- [ ] MQTT publish path + retained state / LWT
- [ ] `server/docker-compose.yml` + Mosquitto config in git
- [ ] Influx write path + one Grafana dashboard
- [ ] Node-RED: read-only status / non-critical setpoints only
- [ ] Forcible “server down” test: pump loop still correct

## Phase 2+ (not scheduled — do not pre-build empty trees)

Recreate directories when work actually starts.

- **Harden** — PCB (`hardware/bbu-controller`); ACS pump loop node (new `firmware/…` when named)
- **Energy accounting** — monitor-only board for fuel-in vs BTU-delivered
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
| `firmware/gateway-wroom` | (rename only if the module story changes) |

## Convention stability

When MQTT topics, ESP-NOW layouts, or compose service names change, prefer one commit (or tightly stacked commits) across **firmware + `server/` + `docs/`**. Update STATUS when user-visible capability moves; keep closed issues for root-cause lore.
