# Status

**As of:** 2026-08-14  
**Phase:** 1 — prototype (BBU control node + WiFi gateway)  
**Repo:** https://github.com/bracino/bracino

## Summary

Hardware definition for the module prototype is written (ADR 001, KiCad v0.06). Breadboard bring-up of rails, ADS1115, and the CT path is in progress. `firmware/node-bbu` has a **bench sketch** (relay toggle + A0 rms), not a control loop. No ESP-NOW, MQTT schema, or compose stack yet.

Open design work: [`issues/open/`](../issues/open/). Plan: [`ROADMAP.md`](ROADMAP.md). Kickoff scrap (not maintained): [`project_slug.md`](project_slug.md).

## What works

| Area | State |
|------|--------|
| Repo layout + git remote | Solid (phase 1 only) |
| Root README / AGENTS / MIT license | Solid |
| STATUS / ROADMAP / issues notebook | Process solid; this file tracks bench reality |
| Control-node HW definition | **Mostly settled** — ADR 001 + KiCad v0.06 under `hardware/bbu-controller/` |
| `node-bbu` firmware | **Bring-up only** — builds (ESP-IDF 5.2 / esp32c3); serial `on`/`off`/`s`/`scan`. No control loop |
| `gateway` firmware | **Not started** |
| MQTT topic + payload schema | **Not decided** |
| ESP-NOW payload schema | **Not decided** |
| Relay drive (5 V module vs GPIO10) | **Open** — 5 V IN pull-up overpowers the C3; module temporarily on 3.3 V. NPN (or FET) planned |
| CT / current sense | **Binary only** on this prototype — [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md) |
| NTCs (A1–A3) | **Not on the breadboard yet** |
| `server/` docker compose + provisioning | **Not started** |
| Grafana dashboards / Node-RED flows | **Not started** |
| Influx backup/retention policy | **Not decided** |

## Bench (2026-08-14) — do not overclaim

Verified by hand on the breadboard (not automated):

- 12 V → buck 5 V → MCU 3.3 V rails.
- ADS1115 at 0x48 on GPIO7 SDA / GPIO6 SCL.
- ZMCT103C on 3.3 V, A0, pot **2 turns CCW** from full CW: relay off ≈ 0 mV rms; relay on / no load ≈ 37 mV; ~0.15 A fan ≈ 175 mV. Dryer currents 0.84–7.8 A are **not** monotonic — do not treat A0 as amperes.
- Relay module at 5 V: IN looks like a 5 V pull-up; GPIO10 cannot force off; toggling GPIO10 did nothing. Module then run at 3.3 V so bench loads could be switched. **Not** the long-term drive.

Shared ESP-IDF is under `~/projects/share/lib/esp/esp-idf` (AGENTS still says `~/projects/shared`).

## Known constraints (always true)

- Pump on/off must remain correct with gateway/broker/server **down**.
- No WAN/cloud dependency for heat.
- Secrets never in git (Node-RED flows, compose, firmware sources).
- Shared ESP-IDF lives outside this repo.

## Architecture (current tree)

```text
firmware/node-bbu/         bring-up firmware (relay + A0)
firmware/gateway/          placeholder README
hardware/bbu-controller/   KiCad prototype v0.06 + pin map
server/{mosquitto,nodered,grafana,influx-init}/
docs/STATUS.md ROADMAP.md ADR_001.txt DESIGN_NOTE_001…
docs/CONTEXT/ HW_REFS/     plant notes + datasheets
issues/{open,closed,fixtures}/
```

## Verification

`firmware/node-bbu` builds with:

```bash
. ~/projects/share/lib/esp/esp-idf/export.sh
cd firmware/node-bbu && idf.py build
```

Flash/monitor is a human step (`/dev/ttyACM0` on the C3-Zero; port may drop on reset). No compose stack yet.

## License

MIT — see root `LICENSE`.
