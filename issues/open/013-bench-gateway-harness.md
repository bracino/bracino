# 013 — Minimal bench gateway (DN003 wire-contract test harness)

- **Status:** open
- **Type:** firmware / test
- **Opened:** 2026-08-30
- **Refs:** `firmware/gateway/`, DESIGN_NOTE_003, issue 006 (skeletons), issue 011 (node client)

## Context

The 2-ESP desk test of the node's DN003 client (2026-08-30 plan) needs a
gateway-side counterpart **without** the DN004 machinery (WiFi, MQTT,
state machine, registry persistence). This spec defines the minimal bench
gateway: a DN003-speaking counterpart, not the real firmware.

## Scope

**In:** ESP-NOW init on a fixed channel, HELLO→HELLO_ACK (+`TIME_SYNC`),
HEARTBEAT/TELEMETRY_BATCH/EVENT receive + serial log, BATCH_ACK responder
(bench "durable write" = serial log line; ack after that), PARAM_SET
sender on keypress, CONFIG_GET after unknown `config_ver`, retransmit
tolerance.

**Out (deliberately):** WiFi STA, MQTT, state machine, NVS role table,
commit watermarking, maintenance AP. This harness acks on log-write, not
backend-commit — fine for wire-contract validation, **not** evidence of
DN004 compliance.

## Behavior table

| Node sends | Bench gateway does |
|---|---|
| `HELLO` (broadcast) | Reply unicast `HELLO_ACK` with TLV `TIME_SYNC` (host time via serial-set epoch, or epoch of first PC sync); log registration |
| `HEARTBEAT` | Log (liveness line); no reply |
| `TELEMETRY_BATCH` | Decode header + samples; print one line per sample (`capture_ms`→translated epoch, temps, mode, relay, ct, faults); send `BATCH_ACK(end_ms of frame)` |
| `EVENT` | Print tag + value; log |
| `CONFIG_DESC` / `PARAM_ACK` | Print |
| — | `t` keypress: send `TIME_SYNC`; `p` keypress: send canned `PARAM_SET` (e.g. setpoint ±); `s` keypress: print registry/counters |

Bench knobs (compile-time): `BENCH_NO_ACK_S` (suppress BATCH_ACK for N s to
force retransmit/stop-and-wait test), `BENCH_DROP_PCT` (random frame loss),
short ring override on the node for decimation drills.

## Bench scenarios

1. **Discovery:** gateway on ch 6; node cold boot → HELLO/HELLO_ACK/sync.
   Repeat with gateway on ch 1, then ch 11.
2. **Live telemetry:** depth-1 batches at 15 s cadence for ≥10 min; verify
   seq continuity, translation math at the gateway.
3. **Outage/drain:** gateway channel-hops away mid-run (simulates loss);
   node buffers; gateway returns → FIFO drains oldest-first, BATCH_ACK
   watermarks advance, no duplicates after settle.
4. **Pre-gateway bring-up:** node runs alone ≥1 h (fill FIFO), gateway
   appears → drain with back-dated (correct) timestamps; verify
   epoch-0-no-TX invariant (nothing sent before first sync).
5. **Decimation field test:** outage > ring capacity at full cadence →
   span-vs-count gaps in drained frames (decimation evidence), drain
   completes, loop cadence unaffected throughout (non-blocking contract).
6. **PARAM_SET round-trip:** setpoint change from bench keys → PARAM_ACK →
   loop threshold moves (verify on TFT/serial).

## Verify

Harness implemented 2026-08-31 (both projects build clean, esp32 + esp32c3):

- `firmware/shared/bracino_schema/` — shared wire-law header.
- Master: `firmware/bench-espnow-master/` (throwaway half — the real
  gateway tree stays untouched per DN004). WROOM/UART0 console, fixed
  channel 6, `c <ch>` hops for the outage drill, GPIO27 button = status
  print (strapping-pin-safe). Registry keyed by MAC; anchors node clock
  at HELLO_ACK for UTC translation; reassembles CONFIG_DESC fragments
  (2 s timeout); BATCH_ACK only after the `$ committed` log line,
  suppressed before epoch is set (node then buffers — matching the
  epoch-less invariant). Knobs: BENCH_NO_ACK_S, BENCH_DROP_PCT.
- The remote is the REAL node-bbu firmware (011 client) — wire
  validation exercises shipping code.

Still to do: run the six scenarios below on the desk pair; archive
serial logs here as wire-contract evidence.

All six scenarios pass on the desk pair; serial logs archived under
`issues/fixtures/` as the wire-contract evidence for 011.

## 2026-08-31 bench session addenda (bind-chase + RF leakage)

Three firmware defects were found and fixed while running the harness
(evidence: `issues/fixtures/013-{master,node}-bind-chase.log`,
`-ch5-ghost.log`, `-rf-bisect.log`):

1. **Telemetry sample size mismatch** — `bbu_telemetry_v1_t` was 11 B on
   the wire vs DN003's stated 12 B. Fixed by adding the `rsv` reserved
   byte; DN003 worked example updated. No schema_ver bump (wire now
   matches the documented v1).
2. **Bind one channel behind reality** — scan bound to `chans[i]` on any
   HELLO_ACK popped from the rx queue, including ACKs received during the
   *previous* dwell (master rx task busy printing sample blocks). Node
   recorded `bound ch=5/10` while the GW sat on 6; every batch send then
   NAK'd. Fix: rx frames carry the channel at rx time; bind requires
   frame-channel == dwell-channel; stale rx frames flushed at
   `go_unreachable()` (was only at `comms on`).
