# AGENTS.md — Bracino

Guidance for humans and coding agents in this repository.

## What this is

**Bracino** replaces a failed Paradigma MES-BBU controller for a central heating plant (7-unit condo, Tuscany).

**Phase 1 only (active tree):**

- `firmware/node-bbu` — autonomous BBU (thermal storage) pump control node  
- `firmware/gateway-wroom` — ESP-NOW ↔ WiFi/MQTT gateway  
- `server/` — Mosquitto, Node-RED, InfluxDB, Grafana (LAN, compose-from-git)  
- `hardware/bbu-controller` — PCB when off protoboard  

Long-form intent: [`project_slug.md`](project_slug.md). Human entry: [`README.md`](README.md). Capability snapshot: [`docs/STATUS.md`](docs/STATUS.md). Plan: [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Non-negotiable: control autonomy

**The control node is authoritative over the pump on its own.** Local sensors + local control loop must work if gateway, broker, or backend are down. MQTT/server are supervisory (telemetry out, non-critical commands in). Reject changes that put pump safety or on/off decisions on the network path.

## Repo layout

```text
bracino/
  firmware/node-bbu/
  firmware/gateway-wroom/
  hardware/bbu-controller/
  server/{docker-compose later, mosquitto, nodered, grafana, influx-init}/
  docs/                 # STATUS, ROADMAP, schemas as they land
  issues/{open,closed,fixtures}/
  ephemera/             # gitignored scratch (create locally as needed)
  project_slug.md
  AGENTS.md
  README.md
```

- Monorepo under GitHub org `bracino`. No submodules.
- **Do not** recreate phase 2/3 firmware trees until that work starts.
- Directory `node-bbu` is **MCU-neutral** (bench may be ESP32-C3 now; name does not encode flash target).

## Stack and environment

| Layer | Choice |
|--------|--------|
| Firmware | C, ESP-IDF |
| Broker | Mosquitto (retained + LWT) |
| Logic / light UI | Node-RED (non-safety-critical only) |
| History | InfluxDB + Grafana |
| Deploy | `git clone && docker compose up` on LAN |

- Dev VM: headless Ubuntu; `~/projects` shared with host.
- Shared IDF and large toolchains: `~/projects/shared` — do not vendor full IDF here.
- Secrets: env / untracked files only.
- Influx **data** ≠ config; backup separately. Stack config ∈ git.

## Issues notebook (`issues/`)

Full rules: [`issues/README.md`](issues/README.md).

| Path | Role |
|------|------|
| `issues/open/NNN-slug.md` | Active work |
| `issues/closed/NNN-slug.md` | Done / wontfix — keep lore |
| `issues/fixtures/` | Committed minimal repro material |
| `ephemera/` | Local scratch only (gitignored) |

**Session habit**

1. Skim `issues/open/` before firmware, protocol, or server work.  
2. Numbered human scribbles → file/update `NNN` issues.  
3. On finish: move to `closed/`, fill **Fix** + **Verify**; bump STATUS/ROADMAP if capability or contracts changed.  
4. Promote durable schema decisions into `docs/` once settled (issues are the trail; docs are the contract).

## Coding norms

### Firmware (ESP-IDF / C)

- Clear C; match in-tree style when it exists.
- Keep control loop + local I/O free of hard deps on ESP-NOW / WiFi / MQTT / OTA success.
- Define fail-safe sensor-fault behavior (default: don’t destroy the plant).
- Mind ESP32 footguns (stack sizes, log-in-lock, etc.); use esp-idf-build skill when building/flashing.
- Protocol renames: firmware + `server/` + `docs/` together when possible.
- Agents prepare build/flash commands; **humans** run them on hardware unless explicitly asked otherwise.

### Server

- Mosquitto, Node-RED flows (no embedded secrets), Grafana provisioning → git.
- Node-RED: schedules/thresholds/coordination — **not** offline pump interlocks.

### Docs

- Settled MQTT/ESP-NOW/BOM → `docs/`.
- `project_slug.md` only when product intent actually changes.

## Phase priority (phase 1)

**Autonomous pump control → ESP-NOW link → MQTT telemetry → server dashboards → convenience.**

## Open design debts

Undecided until written under `docs/` or closed as design issues:

- HW shape / BOM / verification  
- MQTT topics + payloads  
- ESP-NOW payloads  
- Node-RED / Grafana first cut  
- Influx backup/retention policy  

Propose minimal concrete schemas in `docs/` rather than scattering magic strings.

## Git habits

- Remote: `git@github.com:bracino/bracino.git` (SSH).
- Prefer atomic cross-layer commits for contracts.
- No build trees, secrets, `ephemera/`, venvs, `node_modules/`.

## Do not

- Network/broker in the pump critical path  
- Expand HA or cloud control without an explicit human decision  
- Pre-create future-node firmware directories  
- Nested repos/submodules  
- Claim bench results without hardware runs  
- Delete closed issues  

## Quick pointers

| Need | Where |
|------|--------|
| Product intent | `project_slug.md`, `README.md` |
| Now / next | `docs/STATUS.md`, `docs/ROADMAP.md` |
| Active work | `issues/open/` |
| Agent rules | this file |
| BBU firmware | `firmware/node-bbu/` |
| Gateway firmware | `firmware/gateway-wroom/` |
| Compose stack | `server/` |
| Shared ESP-IDF | `~/projects/shared` (outside repo) |
