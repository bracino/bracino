/*
 * espnow_schema.h — Bracino node ↔ gateway wire law (DESIGN_NOTE_003).
 *
 * Canonical shared registry: envelope, message types, TLV tags, event ids,
 * node types, BBU telemetry struct, BBU param ids. Included by BOTH node
 * and gateway firmware — never fork this file; add, don't renumber.
 *
 * Layouts are packed little-endian (matches ESP32/Xtensa native order).
 * Raw source of truth: docs/DESIGN_NOTE_003_espnow_node_schema.md
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- protocol versions ---- */
#define ESPNOW_PROTO_VER   1u   /* envelope format version */
#define BBU_SCHEMA_VER     1u   /* BBU telemetry struct version */
#define BBU_CONFIG_VER     3u   /* BBU parameter descriptor table version */

/* ESP-NOW application payload ceiling (bench-verify against IDF! DN003). */
#define ESPNOW_MAX_PAYLOAD 250u

/* Envelope: 13 B fixed header + payload. */
#define ESPNOW_ENV_SIZE    13u
/* Batch frame: envelope + batch header, before samples. */
#define ESPNOW_BATCH_OVH   (ESPNOW_ENV_SIZE + 11u)
/* BBU v1 samples per frame at the 250 B ceiling. */
#define BBU_FRAME_SAMPLES  ((ESPNOW_MAX_PAYLOAD - ESPNOW_BATCH_OVH) / \
                            sizeof(bbu_telemetry_v1_t)) /* = 18 */

/* ---- envelope (DN003, fixed for every message) ---- */
typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;
    uint8_t  node_type;
    uint8_t  node_id;
    uint8_t  msg_type;
    uint16_t seq;           /* frame counter, +1 per envelope, wraps 2^16 */
    uint32_t node_clock_ms; /* ALWAYS ms since boot; wraps ~49.6 d */
    uint8_t  flags;         /* bit 0 MORE_FRAGMENTS, bit 1 SLEEPY */
    uint8_t  boot_session;  /* changes only on node reboot */
    uint8_t  payload_len;
    uint8_t  payload[];
} espnow_envelope_t;

#define ESPNOW_FLAG_MORE_FRAGMENTS 0x01
#define ESPNOW_FLAG_SLEEPY         0x02

/* ---- node_type registry (append-only) ---- */
#define NODE_TYPE_BBU        1u
#define NODE_TYPE_ACS        2u
#define NODE_TYPE_BOILER_MON 3u
#define NODE_TYPE_HEAT_LOADS 4u
#define NODE_TYPE_WEATHER    5u

/* ---- message types (frozen once assigned; append-only) ---- */
typedef enum {
    MSG_INVALID         = 0x00,
    MSG_HELLO           = 0x01, /* node → gw (broadcast), TLV */
    MSG_HELLO_ACK       = 0x02, /* gw → node (unicast), TLV + TIME_SYNC */
    MSG_HEARTBEAT       = 0x03, /* node → gw, envelope only */
    MSG_TIME_SYNC       = 0x04, /* gw → node, time_sync_t (6 B) */
    MSG_TELEMETRY_BATCH = 0x05, /* node → gw, hdr + N samples */
    MSG_BATCH_ACK       = 0x06, /* gw → node, u32 LE capture_ms watermark */
    MSG_EVENT           = 0x07, /* node → gw, TLV (event registry) */
    MSG_CONFIG_GET      = 0x08, /* gw → node */
    MSG_CONFIG_DESC     = 0x09, /* node → gw, TLV, may fragment */
    MSG_PARAM_GET       = 0x0A, /* gw → node, TLV {PARAM_ID} */
    MSG_PARAM_SET       = 0x0B, /* gw → node, TLV {id, value, seq, ttl} */
    MSG_PARAM_ACK       = 0x0C, /* node → gw, TLV {id, result, prev, new} */
} msg_type_t;

