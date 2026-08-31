#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "espnow_schema.h"

typedef struct {
    float tpo_setpoint_c;
    float hysteresis_c;
    uint32_t min_on_time_s;
    uint32_t min_off_time_s;
    uint32_t ct_confirm_s;
    float min_tpo_tpu_delta_c;
    uint32_t max_run_time_min;
} bbu_params_t;

/* DN003 param descriptor: one table drives CONFIG_DESC serialization and
 * the validated id-based setter (admin panel + serial UI share one path). */
typedef struct {
    uint8_t id;
    uint8_t type;   /* PTYPE_* */
    uint8_t flags;  /* PARAM_FLAG_* */
    int32_t min;    /* raw units: x10 for PTYPE_I16_X10 */
    int32_t max;
    int32_t step;
    const char *name;
} bbu_param_desc_t;

void params_init(void);
const bbu_params_t *params_get(void);
void params_set_defaults(void);
bool params_set(const char *name, const char *value);
int params_save(void);
void params_print(void);
float params_tpo_on_c(void);
float params_tpo_off_c(void);

/* Descriptor table (BBU_PARAM_COUNT entries). */
const bbu_param_desc_t *params_table(int *count);
const bbu_param_desc_t *params_desc_by_id(uint8_t id);
bool params_id_from_name(const char *name, uint8_t *id);

/* Validated setter by param_id. Returns false if rejected (range/unknown). */
bool params_set_by_id(uint8_t id, int32_t raw);
/* Raw value for PARAM_GET/PARAM_ACK. Returns false on unknown id. */
bool params_get_raw_by_id(uint8_t id, int32_t *raw);

/* Fires on LOCAL changes only (serial `prog` / later the UI), never on the
 * PARAM_SET path (PARAM_ACK is the echo there). Registered by main.c. */
typedef void (*params_changed_fn_t)(uint8_t id, int32_t raw);
void params_register_changed_cb(params_changed_fn_t fn);

/* Hook pair for ids that live outside params.c (USER_MODE 8, MANUAL_RELAY
 * 9, COMMS_ENABLE 10) — main.c routes them to the control struct / comms
 * gate. Both return false when the value is invalid. */
void params_register_ext_setters(bool (*set)(uint8_t id, int32_t v),
                                 bool (*get)(uint8_t id, int32_t *out));
