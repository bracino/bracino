# docs

| Doc | Role |
|-----|------|
| [STATUS.md](STATUS.md) | What works now |
| [ROADMAP.md](ROADMAP.md) | Phase 1 plan and later ideas |
| [ADR_001.txt](ADR_001.txt) | Accepted module-prototype architecture (v0.08) |
| [DESIGN_NOTE_001_ct_binary_only.md](DESIGN_NOTE_001_ct_binary_only.md) | Prototype CT reports running / not, not amperes |
| [DESIGN_NOTE_002_bbu_control_loop.md](DESIGN_NOTE_002_bbu_control_loop.md) | Offline BBU pump loop (MES-style TPO/TPU) |
| [DESIGN_NOTE_003_espnow_node_schema.md](DESIGN_NOTE_003_espnow_node_schema.md) | **ESP-NOW wire law** — node↔gateway envelope, telemetry FIFO, liveness, time, params (2026-08-30) |
| [DESIGN_NOTE_004_gateway_design.md](DESIGN_NOTE_004_gateway_design.md) | Gateway design: state machine, registry, MQTT topics/QoS/retention contract (2026-08-30) |
| [../issues/](../issues/README.md) | open/closed lab notebook |
| [project_slug.md](project_slug.md) | Kickoff artifact only (historical; OK stale) |

Schemas (MQTT, ESP-NOW) are settled in DESIGN_NOTE_003 / DESIGN_NOTE_004. Backend pipeline (commit service) and the cross-cutting data-integrity matrix are stubbed as DESIGN_NOTE_005 / DESIGN_NOTE_006 in DN004's follow-ups and land here when written.

The CONTEXT folder contains materials relating to the operating environment for the various bracino-project controllers and gateway; the existing physical plant, the condo and caldaia room in general, pumps, electrical and plumbing layout, and related possibly useful information.

The HW_REFS folder contains datasheets, pinouts, programming guides and so forth, for guidance during development of hardware and software.
