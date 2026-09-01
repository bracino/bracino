# DESIGN_NOTE_004 — Gateway design

**Status:** proposed (consolidated)
**Date:** 2026-08-30
**Scope:** `firmware/gateway` internals — runtime state machine, task
topology, node registry, MQTT contract (topics, payloads, QoS, retention),
command pipeline, maintenance interface. The gateway is an
**ESP-NOW ↔ MQTT supervisory bridge**; this note owns its firmware
architecture and its MQTT-side contract.

Out of scope: the ESP-NOW wire schema and node behavior
([DESIGN_NOTE_003](DESIGN_NOTE_003_espnow_node_schema.md) — **wire law**;
where the notes overlap, DN003 wins), the backend pipeline / commit
service (stubs → [DESIGN_NOTE_005](DESIGN_NOTE_005_backend_pipeline.md)),
the cross-cutting failure-mode matrix
([DESIGN_NOTE_006_data_integrity.md](DESIGN_NOTE_006_data_integrity.md)),
broker/server config (`server/`), gateway board definition (`hardware/`).

Related: issues `004` (MQTT schema — this note closes it), `005`
(ESP-NOW payload — closed by DN003), `006`, `008` (Influx retention).

---

## Consolidation decisions (vs. the two prior drafts)

1. **DN003 is the wire law.** All backlog/ack semantics (stop-and-wait,
   `BATCH_ACK` capture_ms watermark, FIFO, decimation) live in DN003. The
   gateway's entire telemetry duty is: decode → timestamp → publish →
   ack-on-commit. The earlier "highest contiguous sequence" ack concept is
   superseded and must not reappear here.
