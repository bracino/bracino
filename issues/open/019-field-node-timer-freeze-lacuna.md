# 019 — field node timer-freeze ~10h54m (05 Sep data lacuna)

- **Status:** open — explanation accepted by human (~90% solid); mitigation deferred to next hardware rev; detection chain added (see 021)
- **Type:** forensics / hardware hazard (availability)
- **Opened:** 2026-09-07
- **Refs:** 015 (logger gateway), 016 (control law), 017 (boiler dropout), DN003/DN004 (amended), 021 (node-gone detection)

---

## What happened (accepted narrative)

The 05 Sep install-day record contained an apparent multi-hour data
lacuna compressed into a single "leap" datapoint (recorded 14:46→14:50Z,
TPO 57.8→73.2 °C, TPU 57.0→63.1, AMB 32.7→34.3 sustained hours).

Root-cause chain, all forensically established on the JSONL record:

1. **The node's ms clock lost ~10h54m of real time.** Between the last
   pre-gap sample (capture_ms 39,154 s) and the leap sample (39,399 s),
   the timer advanced only 245 s while real time advanced dawn→16:50
   local. An honest timer demands cap ≈ 78,000 s there. Every
   honest-timer alternative is excluded by the data: loop-block with
   running timer → visible cap gap (absent); ring overflow → visible
   cap gap (absent); anchor refresh → stamp kink (absent: zero kinks in
   4797 intervals); uniform clock offset → cannot differ pre/post leap
   (06 Sep daytime shows solar-day shape, so post-leap stamps are real).
   A **chip-wide freeze that stops the timer** is the only
   cap-continuity-preserving mechanism.
2. **Trigger:** mains blip at the plant (human-accepted theory). 5 V
   logic rail carries only 20 µF bulk (BOM v0.09: C1+C4; the "10 nF"
   memory was actually C7/C8 100 nF elsewhere) → ms-scale ride-through
   → rail dove through the hang zone. BOR is enabled (level 7) but the
   transient either outpaced the detector filter or hit Espressif's
   documented "doesn't restart on brownout" errata class. See also
   esp32.com threads (t=26298 "Mysterious freeze/stop in the field" —
   measured degraded I/O levels after freeze; t=39097 "Logless system
   freeze") and edaboard "What cause a crystal paused?" (stall +
   spontaneous recovery, no reset).
3. **During the freeze (~11 h):** pump relay held ON (last state
   pre-freeze; harmless here — the boiler self-protects at its own
   setpoint), TFT/encoder dead (human: encoder unresponsive next
   morning, self-recovered later — separate wiring transient but same
   "node pauses" flavor), no logging, no control.
4. **Real damage in this event: none** (tank solar+boiler charged
   during the freeze; the leap is the first honest post-thaw sample).
   **The hazard:** the mirror case — freeze with pump OFF on a cold
   night = no hot water by morning. Availability issue, not safety.

## Why the record looked insane before this

- Pre-freeze samples carry stamps = real + ~10h54m (daytime shapes on
  what was actually night) — the "nighttime pattern at midday".
- The 73 °C leap = the tank state after ~11 unobserved hours, not
  physics compressed into 4 minutes.
- The EVENT records sat at machine-uniform 19-line intervals: drain
  pacing, not event chronology (fixed in 020).
- The probe swap (real midnight, per human memory — vindicated) sits in
  the pre-freeze stretch; the door-open AMB dip is there too.

## Corroboration: controlled decimation run (2026-09-06/07)

To rule out the FIFO decimation/drain path itself (the original
suspicion from the notes) the human ran a controlled experiment:
comms off ~20:42Z, `tel=5`, reboot at 2026-09-06T20:45:00Z (phone
timestamp ±2 s), comms back on ~8 h later — spanning ~3 decimation
cycles with no operator activity in between. Result (plots + record:
`ephemera/telemetry_bbu-decim-exp.jsonl`, `bbu-decim-exp.png`,
`decim-exp-full.png`): the clear nighttime draw-down pattern with
occasional boiler firing, timestamps lining up with the 20:45Z reboot
anchor across all decimation passes. **Decimation/drain is exonerated**
as the cause of the 05 Sep lacuna; combined with the forensic chain
above, the freeze narrative stands as the single explanation. The
several fault events clustered in the anomalous range of the 05 Sep
record are read as consequences of the freeze window, not independent
faults.

## Deferrals (human decisions 2026-09-07)

- **UPS on the 12 V rail** — root-cause fix for blips; next hardware
  rev. Gateway shares plant power and needs it too (its restore_time
  bug made every GW reboot a node-poisoning window; fixed in-tree
  2026-09-07, rides the GW flash).
- **Bulk cap 470–1000 µF on 5 V logic** — would ride out sub-second
  dips entirely; protoboard rework rejected; next rev.
- **Bench sag characterization** (ramp 12 V down/up with serial
  attached; clean-reset vs silent-wedge, fast vs slow transient) —
  deferred (node in service; work queue).
- Note: super-watchdog enablement worth checking in sdkconfig next
  flash — independent clock domain, second chance after clock-tree
  stalls.

## Fix (this tree)

- GW `restore_time()`: restore from `epoch_s` alone (DN004 amended).
- Detection chain so a recurrence *rings*, see 021.

## Verify

- [ ] GW flashed with the fix; on next power-cycle, boot log shows
      `time restored from NVS epoch_s=… (elapsed ignored)` and no
      year-2162-class stamps in the first minutes before MQTT time.
- [ ] Node gone-detection end-to-end (021).
- [ ] If a freeze recurs: serial silent + TFT frozen + telemetry stops
      simultaneously, GW raises node_gone alarm within the liveness
      window.
