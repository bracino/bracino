#include "control.h"

#include <string.h>

static bbu_persist_fn_t s_persist_cb;
static bbu_stats_fn_t s_stats_cb;

static bool is_user_mode(bbu_mode_t m)
{
    return m == BBU_MODE_AUTO || m == BBU_MODE_MANUAL ||
           m == BBU_MODE_TESTING || m == BBU_MODE_OFF;
}

static void go_idle(bbu_ctrl_t *c)
{
    c->cycle = BBU_CYCLE_IDLE;
    c->cycle_s = 0;
    c->relay_on = false;
    c->run_s = 0;
    c->warn_maxrun = false;
}

static void go_running(bbu_ctrl_t *c)
{
    c->cycle = BBU_CYCLE_RUNNING;
    c->cycle_s = 0;
    c->relay_on = true;
    c->run_s = 0;
    c->warn_maxrun = false;
    c->starts++;
}

void bbu_ctrl_register_persist_cb(bbu_persist_fn_t fn)
{
    s_persist_cb = fn;
}

void bbu_ctrl_register_stats_cb(bbu_stats_fn_t fn)
{
    s_stats_cb = fn;
}

void bbu_ctrl_save_stats(bbu_ctrl_t *c)
{
    (void)c;
    if (s_stats_cb) {
        s_stats_cb(c->total_run_s, c->starts);
    }
}

/* Persist last-known user mode + Manual coil state. Never called for
 * TESTING (reboot during Test → Manual/OFF per DN002), never from the
 * Auto loop's own go_idle/go_running. */
static void persist_boot(bbu_ctrl_t *c)
{
    if (s_persist_cb == NULL) {
        return;
    }
    if (c->user_mode == BBU_MODE_TESTING) {
        return; /* intrinsically transient — not persisted */
    }
    s_persist_cb(c->user_mode, c->relay_on);
}

void bbu_ctrl_init(bbu_ctrl_t *c)
{
    memset(c, 0, sizeof(*c));
    c->mode = BBU_MODE_MANUAL;
    c->user_mode = BBU_MODE_MANUAL;
    c->cycle = BBU_CYCLE_IDLE;
}

const char *bbu_mode_name(bbu_mode_t m)
{
    switch (m) {
    case BBU_MODE_AUTO:     return "Auto";
    case BBU_MODE_TPO_ONLY: return "Auto*";
    case BBU_MODE_TESTING:  return "Test";
    case BBU_MODE_FAULT:    return "Fault";
    case BBU_MODE_OFF:      return "Off";
    default:                return "Manual";
    }
}

const char *bbu_cycle_name(bbu_cycle_t cy)
{
    return (cy == BBU_CYCLE_RUNNING) ? "RUNNING" : "IDLE";
}

void bbu_ctrl_request_mode(bbu_ctrl_t *c, bbu_mode_t mode)
{
    bbu_mode_t um0 = c->user_mode;
    if (!is_user_mode(mode)) {
        return;
    }
    c->user_mode = mode;
    c->mode_s = 0;
    c->tpo_only_src = 0;
    if (c->mode == BBU_MODE_FAULT) {
        if (mode == BBU_MODE_AUTO || mode == BBU_MODE_OFF) {
            go_idle(c);
        }
        if (c->user_mode != um0) {
            persist_boot(c);
        }
        return;
    }
    if (mode == BBU_MODE_AUTO &&
        (c->mode == BBU_MODE_AUTO || c->mode == BBU_MODE_TPO_ONLY)) {
        return;
    }
    if (c->mode == mode) {
        return;
    }
    c->mode = mode;
    if (mode == BBU_MODE_AUTO || mode == BBU_MODE_OFF) {
        go_idle(c);
    }
    if (c->user_mode != um0) {
        persist_boot(c);
    }
}

void bbu_ctrl_manual_relay(bbu_ctrl_t *c, bool on)
{
    if (c->mode != BBU_MODE_MANUAL && c->mode != BBU_MODE_TESTING) {
        c->mode = BBU_MODE_MANUAL;
        c->user_mode = BBU_MODE_MANUAL;
        c->mode_s = 0;
        c->tpo_only_src = 0;
    }
    if (on) {
        go_running(c);
    } else {
        go_idle(c);
    }
    if (c->mode == BBU_MODE_MANUAL) {
        /* Manual coil state is the operator's explicit pump decision —
         * a power blip must not silently change it. In TESTING the coil
         * toggle is transient and is not persisted. */
        persist_boot(c);
    }
}

void bbu_ctrl_clear_stats(bbu_ctrl_t *c)
{
    c->starts = 0;
    c->total_run_s = 0;
    if (s_stats_cb) {
        s_stats_cb(c->total_run_s, c->starts);
    }
}

static float on_c(const bbu_params_t *p)
{
    return p->tpo_setpoint_c - p->hysteresis_c * 0.5f;
}

