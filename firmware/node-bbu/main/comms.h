/*
 * comms.h — ESP-NOW client per DESIGN_NOTE_003 (issue 011).
 *
 * Compiled into every node image; inert until comms_enabled is set (NVS,
 * serial `comms on`, later the UI menu per 012). Control loop feeds
 * samples via comms_offer_sample() — an enqueue, never I/O (non-blocking
 * radio contract). All radio work lives in the comms task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* One captured telemetry sample (wire struct + node-side capture stamp). */
typedef struct {
    uint8_t  mode_w;      /* wire mode: BBU_MODE_W_* */
    uint8_t  relay_state; /* 0/1 */
    uint8_t  ct_state;    /* 0=OFF 1=RUNNING 2=NO_CURRENT_WARN */
    int16_t  t_tpo_x10;
    int16_t  t_tpu_x10;
    int16_t  t_amb_x10;
    uint8_t  fault_flags; /* BBU_FAULT_* bits */
} comms_sample_t;

/* Hooks for params that live outside params.c (ids 8/9/10: user mode,
 * manual relay, comms enable). Registered by main.c; return false on
 * rejection (range). This is the same validated setter path the local
 * UI uses — nothing arriving over ESP-NOW can do more than the UI can. */
typedef struct {
    bool (*set)(uint8_t param_id, int32_t value);
    bool (*get)(uint8_t param_id, int32_t *out_value);
} comms_param_hooks_t;

/* Load NVS identity/flags, allocate FIFO, create comms task (idle until
 * enabled). Never blocks the caller. */
void comms_init(void);

bool comms_enabled(void);

/* Enable/disable the comms path. Persists in NVS. Enabling wakes the
 * comms task (radio comes up lazily); disabling stops all transmission. */
void comms_enable(bool on);

/* Called ~1 Hz from the monitor task. Cadence-gated inside (default 15 s);
 * samples are buffered regardless of gateway reachability. */
void comms_offer_sample(const comms_sample_t *s);

/* Best-effort async event (EVENT_FAULT_*, EVENT_PARAM_CHANGED). Queued and
 * sent when the gateway is reachable; dropped + counted during outage. */
void comms_offer_event(uint8_t event_id, const uint8_t *value, uint8_t len);

/* Encode a param's raw value to its wire form (type from the params
 * descriptor table). Returns bytes written (1/2/4) or 0 on unknown id. */
size_t comms_encode_param_value(uint8_t param_id, int32_t raw, uint8_t *out);

/* Bench/status (serial). */
void comms_status_print(void);
bool comms_set_ident(uint8_t node_type, uint8_t node_id); /* NVS-persisted */
void comms_set_sample_period_s(uint32_t s);               /* default 15  */
bool comms_ring_resize(uint16_t samples); /* bench: reallocates EMPTY ring */
uint8_t comms_node_type(void);
uint8_t comms_node_id(void);
