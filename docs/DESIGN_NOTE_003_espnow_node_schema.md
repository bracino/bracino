# DESIGN_NOTE_003 — ESP-NOW node ↔ gateway protocol schema

**Status:** proposed (consolidated)
**Date:** 2026-08-29
**Scope:** Wire schema for ESP-NOW traffic between any sensor/control node
(`node-bbu` first, others later) and `gateway`. This note is the **wire law**
for both firmwares.

Out of scope (gateway firmware work, tracked in
[DESIGN_NOTE_004](DESIGN_NOTE_004_gateway_design.md)): gateway ↔ MQTT topic
and payload mapping, gateway internal task layout / registry implementation,
maintenance SoftAP. Where this note and DN004 overlap (backlog handling,
ack semantics, ESP-NOW gating), **this note wins**; DN004 must be brought
into line (see Open follow-ups).

Related: [DESIGN_NOTE_001](DESIGN_NOTE_001_ct_binary_only.md) (CT binary),
[DESIGN_NOTE_002](DESIGN_NOTE_002_bbu_control_loop.md) (control loop /
fault semantics), issues `004` (MQTT schema), `005` (ESP-NOW payload).

---

## Consolidation decisions (vs. the two prior drafts)

This note replaces both prior drafts. Where they disagreed, the following
was decided:

1. **Single telemetry stream.** Standalone `TELEMETRY` is removed. All
   telemetry — live and backlog alike — flows through one per-node FIFO and
   is sent as `TELEMETRY_BATCH` ("a batch of one is just a batch"). This
   dissolves the live-vs-backlog ordering problem draft 1 carried as an
   open follow-up.
2. **Stop-and-wait acknowledgment.** The node has exactly one outstanding
   batch frame. `BATCH_ACK` carries a `capture_ms` watermark; the node trims
   all FIFO entries at or below it. This replaces the earlier "highest
   contiguous sequence durably committed" scheme, which deadlocks against
   decimation gaps and stalls on lost non-telemetry frames. (DN004 must
   adopt this in place of its contiguous-seq ack section.)
3. **No `HISTORY` flag.** With the FIFO architecture, drain time in the
   sub-minute range, and non-critical live data, arrival-time routing adds
   confusion without value. Everything is written in order with computed
   timestamps; consumers see a brief lag during drain. Flag dropped.
4. **`MORE_FRAGMENTS` stays** (draft 1 wins): `CONFIG_DESC` can exceed the
   ESP-NOW payload ceiling and needs fragmentation; broadcast HELLO does
   not carry it (see CONFIG_DESC).
5. **`BATCH_ACK` is a first-class message type** (draft 2 wins), defined as
   a fixed 4-byte struct, not a TLV tag.
6. **`CONFIG_DESC` is not advertised in envelopes.** `HELLO` carries a
   `config_ver`; the gateway fetches the descriptor once per version change.
7. **Message-type enum values are fixed** (see Message types).
8. **`node_clock_ms` is always ms-since-boot.** Epoch translation is
   gateway-side, anchored by `TIME_SYNC` + `boot_session`.
9. **Decimation policy is specified** (memory-pressure fallback), and the
   FIFO ring size is a per-node-type constant (90 KB for node-bbu phase 1).

---

## Motivation

Bracino will eventually host multiple, heterogeneous node types (BBU
control, weather station, driveway monitor, pool refill controller, …)
with different sensors, different actuators, and different power/duty-cycle
profiles (always-on vs. battery/sleep). Rather than invent a bespoke packet
per node type, this note defines one envelope and a small set of message
types that every node type reuses. A node's *capabilities* are
self-described (`CONFIG_DESC`), not hardcoded into the gateway.

This does not change the core architecture principle: **the control node is
authoritative over its own actuators.** ESP-NOW is a telemetry/supervisory
channel. No node's control loop may block on, or degrade unsafely without,
this link.

## Non-goals (phase 1)

- **No transport-level encryption/auth.** Risk accepted: rural/isolated
  site, boiler room is effectively RF-shielded (underground, thick walls,
  no WiFi inside), single admin user. Nodes must keep working with the
  gateway absent regardless. Revisit if the network ever becomes
  internet-reachable or the physical threat model changes.
- **No sub-second cross-node time alignment.** Coarse alignment via
  periodic `TIME_SYNC` is acceptable for phase 1. **Flag for later:** if a
  use case ever needs tight cross-node event correlation, this schema will
  need a drift-triggered resync or a proper sync protocol — noted here so
  it isn't a surprise.
- **No OTA transport in this note.** OTA is an eventual feature; when it
  lands, `proto_ver`/`schema_ver`/`config_ver` fields below are what make
  mixed old/new firmware on the network survivable in the meantime.
- **Flash wear from local backlog storage:** explicitly a non-issue at
  observed outage frequency (~2×/year typical at ~1 hr, ~4×/year extreme
  peaking at 8 hr). Not designed against.
- **Telemetry backlog survives node reboot:** not provided. The FIFO is
  RAM; a node reboot during an outage loses buffered telemetry history
  (control behavior is unaffected — DN002 owns that). At Bracino's outage
  profile this is acceptable; revisiting means moving the FIFO to flash,
  which is the non-goal above.

---

## Envelope (fixed, every message)

```c
typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;      // envelope format version
    uint8_t  node_type;      // see node_type registry
    uint8_t  node_id;        // logical instance within node_type
    uint8_t  msg_type;       // see message type table
    uint16_t seq;            // rolling frame counter — gap/loss/replay detection
    uint32_t node_clock_ms;  // ALWAYS ms since boot (see Time)
    uint8_t  flags;          // bitfield — see below
    uint8_t  boot_session;   // changes only on node reboot
    uint8_t  payload_len;
    uint8_t  payload[];      // interpretation depends on msg_type
} espnow_envelope_t;
```