2. **Registry is split: durable role table + volatile runtime state**
   (replacing draft 1's "RAM only"). Role identity, MAC binding, and
   role status must survive a gateway reboot for the DN003 node-lifecycle
   rules (RETIRED/ON_LEAVE, rebind detection) to hold. Runtime liveness
   stays volatile.
3. **`BATCH_ACK` waits for backend commit.** A commit service (DN005)
   writes Influx and publishes per-node commit watermarks; the gateway
   acks a drained frame only after the watermark covers it. This makes
   `BATCH_ACK` mean "durable in Influx" without an HTTP client in the
   gateway. (DN005 stub records the requirement.)
4. **The MQTT task owns the gateway state machine**, with asymmetric
   hysteresis: slow to enter ACTIVE, fast to leave. No other task decides
   gateway mode.
5. **Telemetry path = Telegraf-free commit service.** The commit service
   (Python container) subscribes to telemetry topics, writes Influx,
   publishes commit watermarks, runs chain-health evaluation, and
   **alerts the admin on problematic events** (webhook/push — designed in
   DN005, consumed here as a health-gate input). Gateway speaks MQTT
   only — no Influx client on device.
6. **Gateway gets its own presence and event topics** (`bracino/gateway/*`,
   retained status + LWT), and per-node status publication follows the
   republish-on-boot rule below.
7. **Maintenance SoftAP is button-invoked only** — never auto-started —
   and is an **overlay, not a state**: it coexists with ACTIVE (live
   troubleshooting over the AP while ESP-NOW keeps flowing); window
   open/close are gateway events.
8. **Gateway hardware is deliberately thin here** — off-the-shelf parts,
   documented in `hardware/`, not an architectural concern. Button GPIO is
   TBD pending a gateway hardware note; nothing (including the earlier
   GPIO23 sketch, which does not exist on ESP32-C3) is assumed.
9. **Embedded C-string HTTP for the maintenance UI** — no filesystem,
   ~20–30 KB, self-contained, OTA updates the UI automatically.
10. **Mosquitto must run with persistence enabled** so retained statuses
    survive broker restarts. Broker config detail lives in `server/`, the
    requirement lives here and in the DN005 stub.

---

## Architectural responsibilities

The gateway is a **stateless-toward-data, write-through bridge**. It owns:
protocol translation, connection management, node liveness evaluation,
parameter-descriptor caching, and transactional command tracking.

It does **not** own: telemetry backlog (nodes own it — DN003), data
retention (Influx, issue 008), control-loop logic (nodes, DN002), or the
Influx write itself (commit service, DN005). If the downstream path is
broken, the gateway **withdraws from the ESP-NOW network** rather than
accumulate: data ownership returns to the origin nodes.

## Runtime state machine

Three states, one owner (the MQTT task). ESP-NOW is started/stopped in
exactly one place: the ACTIVE entry/exit.

```
  ┌──────────┐
  │   BOOT   │ ──> init hw, NVS, task loops
  └──────────┘
        │
        ▼
┌──────────────┐   any path drops / write fails
│ WAIT_BACKEND │<──────────────────────┐
└──────────────┘                       │
        │  health sustained (N=3)     fast exit
        ▼                             │
  ┌──────────┐                        │
  │  ACTIVE  │ ───────────────────────┘
  └──────────┘
```

1. **BOOT** — peripherals, NVS (role table), task/queue bring-up. No radio.
2. **WAIT_BACKEND** — WiFi STA association, NTP epoch, broker connect
   (with LWT), then wait for sustained chain health (see Health gate).
   ESP-NOW is **disabled**.
3. **ACTIVE** — ESP-NOW enabled; nodes discover/gate as per DN003.
   Any confirmed backend-path failure exits immediately.

### State transitions — entry/exit actions

**WAIT_BACKEND:**

- *Entry:* start WiFi STA with stored credentials; begin health checks
  (WiFi → NTP → broker → commit watermarks).
- *Loop:* retry association/broker; service maintenance AP if invoked;
  read-only diagnostics remain available.
- *Exit conditions:* WiFi associated AND broker connected AND health
  sustained (N=3 consecutive healthy checks).
- *Exit actions:* stop maintenance AP if running; enable ESP-NOW; enter
  ACTIVE.

**ACTIVE → WAIT_BACKEND (any of):** WiFi disassociation, broker
disconnect, commit-watermark staleness (backend not writing), or NTP loss.
*Exit actions:* tear down ESP-NOW entirely — no HELLO_ACKs, no peer
table traffic, no BATCH_ACKs — republish gateway status offline if
graceful. From a node's perspective this is indistinguishable from any
other gateway loss (DN003 handles it; no special cases).

**Hysteresis is asymmetric by design.** Entering ACTIVE is disruptive
(channel lock-in, node HELLO storms, discovery scans); leaving is cheap
(nodes just accumulate into FIFOs). So: **N=3 consecutive healthy checks
to enter ACTIVE; a single confirmed failure to leave.** A flapping broker
must not flap the radio.

### Who owns what

The **MQTT task owns the state machine** — it already owns every network
session (WiFi association state, broker socket, watermark watching). The
Radio and Bridge tasks never decide; they read a shared `gateway_mode`
variable and start/stop ESP-NOW accordingly. One owner, two followers.

## FreeRTOS task topology

Three tasks, communication via queues only — ESP-NOW callbacks stay short,
network blocking never stalls the radio path.

- **Radio task** — ESP-NOW send/recv callbacks copy frames into a queue
  immediately (callback context is precious); transmits frames queued for
  the wire. No parsing in callback context.
- **Bridge task** — owns the volatile runtime registry; decodes envelopes,
  dispatches on `msg_type`, parses TLV, translates timestamps
  (sync-anchor math, below), builds JSON, enqueues MQTT publishes and
  BATCH_ACKs.
- **MQTT task** — all network I/O: WiFi state, broker session,
  subscriptions, publishes, watermark subscription, **and the state
  machine + health gate**.

Frame buffers pass by pointer with single ownership handed across queues
(producer allocates, consumer frees) — no shared-buffer tricks in the
radio path.

## Node registry — split model

Capacity: **32 role slots** (static pool, fixed NVS footprint; no malloc
in the radio path). Well beyond the ~10-node horizon; if ever exhausted,
OTA.

### Persistent role table (NVS)

The DN003 node-lifecycle contract (RETIRED roles never rebind, ON_LEAVE
suppresses alerting, duplicate-identity detection needs the previous MAC)
requires identity state to survive gateway reboots. Volatile-only would
silently downgrade RETIRED roles and forget reference MACs on every
restart.

```c
typedef struct __attribute__((packed)) {
    uint8_t  node_type;
    uint8_t  node_id;          // (node_type, node_id) = role slot key
    uint8_t  mac[6];           // current binding (DN003 lifecycle)
    uint8_t  role_status;      // ACTIVE / ON_LEAVE / RETIRED
    uint8_t  schema_ver;       // last advertised
    uint8_t  config_ver;       // last advertised; invalidates descriptor cache
} nvs_role_record_t;           // ~6 B/role + header; written on change only
```

Written **only on content change** (compare-before-write): a HELLO from an
already-known role with unchanged binding and versions is a RAM-only
no-op, so sleepy nodes HELLOing every ~15 s wake cost zero writes. Actual
writes are rare human-scale events (new role, board swap/rebind,
provisioned status change, version bump). On flash wear: ESP-IDF NVS
wear-levels internally (append-only pages, garbage collection, spread
across the partition), so even pathological write rates amortize over
years — with change-only writes the concern is academic. Reads at boot.

### Volatile runtime record

```c
typedef struct {
    uint8_t  node_type;
    uint8_t  node_id;
    uint8_t  mac[6];
    bool     online;                 // liveness evaluation result
    uint32_t last_seen_ms;           // gateway-uptime ms of last contact with node
    uint8_t  boot_session;           // last seen — anchor validity key
    time_anchor_t anchor;            // last-known TIME_SYNC for this node
    liveness_mode_t mode;            // ALWAYS_ON / PERIODIC_WAKE
    uint16_t liveness_param_s;       // declared heartbeat/wake interval
    uint16_t last_seq;              // gap/replay detection
    // cached latest decoded telemetry + last K events (maintenance page)
} node_record_t;
```

`last_seen_ms` is stamped in the **observer's** clock (gateway uptime) —
it measures age since last contact, not node uptime. The node's own
uptime travels in the envelope (`node_clock_ms`).

**The sync anchor is the translation table for timestamps.** Per node,
`time_anchor_t = {epoch_s, epoch_ms, node_clock_ms_at_sync, boot_session}`
— captured whenever the gateway sends `TIME_SYNC` (every `HELLO_ACK`,
plus 1×/hour standalone). Translation for any arriving frame:
`node_ts = anchor_epoch + (node_clock_ms − anchor_clock)` (mod 2³²);
**if the frame's `boot_session` ≠ anchor's, the anchor is void** — the
gateway does not stamp the frame with stale math, emits a gateway event
(clock anomaly — someone besides the gateway must know), and sends a
fresh `TIME_SYNC` to the node. Sleepy nodes re-anchor every wake
(HELLO_ACK always carries sync); ALWAYS_ON nodes at 1×/hour plus every
HELLO_ACK.

In normal operation the void-anchor path is unreachable: a node that
rebooted sends `HELLO` on boot (DN003), the `HELLO_ACK` refreshes the
anchor before any telemetry flows, and a node whose `HELLO`/`HELLO_ACK`
exchange failed goes to buffering rather than transmitting (DN003). The
path is handled defensively anyway, and it **entangles nothing**: the
drain strategy is untouched — the node never learns of the incident
beyond receiving a sync, and the held frame re-enters the publish path
on the node's existing retransmit timer (live depth-1 batches retransmit
after their 2 s timeout; drain frames likewise, anchor fresh by then).

