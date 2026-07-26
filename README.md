# Bracino

Replacement **control and monitoring** for the central heating plant (*la caldaia*) serving a 7-unit condo in the Tuscan countryside.

The aging Paradigma MES-BBU (~25 years) has corrupted firmware and no longer switches the **350W BBU** (thermal-storage) pump from tank temperature differential. Bracino replaces that core loop and adds remote supervision — without putting the network in the critical path of heat.

**Repo:** [github.com/bracino/bracino](https://github.com/bracino/bracino) (monorepo)

**Status:** [docs/STATUS.md](docs/STATUS.md) · **Roadmap:** [docs/ROADMAP.md](docs/ROADMAP.md) · **Issues notebook:** [issues/](issues/README.md)

**Agents / contributors:** [AGENTS.md](AGENTS.md) · fuller product dump: [project_slug.md](project_slug.md)

## Architecture principle (non-negotiable)

**The control node is authoritative over the pump on its own.** It reads local sensors and runs the control loop whether or not the gateway, MQTT broker, or backend are reachable. MQTT and the server stack are **supervisory only** (telemetry out, non-critical commands in). That is what makes desk-side OTA and remote tinkering safe for a real plant.

## Phase 1 scope (now)

| Piece | Role |
|--------|------|
| **Control node** (`firmware/node-bbu`) | Autonomous BBU pump logic: tank temps (ADC), pump via solenoid, current sense. Bench HW today is ESP32-class (often C3); directory name stays MCU-neutral. |
| **Gateway** (`firmware/gateway-wroom`) | ESP-NOW ↔ WiFi/MQTT bridge (ESP32 WROOM-class ). Works without the control node joining a WiFi BSS. |
| **Server stack** (`server/`) | LAN-only: Mosquitto, Node-RED, InfluxDB, Grafana — `git clone && docker compose up` rebuild model. |
| **Hardware** (`hardware/bbu-controller/`) | KiCad / design notes once off protoboard. |

Later phases (ACS node, BTU monitor, …) are **out of tree until needed** — see roadmap, not empty forever-folders.

## Name

**Bracino** — Tuscan/Pistoiese dialect:

1. Diminutive of *brace* (embers) — small pieces of the same fire (distributed nodes).
2. Keeper/seller of charcoal — “tends the fire.”

## Repo layout

```text
bracino/
  firmware/
    node-bbu/            # BBU pump control
    gateway-wroom/       # ESP-NOW ↔ MQTT gateway
  hardware/
    bbu-controller/      # PCB when ready
  server/                # docker compose + service config
    mosquitto/ nodered/ grafana/ influx-init/
  docs/                  # STATUS, ROADMAP, schemas as they land
  issues/                # open/ closed/ fixtures/ lab notebook
  project_slug.md        # long-form intent
  AGENTS.md
```

Atomic commits across firmware + server when MQTT/ESP-NOW contracts change. No nested repos/submodules.

## Server model

Self-hosted, **local network only**, no cloud requirement for heating:

- **Mosquitto** — retained last-known state; LWT for online/offline
- **Node-RED** — non-safety-critical logic + light phone-friendly UI (thresholds, schedules)
- **InfluxDB + Grafana** — history and owner-shareable read-only graphs

Config in git; secrets via environment. Influx **data** is backed up separately (NAS → cloud), not treated as compose fodder.

Home Assistant was considered and **set aside** (redeploy-from-git story is weaker for this setup).

## Users

Primarily the builder/maintainer. Read-only graphs/status for other unit owners later.

## Development environment

- Headless Ubuntu Server VM; `~/projects` shared with the host
- Firmware: **C**, **ESP-IDF** (shared installs under `~/projects/shared`, not vendored here)
- Flash/bench on real hardware; agents prepare commands, humans run them on the iron

## Verification (as it grows)

```bash
# Firmware (once ESP-IDF projects exist), typical pattern:
# . ~/projects/shared/esp-idf/export.sh   # or your shared install
# cd firmware/node-bbu && idf.py build

# Server (once compose lands):
# cd server && docker compose up -d
```

Track concrete work in [`issues/`](issues/README.md); summarize capability in [`docs/STATUS.md`](docs/STATUS.md).

## License

TBD (not set yet).
