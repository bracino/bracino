/* Host test for control.c — no IDF.  gcc -I../main -o test_control test_control.c ../main/control.c ../main/ntc.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "ntc.h"
#include "params.h"

static int g_fail;
static bbu_mode_t g_cb_mode;
static bool g_cb_relay;
static int g_cb_calls;

static void persist_cb(bbu_mode_t m, bool relay)
{
    g_cb_mode = m;
    g_cb_relay = relay;
    g_cb_calls++;
}

static void expect(int ok, const char *msg)
{
    if (!ok) {
        printf("FAIL %s\n", msg);
        g_fail++;
    }
}

static ntc_sample_t ok_c(float c)
{
    ntc_sample_t s = { .mv = 1760, .c = c, .ok = true };
    return s;
}

static ntc_sample_t bad_rail(void)
{
    ntc_sample_t s = { .mv = 13, .c = 0, .ok = false, .rail_fault = true };
    return s;
}

static bbu_params_t defaults(void)
{
    bbu_params_t p;
    p.tpo_setpoint_c = 60.0f;
    p.hysteresis_c = 3.0f;
    p.min_on_time_s = 180;
    p.min_off_time_s = 60;
    p.ct_confirm_s = 10;
    p.min_tpo_tpu_delta_c = 5.0f;
    p.max_run_time_min = 60;
    return p;
}

static void tick_n(bbu_ctrl_t *c, bbu_sense_t s, const bbu_params_t *p, int n)
{
    s.dt_s = 1;
    for (int i = 0; i < n; i++) {
        bbu_ctrl_tick(c, &s, p);
    }
}

int main(void)
{
    bbu_params_t p = defaults();
    bbu_ctrl_t c;
    bbu_sense_t s;

    /* NTC sanity: 1760 mV ≈ 28 °C at β=3950 */
    ntc_sample_t n = ntc_from_mv(1760);
    expect(n.ok && n.c > 27.0f && n.c < 29.0f, "ntc 1760mV ~ 28C");
    expect(!ntc_from_mv(13).ok, "ntc open is fault");
    expect(!ntc_from_mv(3283).ok, "ntc short is fault");
    n = ntc_from_mv(770);
    expect(n.ok && n.c > -0.5f && n.c < 1.5f, "ntc 770mV ~ ice");
    n = ntc_from_mv(3083);
    expect(n.ok && n.c > 98.0f && n.c < 102.0f, "ntc 3083mV ~ boil");

    bbu_ctrl_init(&c);
    expect(c.mode == BBU_MODE_MANUAL && !c.relay_on, "boot MANUAL off");

    /* auto + cold TPO: wait min_off then RUNNING */
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);
    s.tpo = ok_c(50.0f);
    s.tpu = ok_c(30.0f);
    s.ct_present = false;
    tick_n(&c, s, &p, 59);
    expect(c.cycle == BBU_CYCLE_IDLE && !c.relay_on, "still IDLE during min_off");
    tick_n(&c, s, &p, 2);
    expect(c.cycle == BBU_CYCLE_RUNNING && c.relay_on, "start when TPO cold");

    /* hot top, cold bottom: stay RUNNING (would be a short cycle if TPO-only) */
    s.tpo = ok_c(70.0f);
    s.tpu = ok_c(30.0f);
    s.ct_present = true;
    tick_n(&c, s, &p, 200);
    expect(c.cycle == BBU_CYCLE_RUNNING, "stay RUNNING until TPU rises");

    /* thermocline collapsed */
    s.tpu = ok_c(66.0f);
    tick_n(&c, s, &p, 2);
    expect(c.cycle == BBU_CYCLE_IDLE && !c.relay_on, "stop when charged");

    /* TPO fault → FAULT, pump off */
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);
    s.tpo = ok_c(50.0f);
    s.tpu = ok_c(30.0f);
    s.ct_present = true;
    tick_n(&c, s, &p, 61);
    expect(c.relay_on, "running before TPO fail");
    s.tpo = bad_rail();
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_FAULT && !c.relay_on, "TPO fail → FAULT off");

    /* TPO recovers → Auto (not Manual) */
    s.tpo = ok_c(50.0f);
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_AUTO, "TPO recover → Auto");

    /* TPU fail while RUNNING → TPO_ONLY, keep pumping */
    bbu_ctrl_init(&c);
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);
    s.tpo = ok_c(50.0f);
    s.tpu = ok_c(30.0f);
    s.ct_present = true;
    tick_n(&c, s, &p, 61);
    s.tpu = bad_rail();
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_TPO_ONLY && c.relay_on, "TPU fail → TPO_ONLY keep ON");
    s.tpu = ok_c(30.0f);
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_AUTO && c.relay_on, "TPU recover keeps RUNNING");

    /* no CT after confirm → warn, stay Auto (old box was blind to current) */
    bbu_ctrl_init(&c);
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);
    s.tpo = ok_c(50.0f);
    s.tpu = ok_c(30.0f);
    s.ct_present = false;
    tick_n(&c, s, &p, 61);
    expect(c.cycle == BBU_CYCLE_RUNNING, "started");
    tick_n(&c, s, &p, 12);
    expect(c.mode == BBU_MODE_AUTO && c.warn_noct, "no CT → warn, stay Auto");
    tick_n(&c, s, &p, 5);
    expect(c.mode == BBU_MODE_AUTO && c.warn_noct, "no flap while CT still missing");
    s.ct_present = true;
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_AUTO && !c.warn_noct, "CT back clears no-CT warn");

    /* TPO_ONLY stop does not need TPU delta */
    bbu_ctrl_init(&c);
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);
    s.tpo = ok_c(50.0f);
    s.tpu = bad_rail();
    s.ct_present = true;
    tick_n(&c, s, &p, 61);
    expect(c.mode == BBU_MODE_TPO_ONLY && c.relay_on, "TPO_ONLY running");
    s.tpo = ok_c(70.0f);
    tick_n(&c, s, &p, 200);
    expect(c.cycle == BBU_CYCLE_IDLE, "TPO_ONLY stops on TPO only");

    /* MANUAL ignores cold TPO */
    bbu_ctrl_init(&c);
    s.tpo = ok_c(20.0f);
    s.tpu = ok_c(20.0f);
    s.ct_present = false;
    tick_n(&c, s, &p, 120);
    expect(c.mode == BBU_MODE_MANUAL && !c.relay_on, "MANUAL does not auto-start");

    /* inversion slack */
    bbu_ctrl_init(&c);
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);
    s.tpo = ok_c(60.0f);
    s.tpu = ok_c(60.4f);
    s.ct_present = true;
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_AUTO, "0.4 C invert is slack");
    s.tpu = ok_c(62.0f);
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_TPO_ONLY, "TPU > TPO+1 is severe");

    /* stuck-on warning, no mode change */
    bbu_ctrl_init(&c);
    s.tpo = ok_c(70.0f);
    s.tpu = ok_c(70.0f);
    s.ct_present = true;
    tick_n(&c, s, &p, 3);
    expect(c.warn_stuck && c.mode == BBU_MODE_MANUAL && !c.relay_on,
           "stuck-on warns, stays MANUAL off");

    /* Off: cold TPO must not start */
    bbu_ctrl_init(&c);
    bbu_ctrl_request_mode(&c, BBU_MODE_OFF);
    s.tpo = ok_c(20.0f);
    s.tpu = ok_c(20.0f);
    s.ct_present = false;
    tick_n(&c, s, &p, 120);
    expect(c.mode == BBU_MODE_OFF && !c.relay_on, "Off does not auto-start");

    /* Off survives TPO fault and recover */
    s.tpo = bad_rail();
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_FAULT && c.user_mode == BBU_MODE_OFF && !c.relay_on,
           "Off + bad TPO → Fault overlay, user still Off");
    s.tpo = ok_c(20.0f);
    tick_n(&c, s, &p, 1);
    expect(c.mode == BBU_MODE_OFF && !c.relay_on, "TPO recover stays Off");

    /* DN002 boot persistence: human paths fire the cb, Auto loop does not */
    bbu_ctrl_init(&c);
    bbu_ctrl_register_persist_cb(persist_cb);
    g_cb_calls = 0;
    bbu_ctrl_request_mode(&c, BBU_MODE_AUTO);   /* already boot default? no: init=Manual */
    expect(g_cb_calls == 1 && g_cb_mode == BBU_MODE_AUTO && !g_cb_relay,
           "request_mode(AUTO) persists once");
    s.tpo = ok_c(50.0f);
    s.tpu = ok_c(30.0f);
    tick_n(&c, s, &p, 61);
    expect(c.cycle == BBU_CYCLE_RUNNING && g_cb_calls == 1,
           "Auto loop start does NOT persist");
    s.tpo = ok_c(70.0f);
    s.tpu = ok_c(30.0f);
    tick_n(&c, s, &p, 200); /* burn min_on_time while hot-top/cold-bottom */
    expect(c.cycle == BBU_CYCLE_RUNNING && g_cb_calls == 1,
           "Auto loop still RUNNING, no persist");
    s.tpu = ok_c(66.0f);
    tick_n(&c, s, &p, 2);
    expect(c.cycle == BBU_CYCLE_IDLE && g_cb_calls == 1,
           "Auto loop stop does NOT persist");
    bbu_ctrl_manual_relay(&c, true);
    expect(g_cb_calls == 2 && g_cb_mode == BBU_MODE_MANUAL && g_cb_relay,
           "manual coil ON persists (Manual/ON)");
    bbu_ctrl_manual_relay(&c, false);
    expect(g_cb_calls == 3 && g_cb_mode == BBU_MODE_MANUAL && !g_cb_relay,
           "manual coil OFF persists (Manual/OFF)");
    bbu_ctrl_register_persist_cb(NULL);

    /* TESTING is never persisted */
    bbu_ctrl_init(&c);
    bbu_ctrl_register_persist_cb(persist_cb);
    g_cb_calls = 0;
    bbu_ctrl_request_mode(&c, BBU_MODE_TESTING);
    expect(g_cb_calls == 0, "TESTING never fires persist cb");
    bbu_ctrl_manual_relay(&c, true);
    expect(g_cb_calls == 0 && c.mode == BBU_MODE_TESTING,
           "coil toggle in TESTING not persisted");
    tick_n(&c, s, &p, 15 * 60 + 2); /* Test expiry → Auto internally */
    expect(c.user_mode == BBU_MODE_AUTO && g_cb_calls == 0,
           "Test expiry (internal Auto) does NOT persist");
    /* reboot semantics: fresh init then restore-Auto is the caller's job;
     * here just verify a later Off persists */
    bbu_ctrl_request_mode(&c, BBU_MODE_OFF);
    expect(g_cb_calls == 1 && g_cb_mode == BBU_MODE_OFF,
           "Off persists");
    bbu_ctrl_register_persist_cb(NULL);

    if (g_fail) {
        printf("%d failed\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
