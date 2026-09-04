# 017 — Boiler-dropout user warning (detect "boiler gone" from TPO/TPU trends)

- **Status:** open
- **Type:** enhancement (warning only — not a pump-stop condition)
- **Opened:** 2026-09-04
- **Refs:** 016 (control law), 015 (field logger — needs its data first), `docs/DESIGN_NOTE_002_bbu_control_loop.md`

## Context

Human observation (2026-09-04, during the 016 control-law discussion): when
the boiler stops contributing mid-charge, the visible fingerprint is TPO
falling toward TPU (or even below it) while **both** decline with the pump
running — the pump is equalising the store through an idle jacket.

The 016 backstop already *stops* the pump at the unambiguous end of this
scenario (dT collapsed AND TPO fallen back to `setpoint − hysteresis` after
a peak above it). This issue is about the **earlier, ambiguous phase**: TPO
sliding toward TPU, both declining, TPO maybe still well above the restart
level — no stop should fire there, but the operator should be told the
boiler is not contributing.

## Proposal (user, 2026-09-04)

A **user warning only** — no mode change, pump keeps running:

- While RUNNING: TPO declining over some window, (TPO − TPU) shrinking
  (or TPU > TPO within slack), both trending down → raise warn
  (amber on UI, serial log, ESP-NOW event when comms exist).
- Clears when TPO resumes rising or the cycle ends.

Do **not** wire it into the stop gate — the backstop owns the terminal case.
Implementation deliberately deferred until 015's logger drains some real
history so the decline-rate window and slack can be tuned against data
(summer = few cycles).

## Note

TPU > TPO + 1 °C already flips `TPO_ONLY` (severe, inversion). This warning
is the softer, earlier signal and must not duplicate or gate that path.

## Fix

`tbd` — `firmware/node-bbu/main/control.c` warn flag + UI, after 015 data.

## Verify

- [ ] Filed only — no code yet
