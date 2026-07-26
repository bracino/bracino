# Bracino

Replacement control and monitoring system for the central heating plant ("la caldaia") serving a 7-unit condo in the Tuscan countryside.

## Background

The existing controller — a Paradigma MES-BBU, roughly 25 years old — has corrupted firmware and no longer reliably switches the 350W BBU (thermal storage) pump on/off based on tank temperature differential. Bracino replaces its core logic and extends it with remote monitoring, additional sensors, and room to grow into a small distributed control/monitoring platform for the whole plant.

## Name

**Bracino** — a real Tuscan/Pistoiese dialect word with two relevant senses:
1. Diminutive of *brace* (embers) — "fine/small embers." Fits the distributed-node architecture: small pieces of the same fire, each holding a bit of state.
2. *Bracino* / *braciàio* — one who tends a charcoal kiln, or sells charcoal/wood at retail; also historically used of someone soot-blackened or scruffy from the work. Fits the "keeper of the fire" character of the project.

GitHub organization: `bracino` (created). Repo layout is a single monorepo rather than nested/split repos — see Repo Structure below.

## Architecture principle (non-negotiable)

**The control node is authoritative over the pump on its own.** It reads its own sensors and runs its own control loop regardless of whether the gateway, MQTT broker, or backend server are reachable. MQTT and the backend stack are strictly supervisory: telemetry out, non-critical remote commands in. Nothing above the control node is allowed to be in the critical path for correct pump operation. This is the core design constraint that makes "OTA updates from my desk" safe to do without risking the plant's actual heating.

## Hardware

- **Control node**: ESP32-C3, in the boiler room (no WiFi coverage there). Reads two thermal-storage tank temperatures via ADC, drives the 350W pump via solenoid, monitors pump current draw via a current sensor. Runs the autonomous control loop.
- **Gateway node**: ESP32 WROOM dev module. Bridges ESP-NOW (control node link, works without an associated WiFi network) to WiFi/MQTT (uplink to the office server). Will support additional nodes as the project expands (up to ~5 anticipated).
- **Future nodes** (phase 2+): second control node for the ACS (domestic hot water) pump loop, same hardware design with a different control algorithm; a monitor-only board (phase 3) to track pump/motor activity for fuel-energy-in vs. BTU-delivered accounting.

## Software / server stack

Self-hosted, local-network-only, no cloud dependency, designed to survive WAN outages:

- **Mosquitto** — MQTT broker. Retained messages for last-known state on reconnect; LWT (last will) per node for online/offline status without polling.
- **Node-RED** — middleware/logic layer and lightweight phone-friendly control UI. Deliberately holds non-safety-critical logic (schedules, thresholds, coordination rules) so it can be changed from a browser without re-flashing firmware.
- **InfluxDB + Grafana** — time-series history storage and dashboards/graphs. Read-only Grafana dashboard links can be shared with the other condo unit owners without giving them control access.
- Considered and set aside for now: Home Assistant (lower setup effort, but `.storage/` internal state doesn't redeploy cleanly from a plain git repo the way this stack does).

### Disaster recovery model

- Mosquitto config, Node-RED `flows.json` (no embedded credentials — secrets via environment variables), and Grafana dashboards/datasources (as provisioning YAML/JSON) all live in git and reconstruct the running system via `git clone && docker compose up`.
- InfluxDB data is accumulated state, not config — backed up separately to local NAS, then to cloud storage. Goal: support year-on-year comparisons from late 2027 onward, so retention and backup cadence need to be solid from the start.

## Repo structure (monorepo, under the `bracino` GitHub org)

```
bracino/
  firmware/
    node-c3-bbu/        (phase 1)
    node-c3-acs/        (phase 2)
    gateway-wroom/
    monitor-btu/         (phase 3)
  hardware/
    bbu-controller/      (KiCad, once off protoboard)
  server/
    docker-compose.yml
    mosquitto/
    nodered/
    grafana/
    influx-init/
  docs/
```

Rationale: atomic commits across firmware + server config when protocol/topic changes happen; one version history and tag set spanning the whole stack; individual components can still be split into standalone repos under the same org later if one ever earns independence (no submodules).

## Phases

1. **Prototype** — control node + gateway node on protoboard; get BBU pump logic working correctly and reliably, replacing the failed MES-BBU.
2. **Harden** — move to proper PCB; add second node for ACS pump loop (monitor, possibly control); same hardware, different control algorithm.
3. **Energy accounting** — derived monitor-only board watching pump/motor activity, to compare fuel energy input against BTUs actually delivered to end users.
4. **Further variants** — TBD as the project's usefulness becomes clearer.

## Users

Primarily the builder/maintainer. Some graphs and status data will be presented (read-only) to the other condo unit owners.

## Environment

Development happens in a headless Ubuntu Server VM; hardware flashing/based on the ESP-IDF SDK. Preferred coding language is C. Toolchain specifics TBD as phase 1 firmware work begins. This and all folders under ~/projects are shared with the VM host. Several ESP32 projects might be happening in parallel. Shared large resources like ESP-IDF are therefore kept in ~/projects/shared. 

## Open questions / next steps

- [ ] Hardware shape, what exactly are we monitoring and controlling and how are we doing it. 
- [ ] Control-node hardware BOM/schematic (ADC choice, current sensor, solenoid driver)
- [ ] Hardware testing/verification strategy
- [ ] MQTT topic and payload schema (get this right early — renaming later touches every node's firmware)
- [ ] ESP-NOW payload structure between control node and gateway
- [ ] V.1 of firmware for controller and gateway
- [ ] Node-RED flow structure and initial dashboard layout
- [ ] Backup schedule/retention policy for InfluxDB (local NAS + cloud)