13 bytes of overhead (little-endian multi-byte fields, matching the
ESP32/Xtensa native order), leaving comfortable room under the ~250-byte
ESP-NOW application payload ceiling (verify the actual limit against the
IDF version in use before freezing frame-capacity constants).

`node_type`, `msg_type`, TLV tags, fault ids, and `param_id` live in **one
canonical shared registry** — a single shared header (`espnow_schema.h`,
or a generated table) included by **both** node and gateway firmware — so
a new type, message, or parameter can't drift out of sync between
codebases.

### Envelope, byte-by-byte

| Offset | Field | Size | Purpose |
|---|---|---|---|
| 0 | `proto_ver` | 1 B | Envelope format version |
| 1 | `node_type` | 1 B | Registry entry (BBU=1, weather=5, …) |
| 2 | `node_id` | 1 B | Instance within type — the persistent role slot |
| 3 | `msg_type` | 1 B | Dispatch key |
| 4–5 | `seq` | 2 B | Rolling frame counter, +1 per transmitted envelope (any type); wraps at 2¹⁶ |
| 6–9 | `node_clock_ms` | 4 B | ms since boot — never re-based; wraps at ~49.6 days (Time) |
| 10 | `flags` | 1 B | Bitfield — see below |
| 11 | `boot_session` | 1 B | Changes only on reboot |
| 12 | `payload_len` | 1 B | Length of what follows |
| 13… | `payload[]` | variable | Interpreted per `msg_type` |

**`flags` bits:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `MORE_FRAGMENTS` | This frame is followed by more fragments of the same logical message (`CONFIG_DESC`); see Fragmentation |
| 1 | `SLEEPY` | Sender is a `PERIODIC_WAKE` node — receiver must not expect steady-frame liveness |
| 2 | `RELAYED` | **Reserved**, not assigned — earmarked for future multi-hop relay |
| 3–7 | — | Reserved; transmit as 0, ignore on receive |

### `seq` semantics

`seq` counts *frames*, not samples. It increments once per transmitted
envelope regardless of message type and is used for gap/loss/replay
detection on the receive side. It is **not** the telemetry trimming key —
that is the `BATCH_ACK` capture-time watermark (see Unified telemetry
FIFO). Gaps in `seq` across different message types are expected (e.g. a
lost HEARTBEAT is never retransmitted); the gateway treats `seq` gaps as
informational, never as a reason to stall or discard traffic.

## Message types (`msg_type`)

Values are **frozen once assigned** (append-only, like `node_type`). New
types append at the end of the free range. Never renumber, never reuse.

```c
/* espnow_schema.h — shared canonical header, node AND gateway firmware. */
typedef enum {
    MSG_INVALID         = 0x00,  // reserved — never transmit; zeroed buffers must not dispatch
    /* -- session / discovery -- */
    MSG_HELLO           = 0x01,  // node → gw (broadcast)  TLV
    MSG_HELLO_ACK       = 0x02,  // gw → node (unicast)    TLV, carries TIME_SYNC
    /* -- liveness / time -- */
    MSG_HEARTBEAT       = 0x03,  // node → gw              envelope only, no payload
    MSG_TIME_SYNC       = 0x04,  // gw → node              time_sync_t (6 B)
    /* -- telemetry (unified FIFO, stop-and-wait) -- */
    MSG_TELEMETRY_BATCH = 0x05,  // node → gw              batch_hdr_t + N samples
    MSG_BATCH_ACK       = 0x06,  // gw → node              capture_ms watermark (u32 LE)
    /* -- asynchronous -- */
    MSG_EVENT           = 0x07,  // node → gw              TLV (event registry)
    /* -- configuration / parameters -- */
    MSG_CONFIG_GET      = 0x08,  // gw → node              TLV
    MSG_CONFIG_DESC     = 0x09,  // node → gw              TLV, may fragment
    MSG_PARAM_GET       = 0x0A,  // gw → node              TLV {param_id}
    MSG_PARAM_SET       = 0x0B,  // gw → node              TLV {param_id, value, admin_seq, ttl_s}
    MSG_PARAM_ACK       = 0x0C,  // node → gw              TLV {param_id, result, prev, new}
} msg_type_t;
```

| Value | Name | Direction | Encoding | Notes |
|---|---|---|---|---|
| 0x00 | `INVALID` | — | — | Reserved; receiving one = malformed frame: drop + count |
| 0x01 | `HELLO` | node → gw (broadcast) | TLV | Registration on boot/wake; carries `config_ver` |
| 0x02 | `HELLO_ACK` | gw → node (unicast) | TLV | Confirms registration; **always** carries a fresh `TIME_SYNC` value; may carry a rejection reason |
| 0x03 | `HEARTBEAT` | node → gw | envelope only | ALWAYS_ON liveness, ~2 s; zero payload is the point |
| 0x04 | `TIME_SYNC` | gw → node | fixed struct, 6 B | Standalone push, 1×/hour for ALWAYS_ON nodes |
| 0x05 | `TELEMETRY_BATCH` | node → gw | header + N samples | The only telemetry type; stop-and-wait, one outstanding frame |
| 0x06 | `BATCH_ACK` | gw → node | fixed struct, 4 B | Value = `capture_ms` watermark of the last sample in the acked frame |
| 0x07 | `EVENT` | node → gw | TLV | Event registry (`FAULT_RAISED`, `PARAM_CHANGED`, …) |
| 0x08 | `CONFIG_GET` | gw → node | TLV | Triggered by unknown/changed `config_ver` in HELLO |
| 0x09 | `CONFIG_DESC` | node → gw | TLV, may fragment | Parameter/capability manifest; fragments use `MORE_FRAGMENTS` |
| 0x0A | `PARAM_GET` | gw → node | TLV | |
| 0x0B | `PARAM_SET` | gw → node | TLV | Carries `admin_seq` + `ttl_s` (60 s default) |
| 0x0C | `PARAM_ACK` | node → gw | TLV | `result` ∈ {OK, REJECTED_RANGE, REJECTED_TYPE, EXPIRED} |
| 0x0D–0xFF | — | — | — | Unassigned, append-only |