### Registry persistence behavior at boot

Role table loads from NVS; volatile records start empty. The gateway
republishes the retained per-node `status` set from the role table after
its first liveness evaluation pass (so stale `online=true` from before a
reboot is corrected within one evaluation cycle, not left lying).

## MQTT contract (topics, QoS, retention, payloads)

The boundary everything downstream codes against. Conventions:
**flat JSON** (single level, no nesting — Grafana/Influx friendly);
**enum values as strings** at the MQTT boundary (`"AUTO"`, `"RUNNING"`);
the commit service maps to integer fields on Influx writes (strings for
humans at MQTT, ints for storage — decision recorded). °C as floats.
Timestamps ISO-8601 UTC.

| Topic | Dir | Retained | QoS | Payload |
|---|---|---|---|---|
| `bracino/gateway/status` | GW→ | yes | 1 | `{"online":true,"fw":"…","uptime_s":…}`; LWT = `{"online":false}` |
| `bracino/gateway/event` | GW→ | no | 1 | gateway-generated events (below) |
| `bracino/gateway/commit` | commit svc→ | no | 1 | watermark: `{"node_type":t,"node_id":i,"capture_ms_end":n,"ok":true}` (DN005) |
| `bracino/gateway/health` | commit svc→ | yes | 1 | consolidated chain health (DN005) |
| `bracino/node/<t>/<id>/telemetry` | GW→ | no | 0 | live sample (below) |
| `bracino/node/<t>/<id>/status` | GW→ | yes | 1 | `{"online":true}` / `{"online":false,"reason":…}` |
| `bracino/node/<t>/<id>/event` | GW→ | no | 1 | DN003 EVENT registry rendered as JSON |
| `bracino/node/<t>/<id>/config` | GW→ | yes | 1 | `CONFIG_DESC` rendered as JSON parameter table |
| `bracino/node/<t>/<id>/cmd/set` | →GW | no | 1 | `{param_id, value, admin_seq, ttl_s}` |
| `bracino/node/<t>/<id>/cmd/get` | →GW | no | 1 | `{param_id, admin_seq}` |
| `bracino/node/<t>/<id>/cmd/ack` | GW→ | no | 1 | `{admin_seq, result, prev_value, new_value}` |

