#include "ui.h"

#include <stdarg.h>
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

#define VIEW_ROWS 6

static SemaphoreHandle_t s_mu;
static bbu_ctrl_t *s_c;
static const bbu_params_t *(*s_params)(void);
static void (*s_apply)(void);
static ui_live_t s_live;

static int s_scr = SCR_HOME;
static int s_shown_scr = -1;
static int s_cur;
static int s_top;
static bool s_dirty = true;
static char s_cache[TFT_ROWS][TFT_COLS + 1];
static uint16_t s_cache_fg[TFT_ROWS];

static const bbu_mode_t k_user_modes[] = {
    BBU_MODE_AUTO, BBU_MODE_MANUAL, BBU_MODE_TESTING, BBU_MODE_OFF
};

static void go_home(void)
{
    s_scr = SCR_HOME;
    s_cur = 0;
    s_top = 0;
    s_dirty = true;
}

static void begin_page(void)
{
    if (s_shown_scr == s_scr) {
        return;
    }
    tft_fill(COL_BG);
    memset(s_cache, 0, sizeof(s_cache));
    s_shown_scr = s_scr;
}

static void line(int row, const char *s, uint16_t fg)
{
    char buf[TFT_COLS + 1];
    memset(buf, ' ', TFT_COLS);
    buf[TFT_COLS] = '\0';
    if (s) {
        size_t n = strlen(s);
        if (n > TFT_COLS) {
            n = TFT_COLS;
        }
        memcpy(buf, s, n);
    }
    if (s_cache[row][0] != '\0' &&
        memcmp(s_cache[row], buf, TFT_COLS + 1) == 0 &&
        s_cache_fg[row] == fg) {
        return;
    }
    memcpy(s_cache[row], buf, TFT_COLS + 1);
    s_cache_fg[row] = fg;
    tft_text_row(row, buf, fg, COL_BG);
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

static void sync_top(int n)
{
    if (n <= VIEW_ROWS) {
        s_top = 0;
        return;
    }
    if (s_cur < s_top) {
        s_top = s_cur;
    }
    if (s_cur >= s_top + VIEW_ROWS) {
        s_top = s_cur - (VIEW_ROWS - 1);
    }
    if (s_top < 0) {
        s_top = 0;
    }
    int max_top = n - VIEW_ROWS;
    if (s_top > max_top) {
        s_top = max_top;
    }
}

static void draw_items(const char *const *items, int n, uint16_t focus_fg)
{
    sync_top(n);
    for (int v = 0; v < VIEW_ROWS; v++) {
        int i = s_top + v;
        if (i >= n) {
            line(1 + v, "", COL_FG);
            continue;
        }
        uint16_t fg = (i == s_cur) ? focus_fg : COL_FG;
        line(1 + v, items[i], fg);
    }
}

static void cell(char *dst, size_t n, const char *fmt, ...)
{
    char tmp[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    memset(dst, 0, n);
    strncpy(dst, tmp, n - 1);
}

static void fmt_temp(char *dst, size_t n, const ntc_sample_t *s)
{
    if (!s->ok) {
        cell(dst, n, "FAULT");
        return;
    }
    cell(dst, n, "%4.1f C", (double)s->c);
}

static const char *fault_txt(uint8_t f)
{
    switch (f) {
    case BBU_LAST_TPO: return "TPO";
    case BBU_LAST_TPU: return "TPU";
    default:           return "none";
    }
}

static uint16_t mode_color(const bbu_ctrl_t *c)
{
    if (c->mode == BBU_MODE_FAULT) {
        return COL_BAD;
    }
    if (c->mode == BBU_MODE_TPO_ONLY || c->warn_noct || c->warn_stuck) {
        return COL_WARN;
    }
    return COL_HEADER;
}

static const char *warn_txt(const bbu_ctrl_t *c)
{
    if (c->mode == BBU_MODE_FAULT) {
        return "TPO fault";
    }
    if (c->warn_stuck) {
        return "WARN stuck-on";
    }
    if (c->warn_noct) {
        return "WARN no CT";
    }
    if (c->warn_maxrun) {
        return "WARN max run";
    }
    if (c->mode == BBU_MODE_TPO_ONLY) {
        return "TPU ignored";
    }
    return "";
}

static void draw_home(const bbu_ctrl_t *c, const ui_live_t *lv, const bbu_params_t *p)
{
    char l[32];
    char t[12];
    cell(l, sizeof(l), "BBU %8s", bbu_mode_name(c->mode));
    line(0, l, mode_color(c));

    fmt_temp(t, sizeof(t), &lv->tpo);
    cell(l, sizeof(l), "TPO   %s", t);
    line(1, l, lv->tpo.ok ? COL_FG : COL_BAD);

    fmt_temp(t, sizeof(t), &lv->tpu);
    cell(l, sizeof(l), "TPU   %s", t);
    line(2, l, lv->tpu.ok ? COL_FG : COL_BAD);

    cell(l, sizeof(l), "Pump %s/%s",
         c->relay_on ? "ON" : "OFF",
         lv->ct_present ? "CT" : "none");
    line(3, l, COL_FG);

    cell(l, sizeof(l), "Set    %4.0f C", (double)p->tpo_setpoint_c);
    line(4, l, COL_FG);

    line(5, "> Menu", COL_FOCUS);
    line(6, "", COL_FG);

    const char *w = warn_txt(c);
    line(7, w[0] ? w : "hold=home", w[0] ? COL_WARN : COL_FOOTER);
}

static void draw_sel(int cur)
{
    const char *items[] = {
        "Temperatures",
        "Counters",
        "System Data",
        "Control Prog",
        "Diagnostics",
        "Back",
    };
    (void)cur;
    line(0, "Selection", COL_HEADER);
    draw_items(items, 6, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static void draw_temp(const ui_live_t *lv, const bbu_params_t *p)
{
    char items[6][32];
    const char *pitems[6];
    char t[12];
    fmt_temp(t, sizeof(t), &lv->tpo);
    cell(items[0], sizeof(items[0]), "TPO   %s", t);
    fmt_temp(t, sizeof(t), &lv->tpu);
    cell(items[1], sizeof(items[1]), "TPU   %s", t);
    cell(items[2], sizeof(items[2]), "Setpt  %5.1fC",
         (double)p->tpo_setpoint_c);
    cell(items[3], sizeof(items[3]), "Hyst    %4.1fK",
         (double)p->hysteresis_c);
    cell(items[4], sizeof(items[4]), "dT off  %4.1fK",
         (double)p->min_tpo_tpu_delta_c);
    cell(items[5], sizeof(items[5]), "Back");
    for (int i = 0; i < 6; i++) {
        pitems[i] = items[i];
    }
    line(0, "Temperatures", COL_HEADER);
    draw_items(pitems, 6, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static void draw_cnt(const bbu_ctrl_t *c)
{
    char items[4][32];
    const char *pitems[4];
    cell(items[0], sizeof(items[0]), "Runtime %4luh",
         (unsigned long)(c->total_run_s / 3600u));
    cell(items[1], sizeof(items[1]), "Starts   %5lu",
         (unsigned long)c->starts);
    cell(items[2], sizeof(items[2]), "Clear counts");
    cell(items[3], sizeof(items[3]), "Back");
    for (int i = 0; i < 4; i++) {
        pitems[i] = items[i];
    }
    line(0, "Counters", COL_HEADER);
    draw_items(pitems, 4, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static void draw_sys(const bbu_params_t *p)
{
    char items[8][32];
    const char *pitems[8];
    cell(items[0], sizeof(items[0]), "Setpt  %5.1fC",
         (double)p->tpo_setpoint_c);
    cell(items[1], sizeof(items[1]), "Hyst    %4.1fK",
         (double)p->hysteresis_c);
    cell(items[2], sizeof(items[2]), "Min on   %4lus",
         (unsigned long)p->min_on_time_s);
    cell(items[3], sizeof(items[3]), "Min off  %4lus",
         (unsigned long)p->min_off_time_s);
    cell(items[4], sizeof(items[4]), "dT off  %4.1fK",
         (double)p->min_tpo_tpu_delta_c);
    cell(items[5], sizeof(items[5]), "CT wait  %4lus",
         (unsigned long)p->ct_confirm_s);
    cell(items[6], sizeof(items[6]), "Max run  %4lum",
         (unsigned long)p->max_run_time_min);
    cell(items[7], sizeof(items[7]), "Back");
    for (int i = 0; i < 8; i++) {
        pitems[i] = items[i];
    }
    line(0, "System Data", COL_HEADER);
    draw_items(pitems, 8, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static void draw_prog(const bbu_ctrl_t *c, const ui_live_t *lv)
{
    char items[4][32];
    const char *pitems[4];
    cell(items[0], sizeof(items[0]), "Mode %8s",
         bbu_mode_name(c->user_mode));
    cell(items[1], sizeof(items[1]), "Pump %8s",
         c->relay_on ? "On" : "Off");
    cell(items[2], sizeof(items[2]), "Current %5s",
         lv->ct_present ? "yes" : "none");
    cell(items[3], sizeof(items[3]), "Back");
    for (int i = 0; i < 4; i++) {
        pitems[i] = items[i];
    }
    line(0, "Control Prog", COL_HEADER);
    draw_items(pitems, 4, COL_FOCUS);
    line(7, "click=edit", COL_FOOTER);
}

static void draw_diag(const bbu_ctrl_t *c, const ui_live_t *lv)
{
    char items[5][32];
    const char *pitems[5];
    cell(items[0], sizeof(items[0]), "Sensors  %4s",
         (lv->tpo.ok && lv->tpu.ok) ? "OK" : "BAD");
    const char *ct = "OK";
    if (c->warn_noct) {
        ct = "none";
    } else if (c->warn_stuck) {
        ct = "stuck";
    } else if (!lv->ct_present) {
        ct = "off";
    }
    cell(items[1], sizeof(items[1]), "Pump CT  %4s", ct);
    cell(items[2], sizeof(items[2]), "Last   %6s", fault_txt(c->last_fault));
    cell(items[3], sizeof(items[3]), "FW   12x16 UI");
    cell(items[4], sizeof(items[4]), "Back");
    for (int i = 0; i < 5; i++) {
        pitems[i] = items[i];
    }
    line(0, "Diagnostics", COL_HEADER);
    draw_items(pitems, 5, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static int nitems(int scr)
{
    switch (scr) {
    case SCR_HOME: return 1;
    case SCR_SEL:  return 6;
    case SCR_TEMP: return 6;
    case SCR_CNT:  return 4;
    case SCR_SYS:  return 8;
    case SCR_PROG: return 4;
    case SCR_DIAG: return 5;
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
        s_top = 0;
        break;
    case SCR_SEL:
        switch (s_cur) {
        case 0: s_scr = SCR_TEMP; s_cur = 0; s_top = 0; break;
        case 1: s_scr = SCR_CNT;  s_cur = 0; s_top = 0; break;
        case 2: s_scr = SCR_SYS;  s_cur = 0; s_top = 0; break;
        case 3: s_scr = SCR_PROG; s_cur = 0; s_top = 0; break;
        case 4: s_scr = SCR_DIAG; s_cur = 0; s_top = 0; break;
        default: go_home(); break;
        }
        break;
    case SCR_TEMP:
        if (s_cur == 5) {
            s_scr = SCR_SEL;
            s_cur = 0;
            s_top = 0;
        }
        break;
    case SCR_SYS:
        if (s_cur == 7) {
            s_scr = SCR_SEL;
            s_cur = 2;
            s_top = 0;
        }
        break;
    case SCR_DIAG:
        if (s_cur == 4) {
            s_scr = SCR_SEL;
            s_cur = 4;
            s_top = 0;
        }
        break;
    case SCR_CNT:
        if (s_cur == 2) {
            bbu_ctrl_clear_stats(c);
        } else if (s_cur == 3) {
            s_scr = SCR_SEL;
            s_cur = 1;
            s_top = 0;
        }
        break;
    case SCR_PROG:
        if (s_cur == 0) {
            next_user_mode(c);
        } else if (s_cur == 1) {
            if (c->user_mode == BBU_MODE_MANUAL ||
                c->user_mode == BBU_MODE_TESTING) {
                bbu_ctrl_manual_relay(c, !c->relay_on);
            }
        } else if (s_cur == 3) {
            s_scr = SCR_SEL;
            s_cur = 3;
            s_top = 0;
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
    tft_text_row(4, " turn/click", COL_FOCUS, COL_BG);
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
        if (s_dirty || live_div >= 200) { /* live values ~1 s */
            live_div = 0;
            bbu_ctrl_t snap;
            ui_live_t lv;
            bbu_params_t p;
            xSemaphoreTake(s_mu, portMAX_DELAY);
            snap = *s_c;
            lv = s_live;
            p = *s_params();
            xSemaphoreGive(s_mu);

            begin_page();
            switch (s_scr) {
            case SCR_HOME: draw_home(&snap, &lv, &p); break;
            case SCR_SEL:  draw_sel(s_cur); break;
            case SCR_TEMP: draw_temp(&lv, &p); break;
            case SCR_CNT:  draw_cnt(&snap); break;
            case SCR_SYS:  draw_sys(&p); break;
            case SCR_PROG: draw_prog(&snap, &lv); break;
            case SCR_DIAG: draw_diag(&snap, &lv); break;
            default: break;
            }
            s_dirty = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
