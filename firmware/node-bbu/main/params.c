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
static params_changed_fn_t s_changed_cb;

void params_register_changed_cb(params_changed_fn_t fn)
{
    s_changed_cb = fn;
}

/* DN003 param table: ids are BBU_PARAM_* (shared header). Range checks
 * mirror params_set() string validation exactly — one setter semantics. */
static const bbu_param_desc_t s_table[] = {
    { BBU_PARAM_TPO_SETPOINT_C,      PTYPE_I16_X10, PARAM_FLAG_RW, 200, 900,   5, "tpo_setpoint_c" },
    { BBU_PARAM_HYSTERESIS_C,        PTYPE_I16_X10, PARAM_FLAG_RW, 5,   150,   5, "hysteresis_c" },
    { BBU_PARAM_MIN_ON_TIME_S,       PTYPE_U32,     PARAM_FLAG_RW, 0,   3600,  1, "min_on_time_s" },
    { BBU_PARAM_MIN_OFF_TIME_S,      PTYPE_U32,     PARAM_FLAG_RW, 0,   3600,  1, "min_off_time_s" },
    { BBU_PARAM_CT_CONFIRM_S,        PTYPE_U32,     PARAM_FLAG_RW, 1,   120,   1, "ct_confirm_s" },
    { BBU_PARAM_MIN_TPO_TPU_DELTA_C, PTYPE_I16_X10, PARAM_FLAG_RW, 0,   300,   5, "min_tpo_tpu_delta_c" },
    { BBU_PARAM_MAX_RUN_TIME_MIN,    PTYPE_U32,     PARAM_FLAG_RW, 1,   240,   1, "max_run_time_min" },
    { BBU_PARAM_USER_MODE,           PTYPE_ENUM,    PARAM_FLAG_RW, 0,   3,     1, "user_mode" },
    { BBU_PARAM_MANUAL_RELAY,        PTYPE_ENUM,    PARAM_FLAG_RW, 0,   1,     1, "manual_relay" },
    { BBU_PARAM_COMMS_ENABLE,        PTYPE_ENUM,    PARAM_FLAG_RW, 0,   1,     1, "comms_enable" },
    { BBU_PARAM_SAMPLE_PERIOD_S,     PTYPE_U32,     PARAM_FLAG_RW, 5,   120,   5, "sample_period_s" },
};

const bbu_param_desc_t *params_table(int *count)
{
    if (count) {
        *count = sizeof(s_table) / sizeof(s_table[0]);
    }
    return s_table;
}

const bbu_param_desc_t *params_desc_by_id(uint8_t id)
{
    for (size_t i = 0; i < sizeof(s_table) / sizeof(s_table[0]); i++) {
        if (s_table[i].id == id) {
            return &s_table[i];
        }
    }
    return NULL;
}

bool params_id_from_name(const char *name, uint8_t *id)
{
    for (size_t i = 0; i < sizeof(s_table) / sizeof(s_table[0]); i++) {
        if (strcmp(s_table[i].name, name) == 0) {
            *id = s_table[i].id;
            return true;
        }
    }
    return false;
}

/* ids 8/9/10/11 live outside params.c (mode/relay/comms/tel period) —
 * main.c registers one hook pair; still a single validated setter path.
 * sample_period_s is NOT in the params NVS blob (would bust p1 size). */
static bool (*s_ext_set)(uint8_t id, int32_t v);
static bool (*s_ext_get)(uint8_t id, int32_t *v);

void params_register_ext_setters(bool (*set)(uint8_t, int32_t),
                                 bool (*get)(uint8_t, int32_t *))
{
    s_ext_set = set;
    s_ext_get = get;
}

bool params_set_by_id(uint8_t id, int32_t v)
{
    const bbu_param_desc_t *d = params_desc_by_id(id);
    if (d == NULL) {
        return false;
    }
    if (d->type == PTYPE_U32 && v < 0) {
        return false; /* negative into an unsigned param: type error */
    }
    if (v < d->min || v > d->max) {
        return false;
    }
    switch (id) {
    case BBU_PARAM_TPO_SETPOINT_C:      s_p.tpo_setpoint_c = v / 10.0f; params_save(); break;
    case BBU_PARAM_HYSTERESIS_C:        s_p.hysteresis_c = v / 10.0f; params_save(); break;
    case BBU_PARAM_MIN_ON_TIME_S:       s_p.min_on_time_s = (uint32_t)v; params_save(); break;
    case BBU_PARAM_MIN_OFF_TIME_S:      s_p.min_off_time_s = (uint32_t)v; params_save(); break;
    case BBU_PARAM_CT_CONFIRM_S:        s_p.ct_confirm_s = (uint32_t)v; params_save(); break;
    case BBU_PARAM_MIN_TPO_TPU_DELTA_C: s_p.min_tpo_tpu_delta_c = v / 10.0f; params_save(); break;
    case BBU_PARAM_MAX_RUN_TIME_MIN:    s_p.max_run_time_min = (uint32_t)v; params_save(); break;
    default:
        if (s_ext_set && s_ext_set(id, v)) {
            break;
        }
        return false;
    }
    return true;
}

bool params_get_raw_by_id(uint8_t id, int32_t *raw)
{
    if (raw == NULL) {
        return false;
    }
    switch (id) {
    case BBU_PARAM_TPO_SETPOINT_C:      *raw = (int32_t)(s_p.tpo_setpoint_c * 10.0f); return true;
    case BBU_PARAM_HYSTERESIS_C:        *raw = (int32_t)(s_p.hysteresis_c * 10.0f); return true;
    case BBU_PARAM_MIN_ON_TIME_S:       *raw = (int32_t)s_p.min_on_time_s; return true;
    case BBU_PARAM_MIN_OFF_TIME_S:      *raw = (int32_t)s_p.min_off_time_s; return true;
    case BBU_PARAM_CT_CONFIRM_S:        *raw = (int32_t)s_p.ct_confirm_s; return true;
    case BBU_PARAM_MIN_TPO_TPU_DELTA_C: *raw = (int32_t)(s_p.min_tpo_tpu_delta_c * 10.0f); return true;
    case BBU_PARAM_MAX_RUN_TIME_MIN:    *raw = (int32_t)s_p.max_run_time_min; return true;
    default:
        return s_ext_get && s_ext_get(id, raw);
    }
}

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

/* DN002: every successful local change persists immediately (human-rate
 * writes; `prog default` still needs an explicit `save` by design). */
static void autosave(void)
{
    params_save();
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

    uint8_t id;
    bool id_ok = params_id_from_name(name, &id);

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
    } else if (id_ok && id == BBU_PARAM_SAMPLE_PERIOD_S) {
        if (!params_set_by_id(id, (int32_t)d)) {
            printf("sample_period_s 5..120\n");
            return false;
        }
    } else {
        printf("unknown param '%s'\n", name);
        return false;
    }

    autosave();
    if (id_ok && s_changed_cb) {
        int32_t raw;
        if (params_get_raw_by_id(id, &raw)) {
            s_changed_cb(id, raw);
        }
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
