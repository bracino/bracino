#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "control.h"
#include "ntc.h"
#include "params.h"

typedef struct {
    ntc_sample_t tpo;
    ntc_sample_t tpu;
    bool ct_fitted;
    bool ct_present;
} ui_live_t;

void ui_init(SemaphoreHandle_t mu, bbu_ctrl_t *ctrl,
             const bbu_params_t *(*params_get)(void),
             void (*apply)(void));
void ui_set_live(const ui_live_t *live); /* call with mutex held */
void ui_task(void *arg);