Design notes on the numbering:

- **0x00 = INVALID** catches the "struct was zeroed and never filled in"
  class of bug at dispatch time, cheaply.
- **Pairing discipline:** request/response pairs sit at adjacent values
  (HELLO/HELLO_ACK, CONFIG_GET/CONFIG_DESC, PARAM_GET/…/PARAM_ACK,
  TELEMETRY_BATCH/BATCH_ACK), so a field packet capture reads by eye.
- **No NACK type.** Every gateway failure mode is "go silent" (ESP-NOW is
  torn down when the backend path fails — see Gateway ESP-NOW gating), so
  nodes key off *absence*, not negative acks. Rejections piggyback on
  `HELLO_ACK` / `PARAM_ACK`.
- **Tombstone:** a single-sample live `TELEMETRY` type existed in an early
  draft and was removed in consolidation (see decision 1). Value 0x05 is
  deliberately *not* a resurrection of it. Do not re-add a competing
  live-telemetry type; the FIFO makes a depth-1 batch the live path.

## node_type registry (append-only)

| Value | Type |
|---|---|
| 1 | BBU control |
| 2 | ACS monitor |
| 3 | Boiler monitor |
| 4 | Heat loads monitor |
| 5 | Weather station |
| … | reserve as added |

## TLV convention

`uint8_t tag, uint8_t len, uint8_t value[len]`, little-endian multi-byte
values. Unknown tags are skipped by parsers that don't recognize them —
this is the forward-compatibility mechanism, and it's what lets a weather
station and a pool controller share one wire format without either side
hardcoding the other's fields.

**Tag registry** (one namespace shared by all TLV messages; append-only,
frozen once assigned):

| Tag | Name | Value encoding | Used in |
|---|---|---|---|
| 0x00 | `INVALID` | — | never transmit |
| 0x01 | `NODE_TYPE` | u8 | HELLO |
| 0x02 | `NODE_ID` | u8 | HELLO |
| 0x03 | `LIVENESS_MODE` | u8 (1=ALWAYS_ON, 2=PERIODIC_WAKE) | HELLO |
| 0x04 | `LIVENESS_PARAM` | u16 LE, seconds | HELLO |
| 0x05 | `SCHEMA_VER` | u8 | HELLO |
| 0x06 | `CONFIG_VER` | u8 | HELLO |
| 0x07 | `WAKE_REASON` | u8 (1=POWER_ON, 2=WAKE_FROM_SLEEP, 3=WATCHDOG_RESET) | HELLO |
| 0x08 | `TIME_SYNC` | time_sync_t (6 B) | HELLO_ACK |
| 0x09 | `REJECT_REASON` | u8 | HELLO_ACK (optional) |
| 0x0A | `PARAM_ID` | u8 | PARAM_GET / PARAM_SET / PARAM_ACK |
| 0x0B | `PARAM_VALUE` | n B (type from descriptor) | PARAM_SET / PARAM_ACK |
| 0x0C | `PARAM_DESCRIPTOR` | n B (see Parameters) | CONFIG_DESC |
| 0x0D | `ADMIN_SEQ` | u32 LE | PARAM_SET |
| 0x0E | `TTL_S` | u8 | PARAM_SET |
| 0x0F | `PARAM_RESULT` | u8 (0=OK, 1=REJECTED_RANGE, 2=REJECTED_TYPE, 3=EXPIRED) | PARAM_ACK |
| 0x10 | `PREV_VALUE` | n B | PARAM_ACK |
| 0x11 | `NEW_VALUE` | n B | PARAM_ACK |
| 0x12–0xFF | — | — | unassigned, append-only |

**Event ids are a separate registry** (see EVENT section) — they name the
*semantics* of an EVENT payload, not fields of a message.

## Worked example — live BBU telemetry as a depth-1 batch

Normal operation: FIFO depth 1, one sample per frame. Mode `AUTO`,
relay on, CT `RUNNING`, TPO/TPU/AMB = 65.0/64.8/21.0 °C, no faults,
`schema_ver=1`, frame #42 since boot, 1,234,567 ms uptime, boot session 5,
sample captured at 1,234,567 ms:

```
Envelope (13 B):
01 01 01 05 2A 00 87 D6 12 00 00 05 16
          ^--^ ^---------^  ^  ^  ^
          seq  node_clk_ms  |  |  payload_len=22
          ^  ^              |  boot_session
          |  flags=0x00 (not sleepy, no fragments)
          msg_type=0x05 (TELEMETRY_BATCH)

Batch header (11 B):
87 D6 12 00 87 D6 12 00 01 00 01
^---------^ ^---------^ ^--^ ^
start_ms    end_ms      count schema_ver=1
(= capture_ms of first sample)

Sample, bbu_telemetry_v1_t (11 B):
00 01 01 8A 02 88 02 D2 00 00 01
^  ^  ^  ^---^ ^---^ ^---^ ^  ^
|  |  |  t_tpo t_tpu t_amb |  schema_ver=1
|  |  ct_state=RUNNING     fault_flags=0
|  relay_state=ON
mode=AUTO

Full frame (35 B):
01 01 01 05 2A 00 87 D6 12 00 00 05 16 87 D6 12 00 87 D6 12 00 01 00 01
00 01 01 8A 02 88 02 D2 00 00 01
```