/* ---- TLV tag registry (one namespace, append-only) ---- */
typedef enum {
    TLV_INVALID           = 0x00,
    TLV_NODE_TYPE         = 0x01, /* u8 */
    TLV_NODE_ID           = 0x02, /* u8 */
    TLV_LIVENESS_MODE     = 0x03, /* u8: 1=ALWAYS_ON 2=PERIODIC_WAKE */
    TLV_LIVENESS_PARAM    = 0x04, /* u16 LE, seconds */
    TLV_SCHEMA_VER        = 0x05, /* u8 */
    TLV_CONFIG_VER        = 0x06, /* u8 */
    TLV_WAKE_REASON       = 0x07, /* u8 */
    TLV_TIME_SYNC         = 0x08, /* time_sync_t, 6 B */
    TLV_REJECT_REASON     = 0x09, /* u8 */
    TLV_PARAM_ID          = 0x0A, /* u8 */
    TLV_PARAM_VALUE       = 0x0B, /* n B (type from descriptor) */
    TLV_PARAM_DESCRIPTOR  = 0x0C, /* n B, see param descriptor */
    TLV_ADMIN_SEQ         = 0x0D, /* u32 LE */
    TLV_TTL_S             = 0x0E, /* u8 */
    TLV_PARAM_RESULT      = 0x0F, /* u8 */
    TLV_PREV_VALUE        = 0x10, /* n B */
    TLV_NEW_VALUE         = 0x11, /* n B */
} tlv_tag_t;

/* ---- liveness / wake registries ---- */
#define LIVENESS_ALWAYS_ON    1u
#define LIVENESS_PERIODIC_WAKE 2u

#define WAKE_POWER_ON          1u
#define WAKE_FROM_SLEEP        2u
#define WAKE_WATCHDOG_RESET    3u

/* ---- time sync: epoch at the sync instant (gateway is time authority) ---- */
typedef struct __attribute__((packed)) {
    uint32_t epoch_s;   /* seconds since Unix epoch at sync instant */
    uint16_t epoch_ms;  /* 0–999 sub-second */
} time_sync_t;          /* 6 B */

/* ---- telemetry batch (header + N samples of one schema) ---- */
typedef struct __attribute__((packed)) {
    uint32_t start_ms;   /* node_clock_ms of first sample (declared span) */
    uint32_t end_ms;     /* node_clock_ms of last sample */
    uint16_t count;      /* may be < span when decimated — that is the record */
    uint8_t  schema_ver;
} telemetry_batch_hdr_t; /* 11 B */

/* BBU telemetry, schema_ver = 1 (12 B). Temps are °C ×10. */
/* BBU ct_state values (bbu_telemetry_v1_t). 3 = CT not fitted / A0 ignored —
 * the plant drives a hidden contactor coil through the node relay (issue 014),
 * so pump current never crosses node wiring; see DESIGN_NOTE_001. */
#define BBU_CT_STATE_OFF             0u
#define BBU_CT_STATE_RUNNING         1u
#define BBU_CT_STATE_NO_CURRENT_WARN 2u
#define BBU_CT_STATE_NOT_FITTED      3u

typedef struct __attribute__((packed)) {
    uint8_t  mode;        /* bbu_mode wire encoding, see BBU_MODE_W_* */
    uint8_t  relay_state; /* 0/1 */
    uint8_t  ct_state;    /* BBU_CT_STATE_* above */
    int16_t  t_tpo_x10;
    int16_t  t_tpu_x10;
    int16_t  t_amb_x10;
    uint8_t  fault_flags; /* bit per sensor fault, see BBU_FAULT_* */
    uint8_t  schema_ver;  /* = 1 */
    uint8_t  rsv;         /* reserved — transmit 0 (pads to the stated 12 B) */
} bbu_telemetry_v1_t;

/* Wire encoding of BBU modes (order chosen to match user-facing modes;
 * internal bbu_mode_t in control.h maps in node firmware). */
#define BBU_MODE_W_MANUAL 0u
#define BBU_MODE_W_AUTO   1u
#define BBU_MODE_W_TEST   2u
#define BBU_MODE_W_OFF    3u

