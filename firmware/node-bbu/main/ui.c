#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "enc.h"
#include "tft.h"

enum {
    SCR_HOME = 0,
    SCR_SEL,
    SCR_TEMP,
    SCR_CNT,
    SCR_SYS,
    SCR_PROG,
    SCR_DIAG,
};

static SemaphoreHandle_t s_mu;
static bbu_ctrl_t *s_c;
static const bbu_params_t *(*s_params)(void);
static void (*s_apply)(void);
static ui_live_t s_live;

static int s_scr = SCR_HOME;
static int s_cur;
static bool s_dirty = true;

static const bbu_mode_t k_user_modes[] = {
    BBU_MODE_AUTO, BBU_MODE_MANUAL, BBU_MODE_TESTING, BBU_MODE_OFF
};

static void go_home(void)
{
    s_scr = SCR_HOME;
    s_cur = 0;
    s_dirty = true;
}

static void clip_cur(int n)
{
    if (n <= 0) {
        s_cur = 0;
        return;
    }
    while (s_cur < 0) {
        s_cur += n;
    }
    s_cur %= n;
}

static void fmt_temp(char *dst, size_t n, const ntc_sample_t *s)
{
    if (!s->ok) {
        snprintf(dst, n, "FAULT");
        return;
    }
    snprintf(dst, n, "%4.1f C", (double)s->c);
}

static const char *fault_txt(uint8_t f)
{
    switch (f) {
    case BBU_LAST_TPO: return "TPO";
    case BBU_LAST_TPU: return "TPU";
    default:           return "none";
    }
}

static void draw_home(const bbu_ctrl_t *c, const ui_live_t *lv, const bbu_params_t *p)
{
    char l[32];
    char t[12];
    uint16_t mc = COL_FG;
    if (c->mode == BBU_MODE_FAULT) {
        mc = COL_BAD;
    } else if (c->mode == BBU_MODE_TPO_ONLY || c->warn_noct || c->warn_stuck) {
        mc = COL_WARN;
    }
    snprintf(l, sizeof(l), "BBU          %s", bbu_mode_name(c->mode));
    tft_text_row(0, l, mc, COL_BG);
    tft_text_row(1, "", COL_FG, COL_BG);

    fmt_temp(t, sizeof(t), &lv->tpo);
    snprintf(l, sizeof(l), "TPO   %s", t);
    tft_text_row(2, l, lv->tpo.ok ? COL_FG : COL_BAD, COL_BG);

    fmt_temp(t, sizeof(t), &lv->tpu);
    snprintf(l, sizeof(l), "TPU   %s", t);
    tft_text_row(3, l, lv->tpu.ok ? COL_FG : COL_BAD, COL_BG);

    snprintf(l, sizeof(l), "Pump  %s / %s",
             c->relay_on ? "ON" : "OFF",
             lv->ct_present ? "Active" : "none");
    tft_text_row(4, l, COL_FG, COL_BG);

    snprintf(l, sizeof(l), "Set   %.0f C", (double)p->tpo_setpoint_c);
    tft_text_row(5, l, COL_FG, COL_BG);

    tft_text_row(6, "> Selection", COL_ACCENT, COL_BG);

    const char *w = "";
    if (c->mode == BBU_MODE_FAULT) {
        w = "TPO fault";
    } else if (c->warn_stuck) {
        w = "WARN stuck-on";
    } else if (c->warn_noct) {
        w = "WARN no CT";
    } else if (c->warn_maxrun) {
        w = "WARN max run";
    } else if (c->mode == BBU_MODE_TPO_ONLY) {
        w = "TPU ignored";
    }
    tft_text_row(7, w, w[0] ? COL_WARN : COL_DIM, COL_BG);
    tft_fill_rect(0, 8 * 16, TFT_W, TFT_H - 8 * 16, COL_BG);
}

static void row_at(int y, bool on, const char *s)
{
    char l[32];
    snprintf(l, sizeof(l), "%s %s", on ? ">" : " ", s);
    tft_text_row(y, l, on ? COL_ACCENT : COL_FG, COL_BG);
}

static void draw_sel(int cur)
{
    tft_text_row(0, "Selection", COL_DIM, COL_BG);
    row_at(1, cur == 0, "Temperatures");
    row_at(2, cur == 1, "Counters");
    row_at(3, cur == 2, "System Data");
    row_at(4, cur == 3, "Control Program");
    row_at(5, cur == 4, "Diagnostics");
    row_at(6, cur == 5, "Back");
    tft_fill_rect(0, 7 * 16, TFT_W, TFT_H - 7 * 16, COL_BG);
}

