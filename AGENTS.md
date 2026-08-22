# AGENTS.md — Bracino

Guidance for humans and coding agents in this repository.

## What this is

**Bracino** replaces a failed Paradigma MES-BBU controller for a central heating plant (7-unit condo, Tuscany).

**Phase 1 only (active tree):**

- `firmware/node-bbu` — autonomous BBU (thermal storage) pump control node
- `firmware/gateway` — ESP-NOW ↔ WiFi/MQTT gateway
- `server/` — Mosquitto, Node-RED, InfluxDB, Grafana (LAN, compose-from-git)
- `hardware/bbu-controller` — PCB when off protoboard

Human entry: [`README.md`](README.md). Capability: [`docs/STATUS.md`](docs/STATUS.md). Plan: [`docs/ROADMAP.md`](docs/ROADMAP.md). Kickoff scrap (historical, may be stale): [`docs/project_slug.md`](docs/project_slug.md) — do not treat as living truth.

## Non-negotiable: control autonomy

**The control node is authoritative over the pump on its own.** Local sensors + local control loop must work if gateway, broker, or backend are down. MQTT/server are supervisory (telemetry out, non-critical commands in). Reject changes that put pump safety or on/off decisions on the network path.

## Repo layout

```text
bracino/
  firmware/node-bbu/
  firmware/gateway/
  hardware/bbu-controller/
  server/{docker-compose later, mosquitto, nodered, grafana, influx-init}/
  docs/                 # STATUS, ROADMAP, schemas; historical project_slug.md
  issues/{open,closed,fixtures}/
  ephemera/             # gitignored scratch (create locally as needed)
  AGENTS.md
  README.md
  LICENSE               # MIT
```

- Monorepo under GitHub org `bracino`. No submodules.
- **Do not** recreate phase 2/3 firmware trees until that work starts.
- `node-bbu` and `gateway` names are **MCU/module-neutral** (bench parts may be C3 / WROOM today).

## Stack and environment

| Layer | Choice |
|--------|--------|
| Firmware | C, ESP-IDF |
| Broker | Mosquitto (retained + LWT) |
| Logic / light UI | Node-RED (non-safety-critical only) |
| History | InfluxDB + Grafana |
| Deploy | `git clone && docker compose up` on LAN |

- Dev VM: headless Ubuntu; `~/projects` shared with host.
- Shared IDF and large toolchains: `~/projects/share` (IDF at `~/projects/share/lib/esp/esp-idf`) — do not vendor full IDF here. Older notes said `~/projects/shared`.
- Secrets: env / untracked files only.
- Influx **data** ≠ config; backup separately. Stack config ∈ git.

## Session bridge (`session-kickoff.md`)

Local-only scratch at the **repo root**, gitignored — an ephemeral extension of this file for cross-session handoff (e.g. “continue hardware talk tomorrow”).

A second local-only file, `session-kickoff.md.bak`, is the previous-session copy. Also gitignored. It exists so a wipe, empty overwrite, or other munging of the live pad does not lose the last non-empty handoff.

**New session (agents):**

1. If `session-kickoff.md` exists and is **non-empty** (more than whitespace), **read it** and **surface its substance to the user** before diving into other work.
2. If the live file is **empty or missing**, check `session-kickoff.md.bak`. If the backup is **non-empty**, **read it**, treat it as the handoff, and **surface its substance** the same way. Mention that the live pad was empty and the backup was used.
3. After the live file (or, if that was empty, the backup) has been read/presented (or the user dismisses it), **copy the non-empty source onto `session-kickoff.md.bak`** (overwrite the previous backup). Then **zero `session-kickoff.md` without deleting it** — truncate to empty (or whitespace-only), leave the path in place so the user can keep using it as a bridge pad. Do **not** empty the `.bak`.
4. Do **not** commit `session-kickoff.md` or `session-kickoff.md.bak`. Do not treat them as durable docs; promote anything lasting into `issues/`, `docs/STATUS.md`, `docs/ROADMAP.md`, or this file.

Humans may freely overwrite the live file mid-session to stage the next bridge. Overwriting the live file does not update the backup; the backup is only refreshed by the agent step above, so a mid-session munge of the live pad still has the last ingested copy.

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
5. Do **not** maintain `docs/project_slug.md` as living truth — README / STATUS / ROADMAP / issues own current intent.

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
- Prefer STATUS/ROADMAP/issues over the historical slug.

### Hardware (KiCad)

- **Do not read** `.kicad_sch` / `.kicad_pcb` (or other KiCad source) to describe the circuit.
- For what is connected to what, use the exported **BOM**, **netlist**, and **ERC** next to the project (currently `hardware/bbu-controller/bbu_controller_prototype_kicad/*-v0.08-*`) plus `module_pin_mapping_bbu_controller_prototype.csv`.
- Humans edit the schematic in KiCad; agents treat those exports as the readable description.

## Phase priority (phase 1)

**Who/what on the wire (hardware + local control loop) first**, then ESP-NOW, then MQTT, then server dashboards, then convenience. Protocols come after the physical talkers are clear.

## Open design debts

Undecided until written under `docs/` or closed as design issues — see `issues/open/`.

Propose minimal concrete schemas in `docs/` rather than scattering magic strings.

## Git habits

- Remote: `git@github.com:bracino/bracino.git` (SSH).
- Prefer atomic cross-layer commits for contracts.
- No build trees, secrets, `ephemera/`, `session-kickoff.md`, `session-kickoff.md.bak`, venvs, `node_modules/`.
- **Commits:** agents **may create local commits without asking** when a coherent unit of work is done and the tree is intentional (scaffolding, issue filing, focused fixes). Use clear messages; don’t vacuum unrelated junk.
- **Push to GitHub:** **always ask first** unless the user explicitly ordered a push for this step (“push that”, “push to origin”, etc.). “Commit when it makes sense” ≠ permission to push.

## Do not

- Network/broker in the pump critical path
- Expand HA or cloud control without an explicit human decision
- Pre-create future-node firmware directories
- Nested repos/submodules
- Claim bench results without hardware runs
- Delete closed issues
- Push without asking (see Git habits)
- Treat `docs/project_slug.md` as authoritative over README/STATUS/ROADMAP

## Quick pointers

| Need | Where |
|------|--------|
| Product intent (current) | `README.md`, `docs/STATUS.md`, `docs/ROADMAP.md` |
| Kickoff scrap (stale OK) | `docs/project_slug.md` |
| Active work | `issues/open/` |
| Agent rules | this file |
| BBU firmware | `firmware/node-bbu/` |
| Gateway firmware | `firmware/gateway/` |
| Compose stack | `server/` |
| Shared ESP-IDF | `~/projects/share/lib/esp/esp-idf` (outside repo) |
