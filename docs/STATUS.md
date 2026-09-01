# Status

**As of:** 2026-09-01  
**Phase:** 1 — prototype (BBU control node + WiFi gateway)  
**Repo:** https://github.com/bracino/bracino

## Summary

Hardware definition for the module prototype is written (ADR 001, KiCad **v0.08**). The **protoboard is soldered to v0.08** and has passed the same I/O tests as the breadboard. `firmware/node-bbu` runs the offline loop ([DESIGN_NOTE_002](DESIGN_NOTE_002_bbu_control_loop.md)): boots **Manual**, `auto` / `sim` / `halt` on the desk. Human-reported **desk `sim` walk-through passed** (2026-08-22). Human-reported (2026-08-25): dummy **AC load** validated; NTC ice/boil two-point; USB safety block fabricated; TFT+encoder on breadboard (glyphs/flicker fixed). Human-reported (2026-08-26): landscape/SPI2/ISR image **loads and runs**; encoder is **much better**; doubled 6×8 font is **unacceptable**. Human-reported (2026-08-27): TFT+encoder **on the protoboard**; Modern DOS 8×16 **readable**. Human-reported (2026-08-28): UI menus **usable** — rotate/click/hold OK; System Data / Control Program / etc. navigate; WARN amber correct (BGR palette). Issue **010 closed**. Not plant-proven; no real BBU pump. No ESP-NOW, MQTT, or compose stack.

**ESP-NOW wire bring-up (closed 2026-09-01, issues 011 + 013):** the DN003 client is implemented in `firmware/node-bbu` (behind the `comms_enabled` NVS flag, default off — radio never in the pump path) and validated end-to-end on the desk against the throwaway bench master `firmware/bench-espnow-master` (the real `firmware/gateway` stays untouched per DN004). Proven: HELLO → HELLO_ACK+TLV time, CONFIG_GET/DESC (2 fragments), TELEMETRY_BATCH → BATCH_ACK stop-and-wait with watermark trim, sane UTC, epoch-less TX gate, overnight 11 h accumulation + 42 s drain (3928 samples, 7 decim passes), 20% frame-loss retransmit, unreachable→rescan on the cached channel in tens of ms. Wire defects found and fixed during bring-up: 11 B vs 12 B sample, scan bind-race, pinned peer channel, 10 s rest before first scan. Adjacent-channel leakage at cm desk range ~10% (ch 5↔6) — bench-only, self-heals. Evidence: [013](../issues/closed/013-bench-gateway-harness.md).

Issue **010 closed**. Not plant-proven; no real BBU pump. MQTT and compose stack not started. First boiler-room hookup was inconclusive — the node's contact closes a **230 VAC coil line** (earlier ~24 V note was wrong) to an **ABB ECB24-40 contactor** in the breaker box, which switches the pump. Pump ON worked; OFF did not (pump kept running). **Root cause found and fixed (2026-09-01):** the RC snubber across our contacts leaked enough to hold the contactor coil; **with the snubber lifted, the pump switches correctly from the node**. **CT confirm-running is a bust by construction** — pump current never flows through node wiring — so the **CT is dropped from the circuit too** (firmware ignores A0, no-CT warning suppressed; blindness is MES-BBU parity by design). Run-confirmation is deferred to the future caldaia monitor node (phase 2: CTs on pump/auger/blower + extra thermals). Install (012) unblocks pending the 009 real-pump checklist + field-image firmware pass: [issues/closed/014](../issues/closed/014-hidden-second-relay-snubber.md).

**Overnight outage drill (2026-09-01, unattended):** gateway off all night while the node ran AUTO — FIFO accumulated to 3928/4096 with 7 decimation passes, loop unaffected (non-blocking contract in the field). Morning drain: `comms off` (FIFO held) → gateway up + epoch → `comms on` → re-anchor + CONFIG_DESC + **3928 samples drained in ~42 s, watermarks monotonic, zero retransmits/malformed, UTC self-consistent end-to-end**; decimation signature (oldest-half halving) matches the DN003 policy. Frame-loss drill and scan-on-unreachable fix confirmed the same day. Logs: [013](../issues/closed/013-bench-gateway-harness.md).