static float off_c(const bbu_params_t *p)
{
    return p->tpo_setpoint_c + p->hysteresis_c * 0.5f;
}

uint32_t bbu_ctrl_tick(bbu_ctrl_t *c, const bbu_sense_t *s, const bbu_params_t *p)
{
    uint32_t ev = 0;
    bbu_mode_t mode0 = c->mode;
    bbu_cycle_t cy0 = c->cycle;
    bool stuck0 = c->warn_stuck;
    bool max0 = c->warn_maxrun;
    bool noct0 = c->warn_noct;

    uint32_t dt = s->dt_s ? s->dt_s : 1;
    c->mode_s += dt;
    c->cycle_s += dt;

    if (c->relay_on) {
        c->run_s += dt;
        c->total_run_s += dt;
        if (c->run_s >= p->max_run_time_min * 60u) {
            c->warn_maxrun = true;
        }
    }

    if (!c->relay_on && s->ct_present) {
        c->stuck_s += dt;
        if (c->stuck_s >= BBU_STUCK_CONFIRM_S) {
            c->warn_stuck = true;
        }
    } else {
        c->stuck_s = 0;
        c->warn_stuck = false;
    }

    if (c->user_mode == BBU_MODE_TESTING && c->mode_s >= BBU_TEST_LIMIT_S) {
        c->user_mode = BBU_MODE_AUTO;
        c->mode = BBU_MODE_AUTO;
        c->mode_s = 0;
        c->tpo_only_src = 0;
        go_idle(c);
        ev |= BBU_EVT_TEST_END;
    }

    bool tpo_bad = !s->tpo.ok;
    bool tpu_bad = !s->tpu.ok;
    if (s->tpo.ok && s->tpu.ok && s->tpu.c > s->tpo.c + BBU_TPU_INVERT_SLACK_C) {
        tpu_bad = true;
    }
    /* CT-less hardware (014): warn_noct can never fire. */
    bool no_ct = s->ct_fitted &&
                 (c->cycle == BBU_CYCLE_RUNNING) &&
                 (c->cycle_s >= p->ct_confirm_s) &&
                 !s->ct_present;
    c->warn_noct = no_ct;

    if (tpo_bad) {
        if (c->mode != BBU_MODE_FAULT) {
            c->mode = BBU_MODE_FAULT;
            c->mode_s = 0;
            c->tpo_only_src = 0;
            c->last_fault = BBU_LAST_TPO;
            go_idle(c);
        }
        goto done;
    }

    if (c->mode == BBU_MODE_FAULT) {
        c->mode = c->user_mode;
        c->mode_s = 0;
        go_idle(c);
    }

    if (c->mode == BBU_MODE_MANUAL || c->mode == BBU_MODE_TESTING ||
        c->mode == BBU_MODE_OFF) {
        goto done;
    }

    if (c->user_mode != BBU_MODE_AUTO) {
        goto done;
    }

    if (tpu_bad) {
        c->tpo_only_src |= 1u;
        c->last_fault = BBU_LAST_TPU;
    } else {
        c->tpo_only_src &= ~1u;
    }

    if (c->tpo_only_src) {
        if (c->mode == BBU_MODE_AUTO) {
            c->mode = BBU_MODE_TPO_ONLY;
            c->mode_s = 0;
        }
    } else if (c->mode == BBU_MODE_TPO_ONLY) {
        c->mode = BBU_MODE_AUTO;
        c->mode_s = 0;
    }

    if (c->mode != BBU_MODE_AUTO && c->mode != BBU_MODE_TPO_ONLY) {
        goto done;
    }

    if (c->cycle == BBU_CYCLE_IDLE) {
        if (s->tpo.c <= on_c(p) && c->cycle_s >= p->min_off_time_s) {
            go_running(c);
        }
    } else {
        bool min_on = c->cycle_s >= p->min_on_time_s;
        bool tpo_hi = s->tpo.c >= off_c(p);
        bool charged = (c->mode == BBU_MODE_TPO_ONLY) ||
                       (s->tpu.ok && (s->tpo.c - s->tpu.c) <= p->min_tpo_tpu_delta_c);
        if (min_on && tpo_hi && charged) {
            go_idle(c);
        }
    }

done:
    if (c->mode != mode0) {
        ev |= BBU_EVT_MODE;
    }
    if (c->cycle != cy0) {
        ev |= BBU_EVT_CYCLE;
    }
    if (c->warn_stuck && !stuck0) {
        ev |= BBU_EVT_WARN_STUCK;
    }
    if (c->warn_maxrun && !max0) {
        ev |= BBU_EVT_WARN_MAX;
    }
    if (c->warn_noct && !noct0) {
        ev |= BBU_EVT_WARN_NOCT;
    }
    return ev;
}
