# AGENTS.md — Bracino

Guidance for humans and coding agents working in this repository.

## What this is

**Bracino** replaces a failed Paradigma MES-BBU controller for a central heating plant (7-unit condo, Tuscany). Phase 1: ESP32-C3 control node runs the BBU (thermal storage) pump loop autonomously; an ESP32 WROOM gateway bridges ESP-NOW ↔ WiFi/MQTT to a self-hosted supervision stack (Mosquitto, Node-RED, InfluxDB, Grafana).

Canonical product/context dump: `project_slug.md`. Prefer that file over tribal knowledge when product intent is unclear.

## Non-negotiable: control autonomy

**The control node is authoritative over the pump on its own.** It reads its own sensors and runs its own control loop whether or not the gateway, broker, or backend are up. MQTT and the server stack are supervisory only (telemetry out, non-critical commands in). Nothing above the control node may sit in the critical path of correct pump operation. OTA and remote features must not violate this.

If a change couples pump safety or on/off decisions to network reachability, reject or redesign it.

## Repo layout (monorepo)

```
bracino/
  firmware/
    node-c3-bbu/     # phase 1 — BBU pump control (ESP32-C3)
    node-c3-acs/     # phase 2 — ACS loop (later)
    gateway-wroom/   # ESP-NOW ↔ WiFi/MQTT gateway
    monitor-btu/     # phase 3 — energy accounting (later)
  hardware/
    bbu-controller/  # KiCad when off protoboard
  server/
    docker-compose.yml
    mosquitto/
    nodered/
    grafana/
    influx-init/
  docs/
  project_slug.md
  AGENTS.md
```

Rationale: one history/tag set across firmware + server when MQTT topics or payloads change; split later under the `bracino` org only if a component earns independence (no submodules).

Empty phase directories may hold a short `README.md` stub only until work starts. Do not invent premature abstractions “for phase 2.”

## Stack and environment

| Layer | Choice |
|--------|--------|
| Control / gateway firmware | C, ESP-IDF |
| Broker | Mosquitto (retained state, LWT per node) |
| Logic / light UI | Node-RED (non-safety-critical only) |
| History / graphs | InfluxDB + Grafana |
| Deploy model | `git clone && docker compose up` on LAN; no cloud dependency for control |

- Dev: headless Ubuntu Server VM; tree under `~/projects` is shared with the host.
- Shared heavy toolchains (ESP-IDF, etc.): `~/projects/shared` — do not vendor full IDF into this repo.
- Secrets: environment variables / untracked env files. Never commit credentials into Node-RED flows, compose files, or firmware sources.
- InfluxDB **data** is not config; backup separately (NAS, then cloud). Config that rebuilds the stack lives in git.

## Coding norms

### Firmware (ESP-IDF / C)

- Prefer clear C over cleverness; match existing project style when present.
- Keep the autonomous control loop and local I/O paths free of hard dependencies on ESP-NOW, WiFi, MQTT, or OTA success.
- Fail safe on sensor faults (define and document behavior; default toward “don’t destroy the plant”).
- Watch stack sizes, logging-in-locks, and other ESP32 footguns; see repo skills if present (e.g. esp-idf-build).
- Protocol and topic renames are cross-cutting — update firmware **and** `server/` in the same change when possible.

### Server / infra

- Mosquitto, Node-RED `flows.json` (no embedded secrets), Grafana provisioning YAML/JSON → git.
- Prefer provisioning-as-code over click-ops so disaster recovery stays `compose up`.
- Node-RED holds schedules, thresholds, coordination — not pump safety interlocks that must work offline.

### Docs

- Durable decisions and schemas belong under `docs/` (MQTT topics, ESP-NOW payloads, BOM notes).
- Update `project_slug.md` only when product/phase intent actually changes; don’t use it as a scratch pad.

## Phases (don’t skip the runway)

1. **Prototype** — protoboard control + gateway; correct, reliable BBU pump logic.
2. **Harden** — PCB; ACS node.
3. **Energy accounting** — monitor-only BTU/fuel path.
4. **Further variants** — only when usefulness is proven.

Phase 1 priority order when unclear: **correct autonomous pump control → solid ESP-NOW link → MQTT telemetry → server dashboards → convenience features.**

## Open design debts (resolve explicitly; don’t silently invent)

Until written down in `docs/`, treat these as undecided:

- Hardware shape / BOM / schematic (ADC, current sense, solenoid drive)
- Hardware test/verification strategy
- MQTT topic + payload schema (stabilize early)
- ESP-NOW payload between control node and gateway
- Node-RED flow structure and first dashboard
- InfluxDB backup/retention policy

When implementing in one of these areas, propose a minimal concrete schema or decision in `docs/` rather than scattering magic strings.

## Git and PR habits

- Remote: `https://github.com/bracino/bracino` (monorepo under org `bracino`).
- Prefer atomic commits when firmware and server contracts move together.
- Do not commit build artifacts, `sdkconfig.old`, managed_components blobs unless the project deliberately vendors them, node_modules, Python venvs, or `.env` files.
- Tags may eventually span “whole stack” releases; no need to force versioning before first working prototype.

## What agents should not do

- Put network or broker availability in the pump critical path.
- Add Home Assistant (explicitly set aside) without a human decision to revisit.
- Introduce cloud control planes or require WAN for heating to work.
- Expand monorepo into nested git repos/submodules without an explicit split decision.
- Flash hardware or claim bench results without the user running the action (agents may prepare build/flash commands).

## Quick pointers

| Need | Where |
|------|--------|
| Product intent | `project_slug.md` |
| Agent rules | this file |
| MQTT / ESP-NOW schemas | `docs/` (once written) |
| Compose stack | `server/` |
| BBU firmware | `firmware/node-c3-bbu/` |
| Gateway firmware | `firmware/gateway-wroom/` |
| Shared ESP-IDF installs | `~/projects/shared` (outside repo) |