CT no-current / unusable is **warning only** — stay on the standard algorithm (same blindness as the old MES-BBU, plus a notice). `TPO_ONLY` is for TPU faults only. Since 2026-09-01 the CT is **not fitted at all** (DN001 rev 2): no CT warnings can fire; run-confirmation is deliberately absent until the caldaia monitor node.

Design contracts settled 2026-08-30: **ESP-NOW wire law** ([DESIGN_NOTE_003](DESIGN_NOTE_003_espnow_node_schema.md)) — implemented and evidenced in `node-bbu` (011/013 closed) — and **gateway design + MQTT contract** ([DESIGN_NOTE_004](DESIGN_NOTE_004_gateway_design.md)), not yet built. DN002 updated: boot-in-last-known-mode + NVS-persisted parameters are field-image requirements (issue 012).

Open design work: [`issues/open/`](../issues/open/). Plan: [`ROADMAP.md`](ROADMAP.md). Kickoff scrap (not maintained): [`project_slug.md`](project_slug.md).

## What works

| Area | State |
|------|--------|
| Repo layout + git remote | Solid (phase 1 only) |
| Root README / AGENTS / MIT license | Solid |
| STATUS / ROADMAP / issues notebook | Process solid; this file tracks bench reality |
| Control-node HW definition | **Settled for the module proto** — ADR 001 + KiCad **v0.08**. Protoboard built |
| `node-bbu` firmware | **Loop in tree** — DESIGN_NOTE_002. User modes **Auto / Manual / Test / Off**. Dummy AC load OK. Ice/boil NTCs OK (conversion −5–110 °C). TFT+encoder UI **on protoboard, menus OK** (2026-08-28; 010 closed): Modern DOS 8×16, row DMA + done-wait, 5 ms SW timer, BGR palette. Boots Manual |
| `gateway` firmware | **Not started** — contract settled: DN003 (wire law) + DN004; bench harness spec in issue 013 |
| MQTT topic + payload schema | **Settled** — [DESIGN_NOTE_004](DESIGN_NOTE_004_gateway_design.md) (2026-08-30). Not implemented |
| ESP-NOW payload schema | **Settled and evidenced** — [DESIGN_NOTE_003](DESIGN_NOTE_003_espnow_node_schema.md). Implemented in `node-bbu` (011 closed) behind `comms_enabled`; bench harness 013 closed |
| Commit service (MQTT→Influx + watermarks) | **Designed in outline** — DN004 contract; DN005 to write |
| Relay drive (5 V module vs GPIO10) | **OK on protoboard** — Q1 2N3904, high = ON. Coil toggles and holds; dummy AC load validated (2026-08-25) |
| CT / current sense | **Dropped from the circuit (2026-09-01)** — [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md) rev 2. Bench was boolean-only and reliable (tables below); plant makes it moot: the contact closes a 230 VAC contactor coil (ABB ECB24-40), pump current never crosses node wiring. A0 ignored by firmware (`CT_FITTED 0`), telemetry `ct_state=NOT_FITTED`, no-CT warning suppressed. Snubber also removed (plant-proven). Run-confirmation returns via the phase-2 caldaia monitor node |
| NTCs (A1–A3) | **°C in firmware** (β=3950). Ice ~0.4 °C (770 mV); boil ~100 °C (3083 mV) was FAULT on the old 95 °C cap — conversion now −5–110 °C. Open/short = FAULT |
| 12 V / buck vs USB | **Heartbeat OK on external 5 V** (USB unplugged). J7 is a **5 V-only** jumper (buck VO ↔ board +5 V). USB-blocking holder **fabricated** |
| `server/` docker compose + provisioning | **Not started** (commit service planned — DN005 stub in DN004) |
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

Protoboard CT sweep (2026-08-22, relay ON, `s` n=64): no-load **37–38 mV** rms; 0.13 A **176–180**; 0.86 A **175–177**; 2.0 A **168** (below the fan); 3.6 A **240–242**; 4.1 A **233–236** (`mid` walks); 7.8 A **289–295**; return to no-load **37–38**. Loaded vs not is clear. Amperes are not. Stay boolean. *(Superseded 2026-09-01: CT removed from the circuit for field revs — DN001 rev 2. Bench history only; `s0` can still burst A0 by hand.)*
- Tank **operating** range **18–95 °C** stays on this 10 kΩ/10 kΩ divider (~0.76–3.1 V from ice to boil), clear of open/short. Keep the divider. ADS internal ref is not ratiometric to 3.3 V; ΔT mostly cancels rail drift.
- On-board WS2812 on GPIO10 stays dark; leave unused. Do not put a webserver on `node-bbu`. Gateway / ESP-NOW / backend wait until the offline loop is boring.

