# 018 — Bench C3 TX radiation failure; commissioning RF smoke-test for new nodes

- **Status:** open
- **Type:** hardware / bench process
- **Opened:** 2026-09-06
- **Refs:** issues/closed/013 (proven bench pair), `firmware/gateway` b34efe9 (raw/drop/rssi counters), `firmware/node-bbu` bdf4a44 (serial fw hash), `docs/STATUS.md` (pending)

## Symptom

Breadboard sim node (C3, mac `7c:4f:ad:d1:7f:d8`) transmits HELLOs that
no receiver ever hears — while every firmware-level counter reports
success.

## Evidence trail (2026-09-06 desk session)

- Node scan state machine healthy: bounded scan, `tx_ok=182` over 7
  attempts (26 HELLOs each), channel dwell order cached → 1/6/11 → 1..13.
  GW on ch1 = `belcanto_ospiti` house AP channel (consistent across
  sessions; the "house AP on 6" note in 1a46216 is stale lore).
- GW `s` (b34efe9): `raw=0 drop=0` through **two pinned `hel 1 100`
  bursts at 10 Hz**. `raw` counts every ESP-NOW recv-callback invocation
  before any validation — so this is driver truth: the GW recv_cb never
  fired once. GW STA association (broker/MQTT) working throughout, so
  radio front-end hears a strong AP fine.
- Exonerated in firmware: GW `esp_wifi_set_ps(WIFI_PS_NONE)` already set
  before start (net.c:512); ESP-NOW enable path runs (mode=ACTIVE);
  shared schema identical since node's build (`7305033..HEAD` has zero
  `firmware/shared/` changes) — no proto drift; GW never received a
  frame on any firmware revision it has ever run.
- Cross-check on **independent hardware + firmware**: flashed the
  original bench-master firmware (013, proven RX with this wire law) to
  the original WROOM bench-master board. Also deaf to the node's bursts.
- Node reflash + fresh PHY calibration: log still shows
  `phy_init: saving new calibration data because of checksum failure`.
- Proximity test (cm range): fail. `tx_ok=100` each burst, zero frames
  on air.

## Verdict

TX radiates nothing at any range despite MAC-level send success
(broadcast sends are ack-free, so `tx_ok` only proves driver acceptance).
With two independent receivers both deaf at cm range post-calibration,
this is a board-level fault. Suspect module RF path / counterfeit module;
the recurring PHY-checksum-failure is consistent with a bad calibration
NVM region.

Whether the node's RX side also works is untested and moot: a node must
TX constantly, so the board is a donor either way.

**Open question (human to fill):** was this exact C3 (`7c:4f:ad:d1:7f:d8`)
part of the proven 013 bench pair? If yes the radio degraded in service
(ESD?); if no, it never worked — a module-quality flag (counterfeit C3
supermini radios are known flaky).

## Disposition

- Board physically tagged: `C3 7c:4f:ad:d1:7f:d8 — TX no radiate, raw=0
  @cm, GW b34efe9 + benchmaster both deaf, phy cksum fail x2. DO NOT USE
  as node.`
- Replacement C3 (spare, pins to be soldered) becomes the sim-node.
- **Golden fixtures:** original WROOM bench-master board is labelled
  `bench-master-fixture — golden board` for the project's duration. Once
  a working C3 is proven, it becomes the golden node fixture likewise.
  Future bench work pins against golden boards to minimize variables.

## Commissioning RF smoke-test (process change)

Any new node/gateway hardware must pass an **RF smoke test before any
protocol bring-up** — the failure above cost a session and was invisible
to every firmware counter:

1. Fresh flash, check boot log for `phy_init: ... checksum failure`
   (recurrence = module suspect immediately).
2. Pair against a **golden board** (never a build-in-progress).
3. Known-good RX side runs instrumented firmware (GW ≥ b34efe9:
   `raw=/drop=/last_rssi=` counters in `s`).
4. Node: `comms off` → `hel <ch> 100` pinned burst; confirm `raw=HELLO>0`
   on the receiver at ≤30 cm.
5. Only then: `comms on`, scan/bind drills.

Driver `tx_ok` ≠ radiated power for broadcast; only a counter at the far
radio adjudicates. This is the same lesson as 013's "firmware belief vs
driver truth" — now extended to the TX side.

## Fix

_(pending: replacement C3 on fixture, GW sees HELLO)_

## Verify

_(pending: raw=HELLO count climbing on GW b34efe9 `s`; LED leaves
5-blink state; bind completes)_