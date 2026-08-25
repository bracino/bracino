# Status

**As of:** 2026-08-25 (UI breadboard in tree)  
**Phase:** 1 — prototype (BBU control node + WiFi gateway)  
**Repo:** https://github.com/bracino/bracino

## Summary

Hardware definition for the module prototype is written (ADR 001, KiCad **v0.08**). The **protoboard is soldered to v0.08** and has passed the same I/O tests as the breadboard. `firmware/node-bbu` runs the offline loop ([DESIGN_NOTE_002](DESIGN_NOTE_002_bbu_control_loop.md)): boots MANUAL, `auto` / `sim` on the desk. Human-reported **desk `sim` walk-through passed** (2026-08-22). Human-reported (2026-08-25): current image **flashed and working**; dummy **AC load** on the relay validated (CT still binary, reliable for that); NTC cables built and good; sensors potted, curing; USB safety block fabricated. Temperature calibration deferred. Not plant-proven; no real BBU pump. No ESP-NOW, MQTT, or compose stack.

CT no-current / unusable is **warning only** — stay on the standard algorithm (same blindness as the old MES-BBU, plus a notice). `TPO_ONLY` is for TPU faults only.

Open design work: [`issues/open/`](../issues/open/). Plan: [`ROADMAP.md`](ROADMAP.md). Kickoff scrap (not maintained): [`project_slug.md`](project_slug.md).

## What works

| Area | State |
|------|--------|
| Repo layout + git remote | Solid (phase 1 only) |
| Root README / AGENTS / MIT license | Solid |
| STATUS / ROADMAP / issues notebook | Process solid; this file tracks bench reality |
| Control-node HW definition | **Settled for the module proto** — ADR 001 + KiCad **v0.08**. Protoboard built |
| `node-bbu` firmware | **Loop in tree** — DESIGN_NOTE_002 in `control.c`. User modes **Auto / Manual / Test / Off** (no Normal). Desk `sim` passed. Image **flashed and working** (human, 2026-08-25, pre-UI). Dummy AC load OK. Boots Manual. TFT+encoder menus in tree, not bench-proven |
| `gateway` firmware | **Not started** |
| MQTT topic + payload schema | **Not decided** |
| ESP-NOW payload schema | **Not decided** |
| Relay drive (5 V module vs GPIO10) | **OK on protoboard** — Q1 2N3904, high = ON. Coil toggles and holds; dummy AC load validated (2026-08-25) |
| CT / current sense | **Binary only** — [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md). Dummy load: reliable running/not. No-current is **warn only** (DESIGN_NOTE_002) |
| NTCs (A1–A3) | **°C in firmware** (β=3950). A1=TPO, A2=TPU, A3=AMB (print only). Open/short = FAULT. Cables built; sensors potted, curing. °C vs thermometer still open |
| 12 V / buck vs USB | **Heartbeat OK on external 5 V** (USB unplugged). J7 is a **5 V-only** jumper (buck VO ↔ board +5 V). USB-blocking holder **fabricated** |
| `server/` docker compose + provisioning | **Not started** |
| Grafana dashboards / Node-RED flows | **Not started** |
| Influx backup/retention policy | **Not decided** |

## Bench — do not overclaim

Verified by hand on the **breadboard** (2026-08-14 / 15), then repeated on the **v0.08 protoboard** (human-reported 2026-08-22). Agents have not re-run these.

Breadboard (2026-08-14 / 15):

- 12 V → buck 5 V → MCU 3.3 V rails.
- ADS1115 at 0x48 on GPIO7 SDA / GPIO6 SCL.
- ZMCT103C on 3.3 V, A0, pot **2 turns CCW** from full CW: relay off ≈ 0 mV rms; relay on / no load ≈ 37 mV; ~0.15 A fan ≈ 175 mV. Dryer currents 0.84–7.8 A are **not** monotonic — do not treat A0 as amperes.
- Q1 on GPIO10; module VCC **5 V**. Coil toggles as commanded and holds. Later: switches bench loads.
- NTC dividers (TH1/R4 → A1, TH2/R5 → A2, TH3/R6 → A3). Lab ambient **28 °C**: mid **1758–1769 mV**, rms **0**, pp **0–1 mV**. Warming **raises** mid; cooling **lowers** it. Direction matches NTC from +3.3 V to the tap, 10 kΩ to GND.
- A3 faults: **open** mid **13 mV**; **short** mid **3283 mV**; restore **1762–1769 mV**. Firmware must treat those rails as **fault**, not °C.
- CT: ~0.13 A → A0 rms **168–173 mV**; contacts closed / no load → **37–38 mV**. `mid` still walks. Same gap as 2026-08-14 ([DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md)).

