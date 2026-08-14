# bbu-controller

Physical / electrical notes for the BBU control node.

Living product docs: root `README.md`, `AGENTS.md`, `docs/STATUS.md`, `docs/ROADMAP.md`. Architecture: `docs/ADR_001.txt`. CT limitation: `docs/DESIGN_NOTE_001_ct_binary_only.md`. Datasheets and plant notes: `docs/HW_REFS/`, `docs/CONTEXT/`.

## Prototype (KiCad v0.06)

Module board: ESP32-C3-ZERO, ADS1115, ZMCT103C, JQC-3FE-S-Z relay + snubber, 12 V → 5 V buck, 1.8″ TFT + encoder (not on the breadboard yet). Pin map: `module_pin_mapping_bbu_controller_prototype.csv`.

**Open hardware change (not in v0.06 yet):** drive relay `IN` with an NPN (or N-FET) from GPIO10. Module VCC = 5 V. Direct GPIO10 → IN does not work (5 V pull-up). See STATUS.

ZMCT stays on 3.3 V with the ADC. Prototype firmware will only discriminate pump current present / absent.