Temperature fields are ×10 fixed-point (`0x028A` = 650 → 65.0 °C) to avoid
floats on the wire while keeping one decimal place. During an outage drain
the same frame shape carries `count` > 1 with `start_ms` < `end_ms`;
`boot_session` in the envelope lets the backend stitch a batch that spans
a mid-outage node reboot.

## Liveness model

Two node profiles, declared in `HELLO` (`LIVENESS_MODE` + `LIVENESS_PARAM`):

- **`ALWAYS_ON`** — sends `HEARTBEAT` at a fixed interval, target ~2 s.
  (`TELEMETRY_BATCH` frames also count as liveness evidence.) Gateway
  flags the node unreachable after N missed intervals (N=3 → ~6 s).
- **`PERIODIC_WAKE`** — wakes, registers via `HELLO`, drains its FIFO,
  sleeps. Target wake cadence ~15 s. The gateway must *not* apply the
  always-on timeout: "unreachable" for a sleepy node means missing its
  expected wake window plus grace, not missing a 2 s heartbeat. Sleepy
  nodes set `flags.SLEEPY` so the gateway can distinguish their frame
  pattern.

Gateway surfaces per-node reachability (last-seen, expected vs. actual
cadence) upward; presentation is an MQTT/backend concern (DN004), but the
liveness *inputs* come from this schema.

## Gateway reachability (node's perspective)

The above covers the gateway detecting a gone node. The reverse — a node
detecting a gone gateway — is the trigger for local buffering.

- **Unicast traffic** gets MAC-level delivery feedback for free: ESP-NOW's
  send callback reports whether the peer's radio layer acknowledged the
  frame. Policy: after **N=3 consecutive failed unicast sends**, the node
  declares the gateway unreachable and stops transmitting (FIFO
  accumulation continues per the Unified telemetry FIFO section).
- **`HELLO` is broadcast and gets no MAC-layer ack** — a node that just
  booted or woke has no signal that its registration landed. `HELLO_ACK`
  (gateway → node, unicast) closes this: sent once per boot/wake, doubling
  as the carrier for an immediate `TIME_SYNC` and any rejection reason
  (e.g. role is `RETIRED`). If no `HELLO_ACK` arrives after a couple of
  retries, the node treats the gateway as unreachable for that session —
  for a sleepy node, that means buffer this cycle's sample and go back to
  sleep rather than waiting around.
- Losing the gateway mid-session (channel change, gateway reboot) and
  re-finding it are covered in Gateway discovery — the same N-consecutive-
  failures trip-wire triggers the channel-scan fallback.

## Gateway discovery

ESP-NOW broadcast frames (`ff:ff:ff:ff:ff:ff`) are received by any
ESP-NOW-enabled device on the same WiFi channel, with no pre-existing peer
entry required — so a node's broadcast `HELLO` can reach a gateway it has
never talked to before. The real constraint is **channel, not
addressing**: ESP-NOW rides the radio's current WiFi channel, and the
gateway's channel is dictated by whatever AP it associates to (and can
change — AP reboot, auto channel selection, DFS event).

- **Discovery is a channel scan.** A node with no cached binding — or one
  that just declared the gateway unreachable — broadcasts `HELLO` and
  briefly listens for `HELLO_ACK` on channels 1, 6, and 11 first (covers
  the overwhelming majority of home APs), falling through to a scan of all
  13 channels if none of those answer.
- **Cache the last-known-good channel** on successful bind, and try it
  first on the next boot/wake — the full priority scan is the fallback
  path, not the common case, keeping this cheap for sleepy nodes.
- **First responder wins** if more than one gateway answers (multiple
  gateways in range). No RSSI comparison in phase 1.
- **Gateway channel change mid-session:** the gateway reconciles its own
  channel first (its AP association takes priority). Nodes see this as the
  gateway going unreachable (per the unicast-failure trip-wire), fall back
  into channel-scan discovery, and accumulate telemetry locally for the
  gap exactly as in any other outage — no special case beyond "unreachable
  ⇒ scan."

### Non-blocking radio contract

