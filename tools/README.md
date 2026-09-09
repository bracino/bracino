# tools

Bench/forensics aids — not part of the deployed stack, not design
artifacts. Committed because they parse the DN005 JSONL contract and
have repeatedly proven decisive in data forensics (issues 019, 022,
015 record-hygiene note). Scratch copies live in gitignored `ephemera/`;
this directory is the canonical home once a tool graduates.

## plot_telemetry.py — JSONL telemetry plotter (headless + interactive)

Renders the commit-service JSONL (`kind: telemetry` / `event` /
`gw_ambient`) to PNG or an interactive window: TPO/TPU panel + AMB
panel (node probe + GW probe overlay), pump on/off edge lines with gray
shading between (relay_state), event markers
(fault raise/clear), boot-session start markers (one per distinct
`boot_session` tag), and automatic line-breaks at time gaps AND
boot-session changes — interleaved streams (bench drills share
node_id with the field node) are never connected across sessions.
See issues/closed/015 (record hygiene) and closed/022.

```bash
# headless PNG (agent analysis, printable)
python3 tools/plot_telemetry.py <file.jsonl> --out chart.png \
    [--start ISO8601] [--end ISO8601] [--timezone Europe/Rome]

# interactive (toolbar pan/zoom/save; 'w' = silent snapshot PNG
# written next to --out)
python3 tools/plot_telemetry.py <file.jsonl> --out chart.png --show
```

Notes:
- `--start`/`--end` are UTC.
- Known data traps baked in: bench-drill streams (boot tags 92/170 in
  the Sep-2026 record), the gw_ambient misrange era (pre-
  2026-09-06T14:49Z, masked), and the ~0.3 °C post-boot TPO/TPU
  warm-up transient (real, tiny, documented in closed/022).
- Deps: pandas, matplotlib (the t520/dev-VM python; no venv needed for
  quick runs).
