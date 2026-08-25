#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ntc.h"
#include "params.h"

#define BBU_TPU_INVERT_SLACK_C 1.0f
#define BBU_TEST_LIMIT_S       (15 * 60)
#define BBU_STUCK_CONFIRM_S    2

typedef enum {
    BBU_MODE_MANUAL = 0,
    BBU_MODE_NORMAL,
    BBU_MODE_TPO_ONLY,
    BBU_MODE_TESTING,
    BBU_MODE_FAULT,
} bbu_mode_t;

typedef enum {
    BBU_CYCLE_IDLE = 0,
    BBU_CYCLE_RUNNING,
} bbu_cycle_t;

enum {
    BBU_EVT_MODE       = 1u << 0,
    BBU_EVT_CYCLE      = 1u << 1,
    BBU_EVT_WARN_STUCK = 1u << 2,
    BBU_EVT_WARN_MAX   = 1u << 3,
    BBU_EVT_TEST_END   = 1u << 4,
    BBU_EVT_WARN_NOCT  = 1u << 5,
};

typedef struct {
    ntc_sample_t tpo;
    ntc_sample_t tpu;
    bool ct_present;
    uint32_t dt_s;
} bbu_sense_t;

typedef struct {
    bbu_mode_t mode;
    bbu_cycle_t cycle;
    bool relay_on;
    bool warn_stuck;
    bool warn_maxrun;
    bool warn_noct;
    uint32_t mode_s;
    uint32_t cycle_s;
    uint32_t run_s;
    uint32_t stuck_s;
    uint8_t tpo_only_src; /* bit0 TPU (TPO_ONLY). CT is warn-only. */
} bbu_ctrl_t;

void bbu_ctrl_init(bbu_ctrl_t *c);
const char *bbu_mode_name(bbu_mode_t m);
const char *bbu_cycle_name(bbu_cycle_t cy);

/* User commands. MANUAL/TESTING keep the coil; auto modes start IDLE / OFF. */
void bbu_ctrl_request_mode(bbu_ctrl_t *c, bbu_mode_t mode);
void bbu_ctrl_manual_relay(bbu_ctrl_t *c, bool on);

/* One control tick. Returns a bitmask of BBU_EVT_*. */
uint32_t bbu_ctrl_tick(bbu_ctrl_t *c, const bbu_sense_t *s, const bbu_params_t *p);
