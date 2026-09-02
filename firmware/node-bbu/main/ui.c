/*
 * ui.c — TFT + encoder menus (issue 010; field-image pass 012).
 *
 * 012: Home shows comms status; Temperatures gained AMB and dropped the
 * programmables (Hyst/dT moved to Control Programming); Counters shows
 * FIFO depth; Control Programming is the full DN003 param table with a
 * single validated setter (params_set_by_id — NVS autosave for core
 * params, hooks for mode/relay/comms); Diagnostics carries the comms
 * counter block; System Data shows the UTC build stamp (BRACINO_BUILD).
 */
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "comms.h"
#include "enc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "tft.h"

#ifndef BRACINO_BUILD
#define BRACINO_BUILD "dev"
#endif
#ifndef BRACINO_BUILD_NAME
#define BRACINO_BUILD_NAME "dev"
#endif

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
static int s_edit = -1;   /* SCR_PROG: param-table index being edited */
static int32_t s_edit_raw; /* shadow value while editing (commit on click) */
static bool s_rbt_arm;     /* SCR_DIAG: reboot row armed (click twice) */

/* Quick rows above the DN003 table (pre-012 behavior kept for habit): */
#define PROG_QUICK_ROWS 2
#define PROG_ROW_MODE   0
#define PROG_ROW_PUMP   1
static bool s_dirty = true;
static char s_cache[TFT_ROWS][TFT_COLS + 1];
static uint16_t s_cache_fg[TFT_ROWS];

