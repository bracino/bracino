# Status

**As of:** 2026-07-26  
**Phase:** 1 — prototype (BBU control node + WiFi gateway)  
**Repo:** https://github.com/bracino/bracino

## Summary

Bracino is at **scaffold**. Product intent and repo conventions are written down; **no firmware control loop, no ESP-NOW link, no MQTT schema, and no docker compose stack** are implemented yet. The monorepo is pared to phase 1 trees only (`node-bbu`, `gateway-wroom`, hardware stub, empty server service dirs).

Open design work is tracked under [`issues/open/`](../issues/open/). Overall plan: [`ROADMAP.md`](ROADMAP.md). Intent dump: [`project_slug.md`](../project_slug.md).

## What works

| Area | State |
|------|--------|
| Repo layout + git remote | Solid (phase 1 only) |
| Root README / AGENTS / slug | Solid |
| STATUS / ROADMAP / issues notebook | Solid (process) |
| `node-bbu` firmware (control loop, ADC, pump, current) | **Not started** |
| `gateway-wroom` firmware (ESP-NOW ↔ MQTT) | **Not started** |
| MQTT topic + payload schema | **Not decided** |
| ESP-NOW payload schema | **Not decided** |
| Control-node HW BOM / schematic | **Not decided** |
| `server/` docker compose + provisioning | **Not started** |
| Grafana dashboards / Node-RED flows | **Not started** |
| Influx backup/retention policy | **Not decided** |

## Known constraints (always true)

- Pump on/off must remain correct with gateway/broker/server **down**.
- No WAN/cloud dependency for heat.
- Secrets never in git (Node-RED flows, compose, firmware sources).
- Shared ESP-IDF lives under `~/projects/shared`, outside this repo.

## Architecture (current tree)

```text
firmware/node-bbu/         control node (placeholder README)
firmware/gateway-wroom/    gateway (placeholder README)
hardware/bbu-controller/   PCB later
server/{mosquitto,nodered,grafana,influx-init}/
docs/STATUS.md ROADMAP.md
issues/{open,closed,fixtures}/
```

## Verification

Nothing automated yet. When firmware lands, prefer `idf.py build` (and project-local smoke notes in STATUS). When server lands: `docker compose up` from `server/` on a clean clone with env files supplied out of band.

## License

TBD.