Protoboard CT sweep (2026-08-22, relay ON, `s` n=64): no-load **37–38 mV** rms; 0.13 A **176–180**; 0.86 A **175–177**; 2.0 A **168** (below the fan); 3.6 A **240–242**; 4.1 A **233–236** (`mid` walks); 7.8 A **289–295**; return to no-load **37–38**. Loaded vs not is clear. Amperes are not. Stay boolean.
- Tank range **18–95 °C** stays on this 10 kΩ/10 kΩ divider (~1.4–3.1 V), clear of open/short. Keep the divider. ADS internal ref is not ratiometric to 3.3 V; ΔT mostly cancels rail drift.
- On-board WS2812 on GPIO10 stays dark; leave unused. Do not put a webserver on `node-bbu`. Gateway / ESP-NOW / backend wait until the offline loop is boring (thermometer still open).

Protoboard (human-reported 2026-08-22): soldered to KiCad **v0.08**. Same I/O suite passed: heartbeat on external 5 V, relay switches loads, coil and NTCs read reasonable values. PPTC (F1) not populated; space left. ADS1115 ADDR and ALRT pins not brought out (module onboard pulls; float is fine). AC side (terminal block, relay, snubber) sits on a 1 mm plastic isolation pad.

KiCad **v0.08** BOM / netlist (do not treat `.kicad_sch` as the agent-readable source):

- **Q1** 2N3904: GPIO10 (`RELAY`) → **R1** 2 kΩ → Q1 base; Q1 collector = module `IN`; Q1 emitter = GND. Module VCC on **+5 V**. GPIO10 **high** sinks IN (coil **ON**).
- **TH1 / R4** → A1, **TH2 / R5** → A2, **TH3 / R6** → A3. Each NTC from +3.3 V to the tap; each 10 kΩ from the tap to GND.
- **D1 / R7** (2.2 kΩ): GPIO8 heartbeat LED to GND.
- **D2 / R8** (4.7 kΩ): 12 V power LED on the post-Schottky buck VIN. USB must not light it.
- **J7**: 2-pin jumper, **5 V only**. **In** = buck VO (`+5V_VO`) → board `+5 V` (C3 5 V pin included). **Out** = buck isolated from the rail even if 12 V is still on the inlet; USB free. GND unswitched. Mechanical USB-blocking holder **fabricated** (2026-08-25).
- **F1** PPTC is in the schematic; not fitted on this article.

Human-reported (2026-08-25): current image flashed and working; dummy AC load on the relay validated (CT still binary, reliable for running/not); NTC cables built and tested good; sensors potted, now curing; USB safety block fabricated. Temperature calibration deferred until the potting is done.

Still open on the protoboard: connector / jumper labels, PPTC when stock arrives. UI (TFT + encoder) is on a **breadboard** for validation ([010](../issues/open/010-tft-encoder-menu.md)); not on the protoboard yet. °C vs thermometer still open. Do not put the real BBU pump on this image yet.

Shared ESP-IDF is under `~/projects/share/lib/esp/esp-idf`.

## Known constraints (always true)

- Pump on/off must remain correct with gateway/broker/server **down**.
- No WAN/cloud dependency for heat.
- Secrets never in git (Node-RED flows, compose, firmware sources).
- Shared ESP-IDF lives outside this repo.

## Architecture (current tree)

```text
firmware/node-bbu/         local loop (DESIGN_NOTE_002) + serial / sim
firmware/gateway/          placeholder README
hardware/bbu-controller/   KiCad prototype v0.08 + pin map
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

Flash/monitor is a human step (`/dev/ttyACM0` on the C3-Zero; port may drop on reset — retry, do not rebuild from scratch). Monitor quit: **Ctrl+]**. Pull **J7** before plugging USB. No compose stack yet.

## License

MIT — see root `LICENSE`.
