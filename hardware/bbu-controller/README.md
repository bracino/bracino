# bbu-controller

Physical / electrical notes for the BBU control node.

Living product docs: root `README.md`, `AGENTS.md`, `docs/STATUS.md`, `docs/ROADMAP.md`. Architecture: `docs/ADR_001.txt`. CT limitation: `docs/DESIGN_NOTE_001_ct_binary_only.md`. Datasheets and plant notes: `docs/HW_REFS/`, `docs/CONTEXT/`.

## Prototype (KiCad v0.08)

Project lives here, next to the notes: `bbu_controller_prototype_kicad/` (`.kicad_pro` / `_sch` / `_pcb` plus the v0.08 BOM, netlist, and ERC export). Pin map: `module_pin_mapping_bbu_controller_prototype.csv`.

**Agents:** describe the circuit from the BOM / netlist / ERC, not from `.kicad_sch`.

Module board: ESP32-C3-ZERO, ADS1115, ZMCT103C, JQC-3FE-S-Z relay + Q1 2N3904 + snubber, three NTC 10 kΩ dividers, 12 V → 5 V buck, 1.8″ TFT + encoder (UI not on the protoboard yet). First article is a 12×7 cm protoboard soldered to this schematic.

Relay drive (v0.08 netlist): GPIO10 (`RELAY`) → R1 2 kΩ → Q1 base; Q1 collector = module `IN`; Q1 emitter = GND. Module VCC = **5 V**. GPIO10 high sinks IN (coil ON). Direct GPIO10 → IN does not work (5 V pull-up). Firmware matches that polarity (boot holds GPIO10 low). See STATUS.

NTC dividers: TH1/R4 → A1, TH2/R5 → A2, TH3/R6 → A3; NTC from +3.3 V to the tap, 10 kΩ to GND.

GPIO8 → D1 → R7 2.2 kΩ → GND (heartbeat). D2 / R8 4.7 kΩ is the 12 V power LED on post-Schottky buck VIN (USB must not light it).

**J7** is a 2-pin **5 V-only** jumper: buck VO (`+5V_VO`) ↔ board +5 V. Jumper out isolates the buck even if 12 V is still on the inlet. Do not plug USB with J7 in. Mechanical USB-blocking holder **fabricated** (2026-08-25).

ZMCT stays on 3.3 V with the ADC. Prototype firmware will only discriminate pump current present / absent.

ADS1115 ADDR and ALRT are not wired on the protoboard (module onboard pulls). F1 PPTC is in the schematic, not fitted on this article; space left.

AC-side parts (terminal block, relay, snubber) sit on a 1 mm plastic isolation pad so 220 V does not touch protoboard copper.