Subscription: the gateway subscribes `bracino/node/+/+/cmd/#` (QoS 1).
Telemetry at QoS 0 is deliberate — highest volume, duplicates pointless,
loss harmless (node never trims on live sends; a dropped frame just isn't
acknowledged, and drain frames are covered by BATCH_ACK semantics).

### Telemetry payload

```json
{"mode":"AUTO","relay_state":1,"ct_state":"RUNNING",
 "t_tpo":65.0,"t_tpu":64.8,"t_amb":21.0,"fault_flags":0,
 "boot_session":5,"node_ts":"2026-08-29T20:05:00Z"}
```

`ct_state` maps 1:1 from the DN003 enum (`OFF`, `RUNNING`,
`NO_CURRENT_WARN`, and since 2026-09-01 `NOT_FITTED` — nodes without a CT
in circuit; surface as informational, not a fault).

`boot_session` is deliberately present in the payload: Influx stitching
across a mid-outage node reboot (per role id) needs it as a **tag**, and
it costs one integer. Timestamp for Influx = gateway-computed `node_ts`.

### Gateway-generated events

`bracino/gateway/event` carries what DN003 requires "surfaced for admin
review": role rebind, duplicate-identity alarm, RETIRED-role HELLO claim,
registry-full, health-state transitions, voided time-anchor (clock
anomaly), maintenance-AP window opened/closed. Payload:
`{"what":"rebind","node_type":1,"node_id":2,"detail":…,"gw_ts":…}`.
Unretained QoS 1 — durability comes from the events stream in Influx
(DN006), not from MQTT. Events generated while the broker is
unreachable (e.g. maintenance window opened during WAIT_BACKEND) are
held in a small RAM queue and published on recovery.

## The commit-service contract and BATCH_ACK

**Contract (full design is DN005):** a standalone Python service in the
compose stack subscribes to telemetry/event topics, writes each sample to
Influx, and on durable write publishes `bracino/gateway/commit`
watermarks. The gateway forwards `BATCH_ACK(capture_ms)` for a drained
frame **only after** observing a commit watermark ≥ that frame's
`end_ms` for the sending node. `BATCH_ACK` therefore means, end-to-end,
"durably written to Influx" — DN003's semantics, with no Influx client
(or HTTP stack, or token) on the gateway.

- Watermark staleness (per active node, or globally) is a health-gate
  input: sustained stall ⇒ exit ACTIVE ⇒ nodes retain their data.
- Duplicate drain frames (lost ack, node retransmit) produce duplicate
  Influx writes — idempotent on (measurement, tags, timestamp).
- If the service dies, watermarks stall, the gateway exits ACTIVE, and
  nodes buffer. No node-side changes; the DN003 trip-wire absorbs it.
- **Scope honesty:** this proves the gateway→backend leg only. Node-side
  death is detected by gateway liveness (DN003); node-side gateway-loss
  detection is the DN003 trip-wire. Each chain link owns its detector.

**Why not Telegraf?** Telegraf can do the *ingestion* leg (MQTT consumer
→ Influx output) from a config file. It cannot do the *watermark* leg:
the watermark must be published by the component that performed the
write, and post-write ack correlation per node is not expressible in
Telegraf's config-only pipeline (processors run before output flushes).
Splitting the roles — Telegraf ingests, a helper watermarks — is worse
than either alone: the helper would have to duplicate the consumption
and *assume* Telegraf succeeded, yielding a watermarker that can lie
(Telegraf dead while watermarks advance ⇒ gateway acks unwritten data)
— precisely the failure mode this design exists to prevent. The only
sound alternatives are one component that ingests and watermarks
(chosen; ~300 lines of Python, not a Telegraf rewrite), or
gateway-writes-Influx-directly (rejected: HTTP stack + token on device,
forked write path). Accepting "broker acknowledged" as the commit
signal — which would make Telegraf alone sufficient — is rejected
because an Influx outage would then silently drop acked-and-trimmed
telemetry.