Scanning, `HELLO` retries, batch sends, retransmits, and all other
ESP-NOW work are **asynchronous to the control loop by construction, not
by hope**. This is the explicit scheduling contract behind the Motivation
promise ("no node's control loop may block on, or degrade unsafely
without, this link"):

- The control loop runs in the highest-priority task and **never calls
  into the radio path synchronously** — it hands telemetry samples to the
  FIFO (an enqueue, no I/O) and nothing else.
- All radio work runs in a dedicated comms task, scheduled strictly below
  the loop and below UI input handling.
- Discovery scanning is windowed and bounded: per-channel dwell (start:
  ~250 ms), a total budget per scan attempt (start: ~5 s across the
  priority list), and a rest interval between attempts (start: ≥ 10 s)
  while searching. A gateway-less node is *always searching* on this duty
  cycle, never saturating the radio.
- **Invariant: sample/act cadence and UI responsiveness are bit-identical
  whether or not a scan or drain is in progress.** If bench measurement
  shows loop jitter attributable to comms, the comms budget shrinks until
  it doesn't.

## Gateway ESP-NOW gating (backend availability)

Buffering during an outage is a **node-side responsibility only** — the
gateway does not accumulate telemetry on nodes' behalf. The gateway's
ESP-NOW interface is only up when both its WiFi station is associated
*and* the backend (MQTT broker / Influx) is reachable; if either drops,
ESP-NOW is torn down entirely — no `HELLO_ACK`, no peer table, no unicast
acks, no `BATCH_ACK`.

- From a node's perspective this is indistinguishable from any other
  gateway-unreachable case: the N-consecutive-failures ⇒ buffer-locally
  and rescan logic apply unchanged. **No new node-side behavior is
  required for gateway/backend outages.**
- Gateway storage/complexity stays bounded regardless of node count: it
  either relays live data immediately or relays nothing and lets each node
  hold its own backlog — never both, never accumulating N nodes' worth of
  undelivered data itself.
- **Convenient side effect: this collapses the time-readiness question.**
  Since ESP-NOW only comes up once the backend path is confirmed live, an
  epoch is available by the time any node's `HELLO` could plausibly be
  answered — there is no separate "time-ready" state.
- On backend recovery: gateway re-enables ESP-NOW, nodes rescan or
  re-`HELLO` exactly as in any other reconnect, and backlogs drain via
  the existing `TELEMETRY_BATCH`/`BATCH_ACK` flow. No special-cased
  "resume" logic.

The health-gating implementation (what counts as "backend reachable",
write-verification) is DN004 scope.

## Registration (`HELLO`)

Sent broadcast on boot and on wake (for sleepy nodes), so the gateway can
build its peer table without any hardcoded MACs:

```
tag=NODE_TYPE,      value=<node_type>
tag=NODE_ID,        value=<node_id>
tag=LIVENESS_MODE,  value=<ALWAYS_ON | PERIODIC_WAKE>
tag=LIVENESS_PARAM, value=<heartbeat_interval_s | wake_interval_s>
tag=SCHEMA_VER,     value=<telemetry schema_ver this node speaks>
tag=CONFIG_VER,     value=<config_ver of this node's parameter table>
tag=WAKE_REASON,    value=<POWER_ON | WAKE_FROM_SLEEP | WATCHDOG_RESET>
```

Source MAC is available from the ESP-NOW receive callback — the gateway
keys its peer table on MAC and maps to `(node_type, node_id)` from here.
Provisioning `node_id` deliberately (set once via admin panel or burned in
at flash time) avoids silent collisions if a board is ever swapped;
friendly names are a gateway/backend-layer concern, not this schema's.

`WAKE_REASON = POWER_ON` combined with a fresh `boot_session` is the
gateway's cue that any prior assumptions about this node's clock offset
are void — see Time.

**`CONFIG_VER` in HELLO is the descriptor's advertisement, not the
descriptor itself** (deliberate — see Consolidation decisions): the
gateway compares against its cached registry entry; on unknown or changed
version it issues `CONFIG_GET` → `CONFIG_DESC` (unicast, where retries and
fragmentation work). HELLO stays small and unfragmented.

## Node lifecycle (retirement, leave, replacement)

Identity is `(node_type, node_id)` — a persistent **role slot**,
provisioned deliberately, that carries history forward for as long as it
exists. MAC is just the **binding**: which physical device currently
answers for that role. This separation makes the scenarios below
tractable instead of one-off special cases.

Each role slot carries an admin-set **status** (gateway/backend-side
state, not a wire message):

| Status | Meaning | Rebind allowed? | Alerting |
|---|---|---|---|
| `ACTIVE` | Normal operation | Yes | Full liveness alerting per Liveness model |
| `ON_LEAVE` | Known-absent (rework, upgrade in progress) | Yes | Suppressed — not "unreachable" |
| `RETIRED` | Permanently done | No | None; `HELLO` claiming this role is always an anomaly |

Scenarios:

- **Board fully retired, reflashed into an unrelated new role** — no
  reconciliation needed. It's provisioned with a fresh `(node_type,
  node_id)` at flash time. The old role slot stays `RETIRED` and
  untouched; its history is frozen on purpose. The hardware starts a new
  life under a new identity.
- **Node down for rework, returns as the *same* role** (e.g. weather
  station gains a solar-insolation sensor) — admin sets the role to
  `ON_LEAVE` while it's gone, suppressing unreachable alerts without
  discarding identity or history. On return, `HELLO` re-activates it; a
  `CONFIG_DESC` version bump signals new capabilities to re-fetch. No
  identity ambiguity.
- **Hardware swap, same role, new MAC** (v1 → v2 board, or any physical
  replacement) — the new board is deliberately provisioned with the
  *same* `(node_type, node_id)`. Its `HELLO` shows a MAC that doesn't
  match what's on file. Since the role is `ACTIVE`/`ON_LEAVE` (not
  `RETIRED`), the gateway is allowed to rebind — logged as an `EVENT`
  ("role rebound to new MAC"), never silent. History continues under the
  same role id; `schema_ver`/`config_ver` differences are handled exactly
  as in the rev1→rev2 telemetry case.

**Hazard — duplicate identity.** If the *old* MAC for an
`ACTIVE`/`ON_LEAVE` role is still transmitting at the same moment a
*different* MAC claims the same `(node_type, node_id)`, that is not a
graceful replacement — it's two live devices sharing an identity
(misconfiguration, or a cloned/reflashed board). This must raise a loud
alert, never a silent merge. A rebind is only quietly logged when the
previous MAC has gone quiet first.

A `RETIRED` role never accepts a rebind. Any `HELLO` claiming a retired
`(node_type, node_id)` is surfaced for admin review, never auto-resolved —
most likely someone forgot to reflash a board before reusing it.

Optional for a single-admin setup: an explicit admin action to
**pre-announce an expected rebind** ("role X is about to move to a new
board") before physically swapping hardware, so a rebind reads in the
logs as "gateway did what I told it to expect" rather than "gateway
silently accepted a rebind."

## Time

The gateway is the only time authority on the network. Nodes have no
battery-backed RTC.

**Node-side truth is exactly `{boot_session, node_clock_ms}`** — there is
no epoch on the node. `node_clock_ms` is *always* ms-since-boot; it is
never re-based on sync. It wraps at 2³² ms ≈ 49.6 days; per-boot-session
translation must do modular arithmetic (epoch = sync + (now − sync_clock)
mod 2³²). This is a non-issue in practice since the RAM FIFO (and any
per-boot bookkeeping) dies with the boot session.

- **`TIME_SYNC`** (gateway → node) carries the epoch at the *sync instant*:

```c
typedef struct __attribute__((packed)) {
    uint32_t epoch_s;    // seconds since Unix epoch at the sync instant
    uint16_t epoch_ms;   // 0–999 sub-second
} time_sync_t;           // 6 bytes, little-endian
```

- The node stores `{epoch_s, epoch_ms, node_clock_ms_at_sync}`. Reported
  time = sync epoch + (current `node_clock_ms` − `node_clock_ms_at_sync`),
  mod 2³². The gateway (and only the gateway) converts this to wall-clock
  for MQTT/Influx.
- **Delivery:** every `HELLO_ACK` carries a fresh `TIME_SYNC` value as a
  TLV — this covers boot, wake, and post-outage re-contact, which is
  exactly when drift matters most. Additionally, ALWAYS_ON nodes receive a
  standalone `TIME_SYNC` at 1×/hour.
- **`boot_session`** (envelope byte 11): chosen fresh at boot (random or
  incrementing), changes only on reboot. It lets the gateway/backend
  correctly stitch telemetry across a mid-outage node reboot instead of
  misreading the clock jumping backward as bad data.
- Outage-duration estimation and timestamp stitching are gateway/backend
  concerns (DN004); this schema exposes `boot_session` + local clock
  honestly enough for that reconciliation to be possible.

### Pre-gateway provisioning (epoch-less bring-up)

A node provisioned and powered before any gateway exists (phase-1 field
reality: first node in the plant, gateway built later) runs with **no
epoch at all**. This is defined behavior, not an error:

- Before the first `TIME_SYNC`, the node's effective epoch is **0**:
  reported time is simply `node_clock_ms`. The envelope is honest;
  there is nothing to fake.
- Backlog captured pre-sync is **not stale**. Once an anchor exists
  (first `HELLO_ACK`), the same translation —
  `anchor_epoch + (capture_ms − anchor_clock)` — extrapolates *backwards*
  through the entire buffer: samples taken hours before the gateway first
  answered get correct wall-clock stamps. No special node-side handling;
  the FIFO never needed an epoch.
- Therefore the gateway and commit service **must not discard points for
  being "too old"** (e.g. 1970-based). Back-dated Influx writes are normal
  here; `boot_session` scopes them.
- Multi-boot pre-gateway history is lost by design (RAM FIFO, Non-goals).
  Within a single boot the full backlog survives the gateway's late
  arrival — making first-node bring-up a natural field test of the
  decimation policy and a heavy stop-and-wait drain.

### Sync interval

Starting value: **1×/hour** for ALWAYS_ON nodes (sleepy nodes re-sync via
every `HELLO_ACK` anyway), tunable — adjust from bench measurement.

Rationale: ESP32/C3-class main crystals are typically ±10–20 ppm
(1 ppm = 1 µs/s), giving ~72 ms/hour of drift at 20 ppm — comfortably
inside a <1 s accuracy target with 5–13× margin, on top of the sub-second
quantization of the sync instant itself. This is a sawtooth error: best
right after a sync, worst right before the next.

Caveat: this assumes `node_clock_ms` derives from the main crystal
(`esp_timer`/APB). `PERIODIC_WAKE` nodes' sleep intervals may instead be
timed by the internal RC oscillator, which is far less stable — largely
moot here since those nodes re-sync every ~15 s wake, but worth knowing if
wake cadence ever wanders more than the crystal-drift math predicts.

## Telemetry

All telemetry is `TELEMETRY_BATCH`: a batch header followed by N samples
of one `(node_type, schema_ver)` fixed struct — hand-packed, since this is
the highest-frequency traffic and efficiency matters here more than
self-description.

### Batch header

```c
typedef struct __attribute__((packed)) {
    uint32_t start_ms;    // node_clock_ms of first sample in this frame
    uint32_t end_ms;      // node_clock_ms of last sample in this frame
    uint16_t count;       // samples present; may be < (end-start+1) if decimated
    uint8_t  schema_ver;  // how to parse the structs that follow
} telemetry_batch_hdr_t; // 11 B — boot_session comes from the envelope
```

`start_ms`/`end_ms` are the **declared span**: samples missing inside it
were decimated by node policy, not lost in transit. Inter-frame loss is
impossible to confuse with decimation because of stop-and-wait (see
Unified telemetry FIFO): the gateway commits whole frames in order, and a
frame is never acked unless its entire declared span was durably written.

### Frame capacity

ESP-NOW application payload ceiling ~250 B. Per frame:
13 B envelope + 11 B header = 24 B overhead, leaving
`(250 − 24) / sizeof(sample)` samples. For BBU v1 (12 B): **18 samples**.
State capacity as this formula — it shrinks if a future schema fattens the
struct, and constants must be derived, not hardcoded.

### BBU, `schema_ver = 1`

```c
typedef struct __attribute__((packed)) {
    uint8_t  mode;          // 0=AUTO 1=MANUAL 2=TEST 3=OFF
    uint8_t  relay_state;   // 0/1
    uint8_t  ct_state;      // 0=OFF 1=RUNNING 2=NO_CURRENT_WARN
    int16_t  t_tpo_x10;     // °C ×10 — sentinel on fault, see fault_flags
    int16_t  t_tpu_x10;
    int16_t  t_amb_x10;
    uint8_t  fault_flags;   // bit/sensor: 0=TPO_OPEN 1=TPO_SHORT 2=TPU_OPEN
                            //             3=TPU_SHORT 4=AMB_OPEN 5=AMB_SHORT
    uint8_t  schema_ver;    // = 1
} bbu_telemetry_v1_t;       // 12 bytes
```

`schema_ver` is independent of `proto_ver` — the envelope format can stay
stable while a given node type's telemetry struct evolves. `fault_flags`
bit numbering is canonical (shared header) and is reused as the `fault_id`
value in `FAULT_RAISED`/`FAULT_CLEARED` events, so fault naming never
forks between telemetry and events.

### BBU, `schema_ver = 2` (rev 2 hardware, real current sensing)

Adds a current field (e.g. `int16_t current_ma`) to the above. The gateway
dispatches on `(node_type, schema_ver)` — old and new BBU boards coexist
on the network during a hardware transition without either side's parser
breaking.

## Unified telemetry FIFO (node side)

The heart of the backlog design. There is **one** telemetry path; "live"
is simply FIFO depth 1.

### Invariants

1. Every sample enters the node's local FIFO **at capture time**, before
   any transmission decision. A node never transmits a sample that is not
   currently held in its FIFO.
2. The node has **at most one outstanding `TELEMETRY_BATCH` frame**
   (stop-and-wait). It sends the oldest unacked entries, waits for
   `BATCH_ACK`, then advances.
3. The node trims only on ack: on `BATCH_ACK(capture_ms W)`, remove all
   FIFO entries with `capture_ms ≤ W` (same boot session).
4. The FIFO is RAM-only and bounded (see Sizing). A node reboot loses the
   FIFO — accepted (Non-goals).

### Normal operation (depth 1)

Sample every 15 s (or whatever cadence). At capture the FIFO typically
holds exactly one entry; the node immediately packages it as a batch of
one and transmits. The worked example above *is* the normal-case frame.

### Outage

The gateway is declared unreachable (Liveness / Gateway reachability).
Transmission stops; new samples append to the tail. No special "recovery
mode" flag exists — recovery is simply "the queue is non-empty":

- outage → queue grows
- reconnect → queue drains oldest-first
- fully drained → automatically back to depth-1 real-time

No transitions to manage; the state machine is self-healing.

### Stop-and-wait acknowledgment

- Node builds a batch frame from the oldest unacked entries (up to frame
  capacity) and transmits.
- **Gateway:** on receipt, durably writes the whole frame to the backend,
  then replies `BATCH_ACK` whose value is the frame's last sample
  `capture_ms` (the watermark). If the durable write fails, it sends
  nothing — absence of ack is the failure signal.
- **Node:** on ack, trims all entries ≤ watermark and (if entries remain)
  immediately builds and sends the next frame. If the send callback
  reports failure, or no ack arrives within a short timeout (starting
  value **2 s**, tunable), the node retransmits **the same entries**.
- **Duplicates are safe.** A lost *ack* causes retransmission of already-
  written data; InfluxDB writes are idempotent on
  (measurement, tags, timestamp), so the re-written samples simply
  overwrite themselves. The gateway may additionally skip samples at or
  below its committed watermark as an optimization, but correctness does
  not depend on it.
- **Why not pipelining / contiguous-seq acks:** a sliding window with
  "highest contiguous sequence" acks (an earlier draft) deadlocks against
  decimation gaps (the gateway cannot distinguish "decimated" from "frame
  lost") and stalls on lost non-telemetry frames sharing the `seq`
  space. At Bracino's scale, stop-and-wait costs nothing: worst case is a
  12 h backlog ≈ 7,200 samples ≈ ~400 frames at 18 samples/frame; ESP-NOW
  round trips are milliseconds, so a full drain completes in well under a
  minute. During drain, dashboards may briefly lag the newest sample by
  the drain time (seconds, typically); this is accepted — live data is
  non-critical for a heating tank.

### Sizing

The FIFO ring is a **per-node-type config constant** (battery/sleepy
future nodes should not inherit a mains node's budget):

```
capacity_samples = ring_bytes / sizeof(sample)
node-bbu phase 1: ring_bytes = 90 KB → 7,680 samples ≈ 32 h at 15 s cadence
```

State the formula in code, not a bare constant — the sample size changes
with `schema_ver`, and the capacity claim should follow automatically.

### Decimation (memory-pressure fallback)

When occupancy ≥ ⅞ of ring capacity, run a decimation pass: **drop every
other entry in the oldest half** of the FIFO. Repeat as needed. This
halves old-data resolution and frees half the ring, giving log-amortized
reach far beyond the raw 32 h while keeping recent data dense.

- Decimation only ever touches **unacked** entries. During an outage
  everything is unacked; once draining, acks advance faster than capture,
  so no conflict with the trim invariant ever arises in practice.
- Decimated samples create gaps *inside* a frame's declared
  `[start_ms, end_ms]` span (count < span). That is expected and honest:
  the gateway writes what it receives and never tries to "close" internal
  gaps.
- This is a deliberate, lossy emergency valve, subordinate to the trim
  invariant: the invariant governs normal trimming; decimation is the
  bounded-memory fallback when an outage outlasts the ring.

### Sleepy-node drain

A `PERIODIC_WAKE` node drains up to K frames per wake window (start value:
as many as fit in ~2 s of wake time) before sleeping again. A 12 h outage
buffer (~2,880 samples at 4/min) takes a handful of wake cycles to drain
at 15 s wake cadence — acceptable for monitoring data; tune K from the
bench.

## Parameters (admin-panel parity)

**One setter path.** Every settable value (mode, threshold, hysteresis,
manual relay override, …) has exactly one validated setter in node
firmware. The local encoder UI and the `PARAM_SET` handler both call it —
nothing arriving over ESP-NOW can put the node into a state the local UI
couldn't also put it into.

This falls out of a parameter table per node, keyed by `param_id`
(canonical registry, shared with `node_type` in `espnow_schema.h`):

```
tag=PARAM_DESCRIPTOR, value = { param_id, type_enum, flags(RO/RW),
                                  min, max, step, name_str }