static void draw_temp(int cur, const ui_live_t *lv, const bbu_params_t *p)
{
    char l[32];
    char t[12];
    tft_text_row(0, "Temperatures", COL_DIM, COL_BG);
    fmt_temp(t, sizeof(t), &lv->tpo);
    snprintf(l, sizeof(l), "TPO (Upper) %s", t);
    tft_text_row(1, l, COL_FG, COL_BG);
    fmt_temp(t, sizeof(t), &lv->tpu);
    snprintf(l, sizeof(l), "TPU (Lower) %s", t);
    tft_text_row(2, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Setpoint    %.1f C", (double)p->tpo_setpoint_c);
    tft_text_row(3, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Hysteresis  %.1f K", (double)p->hysteresis_c);
    tft_text_row(4, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Off offset  %.1f K", (double)p->min_tpo_tpu_delta_c);
    tft_text_row(5, l, COL_FG, COL_BG);
    row_at(6, cur == 0, "Back");
    tft_fill_rect(0, 7 * 16, TFT_W, TFT_H - 7 * 16, COL_BG);
}

static void draw_cnt(int cur, const bbu_ctrl_t *c)
{
    char l[32];
    tft_text_row(0, "Counters", COL_DIM, COL_BG);
    snprintf(l, sizeof(l), "Runtime  %lu h",
             (unsigned long)(c->total_run_s / 3600u));
    tft_text_row(1, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Starts   %lu", (unsigned long)c->starts);
    tft_text_row(2, l, COL_FG, COL_BG);
    row_at(3, cur == 0, "Clear counters");
    row_at(4, cur == 1, "Back");
    tft_fill_rect(0, 5 * 16, TFT_W, TFT_H - 5 * 16, COL_BG);
}

static void draw_sys(int cur, const bbu_params_t *p)
{
    char l[32];
    tft_text_row(0, "System Data", COL_DIM, COL_BG);
    snprintf(l, sizeof(l), "Setpoint  %.1f C", (double)p->tpo_setpoint_c);
    tft_text_row(1, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Hysteresis %.1f K", (double)p->hysteresis_c);
    tft_text_row(2, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Min on    %lus", (unsigned long)p->min_on_time_s);
    tft_text_row(3, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Min off   %lus", (unsigned long)p->min_off_time_s);
    tft_text_row(4, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Off offset %.1f K", (double)p->min_tpo_tpu_delta_c);
    tft_text_row(5, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "CT wait   %lus", (unsigned long)p->ct_confirm_s);
    tft_text_row(6, l, COL_FG, COL_BG);
    snprintf(l, sizeof(l), "Max run   %lum", (unsigned long)p->max_run_time_min);
    tft_text_row(7, l, COL_FG, COL_BG);
    row_at(8, cur == 0, "Back");
    tft_fill_rect(0, 9 * 16, TFT_W, TFT_H - 9 * 16, COL_BG);
}

static void draw_prog(int cur, const bbu_ctrl_t *c, const ui_live_t *lv)
{
    char l[32];
    tft_text_row(0, "Control Program", COL_DIM, COL_BG);
    snprintf(l, sizeof(l), "%s Mode    %s",
             cur == 0 ? ">" : " ", bbu_mode_name(c->user_mode));
    tft_text_row(1, l, cur == 0 ? COL_ACCENT : COL_FG, COL_BG);
    snprintf(l, sizeof(l), "%s Pump    %s",
             cur == 1 ? ">" : " ", c->relay_on ? "On" : "Off");
    tft_text_row(2, l, cur == 1 ? COL_ACCENT : COL_FG, COL_BG);
    snprintf(l, sizeof(l), "  Current %s", lv->ct_present ? "Active" : "none");
    tft_text_row(3, l, COL_FG, COL_BG);
    row_at(4, cur == 2, "Back");
    tft_fill_rect(0, 5 * 16, TFT_W, TFT_H - 5 * 16, COL_BG);
}

static void draw_diag(int cur, const bbu_ctrl_t *c, const ui_live_t *lv)
{
    char l[32];
    tft_text_row(0, "Diagnostics", COL_DIM, COL_BG);
    snprintf(l, sizeof(l), "Sensors   %s",
             (lv->tpo.ok && lv->tpu.ok) ? "OK" : "FAULT");
    tft_text_row(1, l, (lv->tpo.ok && lv->tpu.ok) ? COL_OK : COL_BAD, COL_BG);
    const char *ct = "OK";
    uint16_t cc = COL_OK;
    if (c->warn_noct) {
        ct = "No current";
        cc = COL_WARN;
    } else if (c->warn_stuck) {
        ct = "Stuck on";
        cc = COL_WARN;
    } else if (!lv->ct_present) {
        ct = "none";
        cc = COL_DIM;
    }
    snprintf(l, sizeof(l), "Pump CT   %s", ct);
    tft_text_row(2, l, cc, COL_BG);
    snprintf(l, sizeof(l), "Last fault %s", fault_txt(c->last_fault));
    tft_text_row(3, l, COL_FG, COL_BG);
    tft_text_row(4, "FW  2026-08-25", COL_DIM, COL_BG);
    row_at(5, cur == 0, "Back");
    tft_fill_rect(0, 6 * 16, TFT_W, TFT_H - 6 * 16, COL_BG);
}

static int nitems(int scr)
{
    switch (scr) {
    case SCR_HOME: return 1;
    case SCR_SEL:  return 6;
    case SCR_TEMP: return 1;
    case SCR_CNT:  return 2;
    case SCR_SYS:  return 1;
    case SCR_PROG: return 3;
    case SCR_DIAG: return 1;
    default:       return 1;
    }
}

static void next_user_mode(bbu_ctrl_t *c)
{
    int i = 0;
    for (; i < 4; i++) {
        if (k_user_modes[i] == c->user_mode) {
            break;
        }
    }
    i = (i + 1) % 4;
    bbu_ctrl_request_mode(c, k_user_modes[i]);
}

static void on_click(bbu_ctrl_t *c)
{
    switch (s_scr) {
    case SCR_HOME:
        s_scr = SCR_SEL;
        s_cur = 0;
        break;
    case SCR_SEL:
        switch (s_cur) {
        case 0: s_scr = SCR_TEMP; s_cur = 0; break;
        case 1: s_scr = SCR_CNT;  s_cur = 0; break;
        case 2: s_scr = SCR_SYS;  s_cur = 0; break;
        case 3: s_scr = SCR_PROG; s_cur = 0; break;
        case 4: s_scr = SCR_DIAG; s_cur = 0; break;
        default: go_home(); break;
        }
        break;
    case SCR_TEMP:
    case SCR_SYS:
    case SCR_DIAG:
        s_scr = SCR_SEL;
        s_cur = 0;
        break;
    case SCR_CNT:
        if (s_cur == 0) {
            bbu_ctrl_clear_stats(c);
        } else {
            s_scr = SCR_SEL;
            s_cur = 1;
        }
        break;
    case SCR_PROG:
        if (s_cur == 0) {
            next_user_mode(c);
        } else if (s_cur == 1) {
            if (c->user_mode == BBU_MODE_MANUAL || c->user_mode == BBU_MODE_TESTING) {
                bbu_ctrl_manual_relay(c, !c->relay_on);
            }
        } else {
            s_scr = SCR_SEL;
            s_cur = 3;
        }
        break;
    default:
        go_home();
        break;
    }
    s_dirty = true;
}

void ui_init(SemaphoreHandle_t mu, bbu_ctrl_t *ctrl,
             const bbu_params_t *(*params_get)(void),
             void (*apply)(void))
{
    s_mu = mu;
    s_c = ctrl;
    s_params = params_get;
    s_apply = apply;
    enc_init();
    tft_init();
    tft_text_row(2, "  BBU node", COL_FG, COL_BG);
    tft_text_row(4, "  TFT + encoder", COL_DIM, COL_BG);
    tft_text_row(6, "  turn / click", COL_ACCENT, COL_BG);
}

void ui_set_live(const ui_live_t *live)
{
    s_live = *live;
}

void ui_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    s_dirty = true;
    int live_div = 0;
    for (;;) {
        enc_poll();
        int steps = enc_take_steps();
        bool click = enc_take_click();
        bool hold = enc_take_hold();

        if (hold) {
            go_home();
        } else if (steps || click) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            if (steps) {
                s_cur += steps;
                clip_cur(nitems(s_scr));
                s_dirty = true;
            }
            if (click) {
                on_click(s_c);
                if (s_apply) {
                    s_apply();
                }
            }
            xSemaphoreGive(s_mu);
        }

        live_div++;
        if (s_dirty || live_div >= 100) { /* ~500 ms at 5 ms poll */
            live_div = 0;
            bbu_ctrl_t snap;
            ui_live_t lv;
            bbu_params_t p;
            xSemaphoreTake(s_mu, portMAX_DELAY);
            snap = *s_c;
            lv = s_live;
            p = *s_params();
            xSemaphoreGive(s_mu);

            switch (s_scr) {
            case SCR_HOME: draw_home(&snap, &lv, &p); break;
            case SCR_SEL:  draw_sel(s_cur); break;
            case SCR_TEMP: draw_temp(s_cur, &lv, &p); break;
            case SCR_CNT:  draw_cnt(s_cur, &snap); break;
            case SCR_SYS:  draw_sys(s_cur, &p); break;
            case SCR_PROG: draw_prog(s_cur, &snap, &lv); break;
            case SCR_DIAG: draw_diag(s_cur, &snap, &lv); break;
            default: break;
            }
            s_dirty = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