/* fault_flags bits — canonical fault ids, reused by FAULT_RAISED /
 * FAULT_CLEARED events. Frozen; append only. */
#define BBU_FAULT_TPO_OPEN   0u
#define BBU_FAULT_TPO_SHORT  1u
#define BBU_FAULT_TPU_OPEN   2u
#define BBU_FAULT_TPU_SHORT  3u
#define BBU_FAULT_AMB_OPEN   4u
#define BBU_FAULT_AMB_SHORT  5u
#define BBU_FAULT_COUNT      6u

/* ---- parameter value types (descriptor type_enum) ---- */
#define PTYPE_U32     1u   /* value: u32 LE */
#define PTYPE_I16_X10 2u   /* value: int16 LE, °C ×10 */
#define PTYPE_ENUM    3u   /* value: u8 */

#define PARAM_FLAG_RW 0x01u

/* PARAM_SET admin_seq replay guard: reject non-increasing admin_seq. */
#define PARAM_RESULT_OK              0u
#define PARAM_RESULT_REJECTED_RANGE  1u
#define PARAM_RESULT_REJECTED_TYPE   2u
#define PARAM_RESULT_EXPIRED         3u

/* ---- BBU param_id registry (append-only; ids are never reused) ----
 * One validated setter path: local UI and PARAM_SET both go through it.
 * Revocations leave the id permanently reserved (documented here):
 *   5: ct_confirm_s — REVOKED 2026-09-03 (CT dropped from circuit, issue
 *      014 / DN001 rev 2). Never deployed: no gateway existed when the id
 *      was revoked. Do NOT renumber; do NOT reassign id 5. */
#define BBU_PARAM_TPO_SETPOINT_C     1u  /* I16_X10, 200..900        */
#define BBU_PARAM_HYSTERESIS_C       2u  /* I16_X10, 5..150          */
#define BBU_PARAM_MIN_ON_TIME_S      3u  /* U32, 0..3600             */
#define BBU_PARAM_MIN_OFF_TIME_S     4u  /* U32, 0..3600             */
#define BBU_PARAM_MIN_TPO_TPU_DELTA_C 6u /* I16_X10, 0..300          */
#define BBU_PARAM_MAX_RUN_TIME_MIN   7u  /* U32, 1..240              */
#define BBU_PARAM_USER_MODE          8u  /* ENUM, BBU_MODE_W_*       */
#define BBU_PARAM_MANUAL_RELAY       9u  /* ENUM, 0/1 (Manual mode)  */
#define BBU_PARAM_COMMS_ENABLE       10u /* ENUM, 0/1                */
#define BBU_PARAM_SAMPLE_PERIOD_S    11u /* U32, 5..120, default 15  */
#define BBU_PARAM_COUNT              11u /* registry space; 10 live  */

/* ---- event registry (EVENT payload is ONE TLV: tag = event id) ---- */
#define EVENT_FAULT_RAISED   0x01u /* value: fault_id u8          */
#define EVENT_FAULT_CLEARED  0x02u /* value: fault_id u8          */
#define EVENT_PARAM_CHANGED  0x03u /* value: param_id u8, new_value[],
                                    *       source u8 (1=LOCAL_UI,
                                    *       2=PARAM_SET echo)          */
#define EVENT_CONFIG_CHANGED 0x04u /* value: config_ver u8         */
#define EVENT_BATTERY_WARN   0x05u /* value: level_pct u8 (future) */

#define PARAM_SRC_LOCAL_UI   1u
#define PARAM_SRC_PARAM_SET  2u

/* CONFIG_DESC fragmentation (implementation choice, DN003 open item):
 * payload = { frag_idx u8, frag_total u8, descriptor TLVs... };
 * MORE_FRAGMENTS set on every fragment but the last. Fragments are
 * reassembled in idx order; 2 s inter-fragment timeout. */
#define CONFIG_DESC_FRAG_OVH 2u

#ifdef __cplusplus
}
#endif