```

`CONFIG_DESC` serializes this table so the gateway/admin panel can
*discover* what a node exposes rather than hardcoding per-node-type UI.
Mode-switching, threshold changes, manual overrides — all become
`PARAM_SET(param_id, value)`, not bespoke message types.

**Descriptor lifecycle:** the node advertises `config_ver` in every
`HELLO`. The gateway caches the descriptor in its registry and re-fetches
(`CONFIG_GET` → `CONFIG_DESC`) only when the version is unknown or
changed. A node that changes its own parameter table (e.g. after an OTA
adds a tunable) emits `EVENT CONFIG_CHANGED {config_ver}` to force the
re-fetch. The descriptor is never carried in envelopes.

```
PARAM_GET  { param_id }
PARAM_SET  { param_id, value, admin_seq, ttl_s }
PARAM_ACK  { param_id, result, prev_value, new_value }
           result ∈ { OK, REJECTED_RANGE, REJECTED_TYPE, EXPIRED }
```

- **`admin_seq` + `ttl_s`** (default 60 s): guards against a
  delayed/retried `PARAM_SET` double-applying or firing long after it was
  issued — e.g. a "pump off" sent during a node outage shouldn't fire
  unexpectedly the moment the node finally reconnects. Node rejects
  (`EXPIRED`) anything past its TTL. With a single admin user, races
  between local and remote sets are extremely unlikely, but the guard is
  cheap and worth having regardless.
- **Local changes propagate as `EVENT`.** If a parameter is changed at the
  physical encoder/TFT, the node emits `EVENT PARAM_CHANGED` immediately
  (see below) so the admin panel's view of node state never silently goes
  stale.

## EVENT (async notifications)

The EVENT payload is a single TLV record whose tag comes from the **event
registry** below (separate namespace from the field-tag registry — it
names *what happened*, not *which field*). Append-only, frozen once
assigned, lives in `espnow_schema.h`.

| Event id | Name | Value encoding | Purpose |
|---|---|---|---|
| 0x00 | — | — | reserved / never transmit |
| 0x01 | `FAULT_RAISED` | `fault_id u8` (bit position in `fault_flags`) | Sensor fault asserted — same canonical ids as telemetry `fault_flags` |
| 0x02 | `FAULT_CLEARED` | `fault_id u8` | Fault deasserted |
| 0x03 | `PARAM_CHANGED` | `param_id u8, new_value[], source u8` (1=LOCAL_UI, 2=PARAM_SET echo) | Keeps admin panel honest on local encoder changes |
| 0x04 | `CONFIG_CHANGED` | `config_ver u8` | Descriptor changed → gateway should re-`CONFIG_GET` |
| 0x05 | `BATTERY_WARN` | `level_pct u8` | Future sleepy nodes; reserved now |
| 0x06 | `DECIMATION_OCCURRED` | `ring_pct u8, dropped u16` | Diagnostic: a decimation pass fired — evidence for buffer-sizing review |
| 0x07–0xFF | — | — | unassigned, append-only |

The gateway maps event ids to MQTT event-topic payloads (DN004); the
mapping is a gateway concern, the registry here is the contract.

## Future extension: multi-hop relay

Not needed today (all nodes so far are in direct range of a gateway), but
the door stays open cheaply:

- Envelope `flags` bit 2 is earmarked for a future `RELAYED` flag — no
  envelope version bump needed to add it later.
- When designed, a relayed frame would need a small `hop_count`/`max_hops`
  guard against loops, and would preserve the *original* sender's
  `(node_type, node_id, seq)` inside the payload — a relay node is a dumb
  forwarder, it doesn't need to understand what it's carrying, and the
  gateway processes a relayed frame identically to a direct one.

**Explicitly deferred, not designed:** relay election/advertisement, what
triggers a node to fall back to relay mode, and relay-failure behavior.
Speccing this without a concrete out-of-range node in hand would be
guessing; the reserved envelope bits are what make adding it later a
schema extension rather than a schema rewrite.

---

## Open follow-ups (not blocking this schema)

- **Gateway peer-table persistence** (surviving a gateway reboot without
  every node re-registering) — gateway firmware, DN004. Note that
  HELLO-on-boot repopulation makes persistence a convenience, not a
  requirement.
- **DN004 reconciliation:** DN004's drafts carry the older "highest
  contiguous sequence" ack concept; its final text must adopt this note's
  stop-and-wait / capture-watermark design instead. This note is the wire
  law.
- **Fragmentation protocol details** for `CONFIG_DESC` (fragment
  numbering, reassembly window, inter-fragment timeout) — needs
  specifying at implementation time, before the first node type with a
  large parameter table ships. The `MORE_FRAGMENTS` bit is reserved; the
  reassembly rules are not yet written.
- Precise cross-node time alignment — revisit only if a use case needs it.
- OTA transport — later; `proto_ver`/`schema_ver`/`config_ver` here are
  what make mixed-firmware periods survivable when it lands.
- Transport security — accepted low risk for phase 1 given site isolation
  and RF-shielded boiler room; revisit if the threat model changes.
- Multi-hop relay — reserved, not designed; see above.
- Bench-verify: actual ESP-NOW payload ceiling against the IDF version in
  use (frame-capacity constants depend on it); actual crystal drift on
  node hardware (adjust `TIME_SYNC` interval from the 1×/hour starting
  value if warranted); stop-and-wait drain throughput with several nodes
  draining simultaneously; control-loop jitter with discovery scans and
  a drain in progress (Non-blocking radio contract invariant).