## Commands (transactional pipeline)

- Admin publishes `cmd/set` `{param_id, value, admin_seq, ttl_s}` →
  gateway allocates `admin_seq → reply topic` in a tiny pending table and
  transmits `PARAM_SET` over ESP-NOW.
- On `PARAM_ACK`, gateway publishes `cmd/ack` `{admin_seq, result,
  prev_value, new_value}` and deletes the tracker. If the node rejects
  (`EXPIRED`, `REJECTED_RANGE`, …) the result is relayed verbatim.
- **Timeout = the frame's `ttl_s`** (DN003 default 60 s): expired pending
  entries are dropped with a failure ack. **No persistent queue** — if the
  gateway dies, the command dies. That is the design: supervisory control
  must never queue for later.
- `cmd/get` is supported (response over `cmd/ack` with the value); bulk
  reads come from the retained `…/config` descriptor topic instead of
  round-trips.
- The admin UI is **descriptor-driven** (DN003 "one setter path"): the
  panel renders from retained `…/config`, never hardcodes per-node UI.
- QoS 1 on all command topics; `admin_seq` echo makes ack→request
  matching unambiguous even with broker-level duplicates.

## Gateway presence, node status, and retained-message rules

- **Gateway status:** `bracino/gateway/status` retained; LWT publishes
  `{"online":false}` on ungraceful death. Graceful shutdown publishes the
  same and may zero-length-clear topics it owns. Every maintained
  retained topic must have both a will (crash) and a cleanup path
  (shutdown).
- **Per-node status (the rule):** the gateway publishes retained
  `online`/`offline` per node on liveness evaluation (DN003 timeouts: 3
  missed ~2 s heartbeats for ALWAYS_ON; wake-window-plus-grace for
  SLEEPY). At boot it republishes the full set from the persistent role
  table after the first liveness pass. Consumers treat
  `bracino/gateway/status = offline` (LWT) as authority over all node
  statuses — retained per-node `online` values are only as trustworthy
  as the gateway that published them.
- **Broker persistence must be enabled** (retained statuses survive
  broker restarts); configuration lands in `server/`, requirement noted
  here and in the DN005 stub.
- Retained ≠ logged: retained messages are last-value slots (delivered
  once at subscribe time, overwritten on each publish, cleared by a
  zero-length publish). History and time-based retention live in Influx
  (retention policies — issue 008), not in MQTT.

## Health gate (what must be true for ACTIVE)

Evaluated by the MQTT task; all inputs are session/queue states it
already owns:

| Check | Source | Cadence |
|---|---|---|
| WiFi associated | STA event state | continuous |
| Time valid | NTP sync status — two acceptable sources: a local LAN NTP server (e.g. on the compose host) or internet NTP; **either one suffices** | at association + periodic |
| Broker connected | MQTT keepalive/socket | continuous |
| Backend writing | commit-watermark freshness (`bracino/gateway/health`, per-node watermark age) | periodic |

Entry: N=3 consecutive healthy cycles. Exit: single confirmed failure
(e.g. watermark stalled for its timeout ⇒ Influx/commit-service down ⇒
leave ACTIVE ⇒ ESP-NOW down ⇒ nodes buffer). The gateway holds **no
telemetry backlog** by design — write-through or withdraw, never
accumulate.

## Maintenance interface (SoftAP, button-invoked)

A field-maintenance overlay, never part of the control path:

- **Activation: button only** (single long press, pin TBD pending gateway
  hardware ADR), from any state, 10-minute ephemeral timer, WPA2 with a
  printed/default password. The AP is an **overlay, not a gateway
  state**: opening it does not leave ACTIVE, closing it requires no
  recovery, and open/close are gateway events (held for publishing if
  the broker is unreachable).