3. **Unicast TX pinned to a wrong peer channel** — `add_gw_peer()` set
   `peer.channel = s_channel`; ESP-NOW with non-zero peer channel transmits
   on that channel, so with a bad `s_channel` all upstream TX died while
   RX still worked. Peer channel is now 0 (follow the radio). Both sides
   also read back the driver channel after `esp_wifi_set_channel`
   (promiscuous-disable channel revert is a known risk; node retries
   without the toggle and logs loudly).

### Adjacent-channel leakage (bench-only observation)

RF bisect via the harness (`hel 6 10` / `hel 5 10` bursts + master `w 10`
promiscuous sniff, both devices on the desk at cm range):

- `hel 6 10` burst: **10/10 HELLOs** seen by the master on ch 6.
- `hel 5 10` burst: **1/10 HELLOs** reached the master on ch 6.

So ~10% adjacent-channel leakage at cm desk range — this is exactly the
ghost-bind mechanism of defect 2 (a leaked ch-6 frame heard on ch 5 is a
legitimate rx-time-channel-5 frame and passes the new guard). At field
range (rooms apart) leakage is negligible. The unreachable→rescan cycle
(~6 s) self-heals a ghost bind; no further code change needed.

Also measured: ch 6 is busy with ambient WiFi (452 frames in 10 s from AP
`94:83:c4:86:99:ca`). Fine for bench; field channel choice should account
for it (future DN004/gateway note).

## 2026-09-01 addenda (overnight outage + drain — unattended scenarios 3/4/5)

Human ran the accumulation/drain for real instead of the scripted Drill A:
gateway off **all night** (~11 h) while the node ran AUTO on sim temps.
Evidence: `issues/fixtures/013-overnight-outage-drain-{node,master}.log`.

- **Accumulate:** node SCANNING unbound all night, `fifo=3928/4096`,
  `decim_passes=7`, ~36.4k unbound HELLO TX. Pump loop (AUTO) unaffected
  the whole time — non-blocking contract holds in the field, not just bench.
- **Preserve across disable:** `comms off` → "FIFO held, no radio work";
  buffer intact through the off/on cycle.
- **Drain:** after gateway up + epoch + `comms on` → re-anchor + CONFIG_DESC
  (313 B, 2 frags) → **3928 samples drained in ~42 s** (221 frames of 18,
  acked ~180 ms), watermarks strictly monotonic, **zero retransmits,
  NAKs, or malformed lines**; apparent seq gaps are interleaved HEARTBEATs
  (seq is a shared message counter). Live cadence resumed (16 s spans).
- **UTC:** translation verified quantitatively (master epoch → HB clock
  correlation → last drained frame = 10:14:26 UTC, matches log wall time
  ±1 s) and locally contiguous (16 s span-to-span gaps = one capture
  period; first retained sample decodes to node boot + 51 min, consistent
  with uptime math).
- **Decimation signature:** drained spans show graded spacing — ~728 s per
  retained sample in the oldest region down to 15.1 s at the newest —
  exactly the DN003 "drop every other entry of the oldest half on
  ring-full" policy accumulating passes (`decim_passes=7`). Scenario 5's
  span-vs-count evidence.

Status vs the scripted drills: this **supersedes Drill A** (real 11 h
outage + drain, stronger than a 240 s ack-suppression window). Drill B
landed the same day — see the addenda below. Remaining before 013 closes:
reflash the node with the scan-on-unreachable fix and confirm a 3-fail
rebind is ~dwell, not ~50 s.

Note: node firmware now reports `ct_state = NOT_FITTED` (3) on the wire
(CT/snubber dropped, 014 — DN003/DN004 updated); the bench master decodes it.

## 2026-09-01 addenda (Drill B — BENCH_DROP_PCT=20)

Evidence: `issues/fixtures/013-drill-b-drop-{node,master}.log`.

**Drop/retransmit path: pass.** At `tel 5`, ~18 drops across ~70 committed
frames; 15 retransmits recovered 1- and 2-drop sequences (seq 203, 212,
218, 300, …). Spans stayed 5 s, fifo ≤1 while ONLINE, loop reached
RUNNING, `ct=NOT_FITTED` on the wire. One triple-drop of the *same* seq
(383, p=0.2³ ≈ 0.8 %) tripped `MAX_CONSEC_FAIL` — that's the knob doing
what it says, not a loss bug. FIFO held 11 samples through the outage
and drained on rebind (w=668090..719090).

**Scan after 3-fail looked wedged (~50 s) until a typed `comms`.** It
wasn't the status command. Timeline:

- `go_unreachable()` waited `SCAN_REST_MS` (10 s) *before the first
  attempt* — DN003 rest is between attempts, not before the first.
- Then 3 s scan + 10 s rest, three times. Cached ch 11 was first every
  attempt; attempt 1's HELLOs never appeared in the master log (master
  sat on 11 the whole time). Attempt 4 started at 723.872 — 500 ms after
  the `comms` status print at 723.352 — and bound in 6 ms on ch 11.
  Coincidence, not causation.

Firmware follow-up (same day): `go_unreachable` scans immediately; HELLO
dwell waits for the TX callback + 20 ms settle; HELLO_ACK with epoch=0
does not bind (that left the node ONLINE/unanchored at the start of this
run after the master reboot, fifo growing, acks=0). `BENCH_DROP_PCT`
reverted to 0.

Also in this log, not a firmware bug: `n 17882835999` (extra 9) set the
epoch to 2536-09-07 — that's why the committed UTC is nonsense. Integer
seconds only, and check the printed date.

After the bisect, `comms on` bound ch=6 immediately; CONFIG_DESC (2
frags), first BATCH_ACK watermark (w=1573), telemetry at 15 s cadence
with sane UTC and zero malformed lines. HELLO→ACK→CONFIG→BATCH→ACK path
is green.
