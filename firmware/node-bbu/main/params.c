#include "params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PARAMS_NS     "bbu"
#define PARAMS_KEY    "p1"
#define PARAMS_MAGIC  0x31425542u /* 'BBU1' */
#define PARAMS_VER    1u

typedef struct {
    uint32_t magic;
    uint32_t ver;
    bbu_params_t p;
} params_blob_t;

static bbu_params_t s_p;

static void defaults(bbu_params_t *p)
{
    p->tpo_setpoint_c = 60.0f;
    p->hysteresis_c = 3.0f;
    p->min_on_time_s = 180;
    p->min_off_time_s = 60;
    p->ct_confirm_s = 10;
    p->min_tpo_tpu_delta_c = 5.0f;
    p->max_run_time_min = 60;
}

void params_set_defaults(void)
{
    defaults(&s_p);
}

const bbu_params_t *params_get(void)
{
    return &s_p;
}

float params_tpo_on_c(void)
{
    return s_p.tpo_setpoint_c - s_p.hysteresis_c * 0.5f;
}

float params_tpo_off_c(void)
{
    return s_p.tpo_setpoint_c + s_p.hysteresis_c * 0.5f;
}

void params_print(void)
{
    printf("params (beta=3950 assumed):\n");
    printf("  tpo_setpoint_c       %.1f\n", (double)s_p.tpo_setpoint_c);
    printf("  hysteresis_c         %.1f\n", (double)s_p.hysteresis_c);
    printf("  min_on_time_s        %lu\n", (unsigned long)s_p.min_on_time_s);
    printf("  min_off_time_s       %lu\n", (unsigned long)s_p.min_off_time_s);
    printf("  ct_confirm_s         %lu\n", (unsigned long)s_p.ct_confirm_s);
    printf("  min_tpo_tpu_delta_c  %.1f\n", (double)s_p.min_tpo_tpu_delta_c);
    printf("  max_run_time_min     %lu\n", (unsigned long)s_p.max_run_time_min);
    printf("  derived tpo_on=%.1f  tpo_off=%.1f\n",
           (double)params_tpo_on_c(), (double)params_tpo_off_c());
}

static bool in_range_f(float v, float lo, float hi)
{
    return v >= lo && v <= hi;
}

bool params_set(const char *name, const char *value)
{
    char *end = NULL;
    double d = strtod(value, &end);
    if (end == value || (end && *end != '\0')) {
        printf("bad value '%s'\n", value);
        return false;
    }

    if (strcmp(name, "tpo_setpoint_c") == 0) {
        if (!in_range_f((float)d, 20.0f, 90.0f)) {
            printf("tpo_setpoint_c 20..90\n");
            return false;
        }
        s_p.tpo_setpoint_c = (float)d;
    } else if (strcmp(name, "hysteresis_c") == 0) {
        if (!in_range_f((float)d, 0.5f, 15.0f)) {
            printf("hysteresis_c 0.5..15\n");
            return false;
        }
        s_p.hysteresis_c = (float)d;
    } else if (strcmp(name, "min_on_time_s") == 0) {
        if (d < 0.0 || d > 3600.0) {
            printf("min_on_time_s 0..3600\n");
            return false;
        }
        s_p.min_on_time_s = (uint32_t)d;
    } else if (strcmp(name, "min_off_time_s") == 0) {
        if (d < 0.0 || d > 3600.0) {
            printf("min_off_time_s 0..3600\n");
            return false;
        }
        s_p.min_off_time_s = (uint32_t)d;
    } else if (strcmp(name, "ct_confirm_s") == 0) {
        if (d < 1.0 || d > 120.0) {
            printf("ct_confirm_s 1..120\n");
            return false;
        }
        s_p.ct_confirm_s = (uint32_t)d;
    } else if (strcmp(name, "min_tpo_tpu_delta_c") == 0) {
        if (!in_range_f((float)d, 0.0f, 30.0f)) {
            printf("min_tpo_tpu_delta_c 0..30\n");
            return false;
        }
        s_p.min_tpo_tpu_delta_c = (float)d;
    } else if (strcmp(name, "max_run_time_min") == 0) {
        if (d < 1.0 || d > 240.0) {
            printf("max_run_time_min 1..240\n");
            return false;
        }
        s_p.max_run_time_min = (uint32_t)d;
    } else {
        printf("unknown param '%s'\n", name);
        return false;
    }
    return true;
}

int params_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PARAMS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    params_blob_t blob = {
        .magic = PARAMS_MAGIC,
        .ver = PARAMS_VER,
        .p = s_p,
    };
    err = nvs_set_blob(h, PARAMS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void params_init(void)
{
    defaults(&s_p);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        printf("nvs init %s — using defaults\n", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    err = nvs_open(PARAMS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }
    params_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, PARAMS_KEY, &blob, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(blob) &&
        blob.magic == PARAMS_MAGIC && blob.ver == PARAMS_VER) {
        s_p = blob.p;
    }
}