static void go_home(void)
{
    s_scr = SCR_HOME;
    s_cur = 0;
    s_top = 0;
    s_edit = -1;
    s_rbt_arm = false;
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

/* Comms link summary for the Home line (012): off / SCANNING / OK. */
static void comms_link_txt(char *dst, size_t n)
{
    comms_ui_t cm;
    comms_ui_snapshot(&cm);
    if (!cm.enabled) {
        cell(dst, n, "Comms  off");
        return;
    }
    if (cm.link_ok) {
        cell(dst, n, "Comms  OK");
    } else {
        cell(dst, n, "Comms  SCANNING");
    }
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
         lv->ct_fitted ? (lv->ct_present ? "CT" : "none") : "n/f");
    line(3, l, COL_FG);

    cell(l, sizeof(l), "Set    %4.0f C", (double)p->tpo_setpoint_c);
    line(4, l, COL_FG);

    char cl[32];
    comms_link_txt(cl, sizeof(cl));
    line(5, cl, comms_enabled() ? COL_OK : COL_DIM);

    line(6, "> Menu", COL_FOCUS);

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

/* 012: AMB added; Hyst and dT are programmables — they live in Control
 * Programming now, not in the sensor reading list. */
static void draw_temp(const ui_live_t *lv, const bbu_params_t *p)
{
    char items[5][32];
    const char *pitems[5];
    char t[12];
    fmt_temp(t, sizeof(t), &lv->tpo);
    cell(items[0], sizeof(items[0]), "TPO   %s", t);
    fmt_temp(t, sizeof(t), &lv->tpu);
    cell(items[1], sizeof(items[1]), "TPU   %s", t);
    fmt_temp(t, sizeof(t), &lv->amb);
    cell(items[2], sizeof(items[2]), "AMB   %s", t);
    cell(items[3], sizeof(items[3]), "Setpt  %5.1fC",
         (double)p->tpo_setpoint_c);
    cell(items[4], sizeof(items[4]), "Back");
    for (int i = 0; i < 5; i++) {
        pitems[i] = items[i];
    }
    line(0, "Temperatures", COL_HEADER);
    draw_items(pitems, 5, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static void draw_cnt(const bbu_ctrl_t *c)
{
    char items[6][32];
    const char *pitems[6];
    comms_ui_t cm;
    comms_ui_snapshot(&cm);

    /* uptime since boot: DD:HH:MM */
    int64_t up_s = esp_timer_get_time() / 1000000;
    cell(items[0], sizeof(items[0]), "Up    %02ld:%02lu:%02lu",
         (long)(up_s / 86400), (unsigned long)((up_s / 3600) % 24),
         (unsigned long)((up_s / 60) % 60));
    /* persistent across reboots (NVS, saved on pump stop / clear) */
    cell(items[1], sizeof(items[1]), "Pump  %02lu:%02lu",
         (unsigned long)(c->total_run_s / 3600u),
         (unsigned long)((c->total_run_s / 60u) % 60u));
    cell(items[2], sizeof(items[2]), "Starts   %5lu",
         (unsigned long)c->starts);
    /* >1 means buffering (no comms / gateway down) — normal is 0 or 1 */
    cell(items[3], sizeof(items[3]), "FIFO  %4u/%u", cm.fifo, cm.fifo_cap);
    cell(items[4], sizeof(items[4]), "Clear counts");
    cell(items[5], sizeof(items[5]), "Back");
    for (int i = 0; i < 6; i++) {
        pitems[i] = items[i];
    }
    line(0, "Counters", COL_HEADER);
    draw_items(pitems, 6, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

static void draw_sys(const bbu_params_t *p)
{
    char items[10][32];
    const char *pitems[10];
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
    cell(items[7], sizeof(items[0]), "FW    " BRACINO_BUILD_NAME);
    cell(items[8], sizeof(items[0]), "UTC " BRACINO_BUILD);
    cell(items[9], sizeof(items[0]), "Back");
    for (int i = 0; i < 10; i++) {
        pitems[i] = items[i];
    }
    line(0, "System Data", COL_HEADER);
    draw_items(pitems, 10, COL_FOCUS);
    line(7, "click=ok", COL_FOOTER);
}

/* ---- Control Programming: the DN003 param table (012) ---- */

/* Abbreviated display names, parallel to params_table() order. The TFT is
 * 20 cols; full DN003 names (min_tpo_tpu_delta_c = 19 chars) leave no room
 * for the value. Keep each ≤ 7 chars; order MUST match params_table(). */
static const char *const k_short[] = {
    "setpt", "hyst", "min on", "min off", "ct wt",
    "dT min", "maxrun", "mode", "pump", "comms",
};

static const char *prog_short(int idx, int n)
{
    if (idx >= 0 && idx < n && idx < (int)(sizeof(k_short) / sizeof(k_short[0]))) {
        return k_short[idx];
    }
    return "?";
}

static const char *enum_name(uint8_t id, int32_t v)
{
    if (id == BBU_PARAM_USER_MODE) {
        switch (v) { /* wire encoding: BBU_MODE_W_* */
        case 0:  return "Manual";
        case 1:  return "Auto";
        case 2:  return "Test";
        case 3:  return "Off";
        default: return "?";
        }
    }
    return v ? "On" : "Off";
}

static void param_value_txt(char *dst, size_t n, const bbu_param_desc_t *d,
                            int32_t raw, bool editing)
{
    const char *mark = editing ? ">" : " ";
    switch (d->type) {
    case PTYPE_I16_X10:
        cell(dst, n, "%s%5.1f%s", mark, raw / 10.0, editing ? "<" : " ");
        break;
    case PTYPE_ENUM:
        cell(dst, n, "%s%5s%s", mark, enum_name(d->id, raw), editing ? "<" : " ");
        break;
    default: /* PTYPE_U32 */
        cell(dst, n, "%s%5lu%s", mark, (unsigned long)raw, editing ? "<" : " ");
        break;
    }
}

static int prog_count(void)
{
    int n = 0;
    params_table(&n);
    return n + 1 + PROG_QUICK_ROWS; /* params + Back + quick rows */
}

static void draw_prog(const bbu_ctrl_t *c)
{
    int n;
    const bbu_param_desc_t *tab = params_table(&n);
    char items[13][32];
    const char *pitems[13];

    cell(items[0], sizeof(items[0]), "Mode %8s",
         bbu_mode_name(c->user_mode));
    cell(items[1], sizeof(items[1]), "Pump %8s",
         c->relay_on ? "On" : "Off");
    for (int i = 0; i < n; i++) {
        char val[10];
        const bbu_param_desc_t *d = &tab[i];
        int32_t raw = 0;
        if (i == s_edit) {
            raw = s_edit_raw;      /* live preview of the pending value */
        } else {
            params_get_raw_by_id(d->id, &raw);
        }
        /* short name (≤7) + value with >…< edit hint: fits 20 cols */
        param_value_txt(val, sizeof(val), d, raw, i == s_edit);
        cell(items[i + PROG_QUICK_ROWS], sizeof(items[0]), "%-9s %8s",
             prog_short(i, n), val);
    }
    cell(items[n + PROG_QUICK_ROWS], sizeof(items[0]), "Back");
    int total = n + 1 + PROG_QUICK_ROWS;
    for (int i = 0; i < total; i++) {
        pitems[i] = items[i];
    }
    line(0, (s_edit >= 0) ? "Control Prog*" : "Control Prog",
         (s_edit >= 0) ? COL_EDIT : COL_HEADER);
    draw_items(pitems, total, s_edit >= 0 ? COL_EDIT : COL_FOCUS);
    line(7, (s_edit >= 0) ? "click=save hold=cancel" : "click=edit",
         (s_edit >= 0) ? COL_EDIT : COL_FOOTER);
}

/* Edit state for the focused param: a shadow value is stepped and shown
 * live; the COMMIT happens on click via the single validated setter
 * (params_set_by_id — core ids NVS-autosave, mode/relay/comms route
 * through the hooks). Hold leaves the screen, abandoning the edit. */
static void prog_edit_begin(int idx)
{
    int n;
    const bbu_param_desc_t *tab = params_table(&n);
    if (idx < 0 || idx >= n) {
        return;
    }
    int32_t raw = 0;
    if (!params_get_raw_by_id(tab[idx].id, &raw)) {
        return;
    }
    s_edit = idx;
    s_edit_raw = raw;
}

static void prog_edit_step(int steps)
{
    int n;
    const bbu_param_desc_t *tab = params_table(&n);
    if (s_edit < 0 || s_edit >= n) {
        return;
    }
    const bbu_param_desc_t *d = &tab[s_edit];
    s_edit_raw += (int32_t)steps * d->step;
    if (s_edit_raw < d->min) {
        s_edit_raw = d->min;
    }
    if (s_edit_raw > d->max) {
        s_edit_raw = d->max;
    }
}

static void prog_edit_commit(void)
{
    int n;
    const bbu_param_desc_t *tab = params_table(&n);
    if (s_edit < 0 || s_edit >= n) {
        s_edit = -1;
        return;
    }
    params_set_by_id(tab[s_edit].id, s_edit_raw);
    s_edit = -1;
}

static void next_user_mode(bbu_ctrl_t *c)
{
    static const bbu_mode_t k_user_modes[] = {
        BBU_MODE_AUTO, BBU_MODE_MANUAL, BBU_MODE_TESTING, BBU_MODE_OFF
    };
    int i = 0;
    for (; i < 4; i++) {
        if (k_user_modes[i] == c->user_mode) {
            break;
        }
    }
    i = (i + 1) % 4;
    bbu_ctrl_request_mode(c, k_user_modes[i]);
}

static void draw_diag(const bbu_ctrl_t *c, const ui_live_t *lv)
{
    /* 17 rows max (3 sensor + comms status + 11 comms + Reboot + Back).
     * The [17] is load-bearing: 16 overflowed the stack and rebooted the
     * node on screen entry (bench 2026-09-02). */
    char items[17][32];
    const char *pitems[17];
    comms_ui_t cm;
    comms_ui_snapshot(&cm);
    int i = 0;

    cell(items[i++], sizeof(items[0]), "Sensors  %4s",
         (lv->tpo.ok && lv->tpu.ok) ? "OK" : "BAD");
    const char *ct = "OK";
    if (!lv->ct_fitted) {
        ct = "n/f";
    } else if (c->warn_noct) {
        ct = "none";
    } else if (c->warn_stuck) {
        ct = "stuck";
    } else if (!lv->ct_present) {
        ct = "off";
    }
    cell(items[i++], sizeof(items[0]), "Pump CT  %4s", ct);
    cell(items[i++], sizeof(items[0]), "Last   %6s", fault_txt(c->last_fault));
    /* comms diagnostics (012) */
    cell(items[i++], sizeof(items[0]), "Comms   %4s",
         !cm.enabled ? "off" : (cm.link_ok ? "OK" : "SCAN"));
    if (cm.enabled) {
        cell(items[i++], sizeof(items[0]), "Ch   %2u  bnd %c",
             cm.channel, cm.bound ? 'Y' : 'N');
        cell(items[i++], sizeof(items[0]), "GW   %02X%02X%02X",
             cm.gw[3], cm.gw[4], cm.gw[5]);
        cell(items[i++], sizeof(items[0]), "Epc  %8lu",
               (unsigned long)cm.epoch_s);
        cell(items[i++], sizeof(items[0]), "FIFO %3u/%u", cm.fifo, cm.fifo_cap);
        cell(items[i++], sizeof(items[0]), "Fail %6lu",
               (unsigned long)cm.fails);
        cell(items[i++], sizeof(items[0]), "RX   %6lu", (unsigned long)cm.rx);
        cell(items[i++], sizeof(items[0]), "TXok %6lu", (unsigned long)cm.tx_ok);
        cell(items[i++], sizeof(items[0]), "TXfl %6lu", (unsigned long)cm.tx_fail);
        cell(items[i++], sizeof(items[0]), "RTX  %6lu",
               (unsigned long)cm.retrans);
        cell(items[i++], sizeof(items[0]), "Dec  %6lu",
               (unsigned long)cm.decim);
        cell(items[i++], sizeof(items[0]), "Ev   %6lu",
               (unsigned long)cm.ev_sent);
    }
    cell(items[i++], sizeof(items[0]), s_rbt_arm ? "Reboot? click" : "Reboot");
    cell(items[i++], sizeof(items[0]), "Back");
    for (int j = 0; j < i; j++) {
        pitems[j] = items[j];
    }
    line(0, "Diagnostics", COL_HEADER);
    draw_items(pitems, i, COL_FOCUS);
    line(7, s_rbt_arm ? "click=REBOOT" : "click=ok",
         s_rbt_arm ? COL_BAD : COL_FOOTER);
    (void)c;
}

static int nitems(int scr)
{
    switch (scr) {
    case SCR_HOME: return 1;
    case SCR_SEL:  return 6;
    case SCR_TEMP: return 5;
    case SCR_CNT:  return 6;
    case SCR_SYS:  return 10;
    case SCR_PROG: return prog_count();
    case SCR_DIAG: {
        comms_ui_t cm;
        comms_ui_snapshot(&cm);
        /* 3 sensor rows + comms status + Reboot + Back; enabled adds 11
         * comms rows (ch, gw, epoch, fifo, fails, rx, txok, txfl, rtx, dec, ev) */
        return cm.enabled ? 17 : 6;
    }
    default:       return 1;
    }
}

static void on_click_prog(bbu_ctrl_t *c)
{
    int n;
    params_table(&n);
    int back = n + PROG_QUICK_ROWS;
    if (s_edit < 0) {
        if (s_cur == PROG_ROW_MODE) {
            next_user_mode(c);         /* quick toggle: cycle mode */
            return;
        }
        if (s_cur == PROG_ROW_PUMP) {
            if (c->user_mode == BBU_MODE_MANUAL ||
                c->user_mode == BBU_MODE_TESTING) {
                bbu_ctrl_manual_relay(c, !c->relay_on);
            }
            return;
        }
        if (s_cur == back) {           /* Back */
            s_scr = SCR_SEL;
            s_cur = 3;
            s_top = 0;
            return;
        }
        prog_edit_begin(s_cur - PROG_QUICK_ROWS); /* table index */
        return;
    }
    if (s_edit == s_cur - PROG_QUICK_ROWS) {
        prog_edit_commit();            /* click again = save */
        return;
    }
    prog_edit_commit();            /* moving to another row: save first */
    prog_edit_begin(s_cur - PROG_QUICK_ROWS);
}

static void on_click(bbu_ctrl_t *c)
{
    bool was_rbt = s_rbt_arm;
    s_rbt_arm = false;             /* any other click disarms reboot */
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
        if (s_cur == 4) {
            s_scr = SCR_SEL;
            s_cur = 0;
            s_top = 0;
        }
        break;
    case SCR_SYS:
        if (s_cur == 9) {
            s_scr = SCR_SEL;
            s_cur = 2;
            s_top = 0;
        }
        break;
    case SCR_DIAG:
        if (s_cur == nitems(SCR_DIAG) - 2) {
            if (was_rbt) {
                /* Two-click confirm done: full restart through the
                 * normal boot path (NVS mode/params/identity restore).
                 * Relay GPIO is driven low before restore logic runs. */
                esp_restart();
            }
            s_rbt_arm = true;      /* first click: arm */
            break;
        }
        if (s_cur == nitems(SCR_DIAG) - 1) {
            s_scr = SCR_SEL;
            s_cur = 4;
            s_top = 0;
        }
        break;
    case SCR_CNT:
        if (s_cur == 4) {
            bbu_ctrl_clear_stats(c);
        } else if (s_cur == 5) {
            s_scr = SCR_SEL;
            s_cur = 1;
            s_top = 0;
        }
        break;
    case SCR_PROG:
        on_click_prog(c);
        break;
    default:
        go_home();
        break;
    }
    s_dirty = true;
}

/* Encoder turn while editing a param: step the shadow value (range-checked
 * against the descriptor); commit happens on click. */
static void on_steps(int steps)
{
    if (s_scr == SCR_PROG && s_edit >= 0 && steps != 0) {
        prog_edit_step(steps);
        s_dirty = true;
        return;
    }
    s_cur += steps;
    clip_cur(nitems(s_scr));
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

static bool scr_needs_live(int scr)
{
    /* Static menus only redraw on input; live temps/state ~1 Hz. */
    switch (scr) {
    case SCR_HOME:
    case SCR_TEMP:
    case SCR_CNT:
    case SCR_PROG:
    case SCR_DIAG:
        return true;
    default:
        return false;
    }
}

void ui_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    s_dirty = true;
    int live_div = 0;
    for (;;) {
        /* Switch is polled by enc's 5 ms timer; just consume events here. */
        int steps = enc_take_steps();
        bool click = enc_take_click();
        bool hold = enc_take_hold();

        if (hold) {
            go_home();
        } else if (steps || click) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            if (steps) {
                on_steps(steps);
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
        bool live = scr_needs_live(s_scr) && live_div >= 200; /* ~1 s */
        if (s_dirty || live) {
            live_div = 0;
            bbu_ctrl_t snap;
            ui_live_t lv;
            bbu_params_t p;
            int scr;
            xSemaphoreTake(s_mu, portMAX_DELAY);
            snap = *s_c;
            lv = s_live;
            p = *s_params();
            scr = s_scr;
            xSemaphoreGive(s_mu);

            begin_page();
            switch (scr) {
            case SCR_HOME: draw_home(&snap, &lv, &p); break;
            case SCR_SEL:  draw_sel(s_cur); break;
            case SCR_TEMP: draw_temp(&lv, &p); break;
            case SCR_CNT:  draw_cnt(&snap); break;
            case SCR_SYS:  draw_sys(&p); break;
            case SCR_PROG: draw_prog(&snap); break;
            case SCR_DIAG: draw_diag(&snap, &lv); break;
            default: break;
            }
            s_dirty = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