Protoboard (human-reported 2026-08-22): soldered to KiCad **v0.08**. Same I/O suite passed: heartbeat on external 5 V, relay switches loads, coil and NTCs read reasonable values. PPTC (F1) not populated; space left. ADS1115 ADDR and ALRT pins not brought out (module onboard pulls; float is fine). AC side (terminal block, relay, snubber) sits on a 1 mm plastic isolation pad.

KiCad **v0.08** BOM / netlist (do not treat `.kicad_sch` as the agent-readable source):

- **Q1** 2N3904: GPIO10 (`RELAY`) → **R1** 2 kΩ → Q1 base; Q1 collector = module `IN`; Q1 emitter = GND. Module VCC on **+5 V**. GPIO10 **high** sinks IN (coil **ON**).
- **TH1 / R4** → A1, **TH2 / R5** → A2, **TH3 / R6** → A3. Each NTC from +3.3 V to the tap; each 10 kΩ from the tap to GND.
- **D1 / R7** (2.2 kΩ): GPIO8 heartbeat LED to GND.
- **D2 / R8** (4.7 kΩ): 12 V power LED on the post-Schottky buck VIN. USB must not light it.
- **J7**: 2-pin jumper, **5 V only**. **In** = buck VO (`+5V_VO`) → board `+5 V` (C3 5 V pin included). **Out** = buck isolated from the rail even if 12 V is still on the inlet; USB free. GND unswitched. Mechanical USB-blocking holder **fabricated** (2026-08-25).
- **F1** PPTC is in the schematic; not fitted on this article.

Human-reported (2026-08-25): current image flashed and working; dummy AC load on the relay validated (CT still binary, reliable for running/not); NTC cables built and tested good; sensors potted. Ice water: TPO/TPU/AMB **0.2–0.6 °C** at **760–776 mV**. Boiling: AMB then TPO/TPU climb to **94.8 °C / 3053 mV**, then **FAULT at 3063–3083 mV** — that is ~100 °C hitting the old 95 °C software cap, not a short (short ≈ 3283 mV). TFT LED brighter with **R3 = 100 Ω** (schematic still 220 Ω). Encoder module is fine; bounce was the 5 ms A-edge poll. Human (2026-08-26): ISR rewrite **loads and runs**, encoder **much better**; doubled 6×8 font **rejected**.

Human (2026-08-27): UI moved onto the **protoboard**. Modern DOS 8×16 (CC0, 20×8 grid, col 0 blank) is **readable**, upright, LTR; no right-edge junk. MADCTL stays MY|MV|BGR with software X/Y flip (MX|MV made glyphs worse).

Human (2026-08-28): **010 closed.** Rotate and click acceptable; hold ~0.8 s returns Home; all Selection menus open distinct screens; Control Program Manual + relay ON shows amber **WARN no CT**; live digits stable. Firmware fixes: DMA done-wait (`fa7659f`); full-row blit + independent encoder timer / wall-clock hold (`e454f60`); BGR-aware `COL_RGB565` palette so amber is not deep blue (`ee3e6c3`). Light residual: serial `st` with panel unplugged not re-checked this pass.

Still open on the protoboard: connector / jumper labels, PPTC when stock arrives. Do not put the real BBU pump on this image yet.

Shared ESP-IDF is under `~/projects/share/lib/esp/esp-idf`.

## Known constraints (always true)

- Pump on/off must remain correct with gateway/broker/server **down**.
- No WAN/cloud dependency for heat.
- Secrets never in git (Node-RED flows, compose, firmware sources).
- Shared ESP-IDF lives outside this repo.

## Architecture (current tree)

```text
firmware/node-bbu/         local loop (DESIGN_NOTE_002) + serial / sim + TFT UI
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
