#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float tpo_setpoint_c;
    float hysteresis_c;
    uint32_t min_on_time_s;
    uint32_t min_off_time_s;
    uint32_t ct_confirm_s;
    float min_tpo_tpu_delta_c;
    uint32_t max_run_time_min;
} bbu_params_t;

void params_init(void);
const bbu_params_t *params_get(void);
void params_set_defaults(void);
bool params_set(const char *name, const char *value);
int params_save(void);
void params_print(void);
float params_tpo_on_c(void);
float params_tpo_off_c(void);