- **The AP and ACTIVE coexist — this is the point of the design.** The
  two real field situations are:
  1. **Provisioning** (gateway can't reach the LAN): the gateway is in
     WAIT_BACKEND anyway, where ESP-NOW is already off by the gating
     rule (DN003) — the AP pauses nothing that wasn't already paused.
     When the STA associates, the radio jumps to the router's channel
     and maintenance clients drop once, mid-provisioning; they
     reconnect on the new channel.
  2. **Troubleshooting nodes lacking their own UI**: the gateway is in
     ACTIVE, healthy, ESP-NOW live. The softAP binds to the STA channel
     by construction, so **AP + STA + ESP-NOW coexist and nothing
     pauses** — live node data from the RAM registry is on the pages
     while telemetry keeps flowing to the backend. (If the gateway is
     in WAIT_BACKEND, live ESP-NOW data is infeasible by design — that
     troubleshooting belongs on the node's bench, not over this AP.)
- **Channel coupling, stated as behavior:** softAP and STA share one
  radio channel. During ACTIVE the AP sits on the STA channel by
  construction. In WAIT_BACKEND the AP pins the channel until STA
  association succeeds, then the radio jumps and AP clients are
dropped (case 1 above).
- **UI reads the gateway's RAM registry, not MQTT** — it must work
  precisely when MQTT is down, with no serialize/deserialize round-trip,
  from the freshest data the gateway already holds.
- **Three pages, embedded C strings (~20–30 KB), no filesystem:**
  1. *Network/gateway status* — fw version, SSID/channel/RSSI, broker +
     backend-write status, IP, uptime; button: change WiFi credentials.
  2. *Nodes* — live table from the registry (node, last seen, online),
     latest decoded telemetry per node from RAM.
  3. *Diagnostics* — ESP-NOW channel, gateway MAC, RX/TX counters, seq
     gaps, last-20-events ring per node (RAM, repopulates after boot).
- **Data flow:** ESP-NOW → registry (RAM) → {HTTP pages, MQTT publish}.
  The registry feeds both consumers; the maintenance plane never
  subscribes to the broker it is meant to troubleshoot around.

## Failure handling summary

| Failure | Behavior |
|---|---|
| WiFi loss | WAIT_BACKEND; ESP-NOW down; nodes buffer |
| Broker loss | same |
| Commit-watermark stall (Influx/service down) | same — the gateway never acks what wasn't durably written |
| Gateway reboot | role table persists; volatile registry repopulates via HELLO; node statuses republished after first liveness pass; nodes re-register and re-anchor time via HELLO_ACK |
| Node unreachable | liveness timeout ⇒ retained `/status` offline + gateway event |
| Gateway reboot mid-command | pending-command table is RAM; command dies — correct for supervisory control |

Every row ends the same way: **data retained at the source, heat
unaffected.** The chain-wide version of this table is DN006.

## Implementation notes

- Three tasks + queues as above; queues own memory handoff; no ESP-NOW
  API calls from bridge/MQTT contexts.
- Static allocation for registry and queues; no malloc in radio paths.
- Registry capacity 32 (above).
- State machine + health gate live in the MQTT task; `gateway_mode` is
  the only shared state the other tasks read.
- Module-neutral firmware (C3/WROOM-class parts acceptable); button GPIO
  TBD pending `hardware/` gateway note.

## Open follow-ups

- **DN005 — backend pipeline & commit service** (stub): commit-watermark
  semantics and `bracino/gateway/{commit,health}` contract details;
  Telegraf-free pipeline choice (service writes Influx directly from
  MQTT); Mosquitto persistence enabled; Influx field/tag mapping
  (enums→ints); retention policies (issue 008); notification hooks
  (e.g. ntfy webhook on chain alarms).
- **DN006 — data integrity**: cross-cutting failure-mode matrix (one row
  per failure: detection, latency, data disposition, admin surface),
  chain map, explicit non-coverage list.
- **DN003 touch-up** (one pass, after this note lands): remove the moot
  "stitch across mid-buffer reboot" sentence (RAM FIFO dies on node
  reboot — what survives is anchor invalidation via `boot_session`);
  add cross-reference that the gateway registry persists role table +
  per-node sync anchors/`boot_session`.
- **Issue 004** closes against this note once the topic/payload table
  above is ratified.
- **Gateway hardware note** (button pin, module choice, enclosure) —
  `hardware/` when the bench build happens; not an architectural concern.
- Bench items: ESP-NOW payload ceiling vs IDF version; stop-and-wait
  drain throughput with several nodes draining simultaneously (shared
  with DN003).
