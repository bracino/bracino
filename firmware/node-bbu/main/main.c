/*
 * node-bbu: local BBU pump loop + serial bring-up.
 *
 * Boots MANUAL / coil OFF. `auto` runs DESIGN_NOTE_002 (Auto).
 * A1=TPO, A2=TPU, A3=AMB (print only). β=3950. CT is boolean.
 * Do not put the real BBU pump on this image until 009 is proven.
 *
 * Schematic v0.08: GPIO10 → R1 2k → Q1; GPIO8 → D1 → R7.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "control.h"
#include "comms.h"
#include "ntc.h"
#include "nvs.h"
#include "params.h"
#include "enc.h"
#include "espnow_schema.h"
#include "ui.h"
#include "bracino_log.h"

#define PIN_RELAY          GPIO_NUM_10
#define PIN_HEART          GPIO_NUM_8
#define PIN_I2C_SCL        GPIO_NUM_6
#define PIN_I2C_SDA        GPIO_NUM_7
#define I2C_HZ             100000
#define ADS1115_ADDR       0x48

#define ADS1115_REG_CONV   0x00
#define ADS1115_REG_CONFIG 0x01
#define ADS1115_CFG_SS     0x838B
#define ADS1115_LSB_UV     125

/* CT fitted? ZMCT + snubber dropped for the plant (issue 014): the node's
 * relay only closes a contactor coil circuit, so pump current never crosses
 * node wiring and A0 can never see the pump. 0 = the monitor ignores A0 and
 * reports ct_state=NOT_FITTED; flip to 1 to restore bench sensing. The
 * serial `s0` command still bursts A0 by hand either way. */
#define CT_FITTED          0
#define CT_BURST_N         64
#define CT_ON_RMS_MV       90
#define CMD_LINE_MAX       80
#define ADS_CH_MAX         3

#define LED_IDLE           0
#define LED_RUNNING        1
#define LED_ALERT          2

static i2c_master_dev_handle_t s_ads;
static SemaphoreHandle_t s_mu;
static bbu_ctrl_t s_ctrl;
static volatile int s_led_pat = LED_IDLE;
static bool s_prog;
static bool s_sim_tpo;
static bool s_sim_tpu;
static ntc_sample_t s_fake_tpo;
static ntc_sample_t s_fake_tpu;
static bool s_hw_relay;

#define PARAMS_NS     "bbu"
#define BOOT_KEY      "boot"
#define BOOT_MAGIC    0x32425442u /* 'BTB2' */
#define BOOT_VER      1u

typedef struct {
    uint32_t magic;
    uint32_t ver;
    uint8_t mode;  /* bbu_mode_t user mode (never TESTING) */
    uint8_t relay; /* Manual coil state */
} boot_blob_t;

static void persist_boot_cb(bbu_mode_t user_mode, bool relay_on)
{
    nvs_handle_t h;
    if (nvs_open(PARAMS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    boot_blob_t blob = {
        .magic = BOOT_MAGIC,
        .ver = BOOT_VER,
        .mode = (uint8_t)user_mode,
        .relay = relay_on ? 1u : 0u,
    };
    if (nvs_set_blob(h, BOOT_KEY, &blob, sizeof(blob)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* DN002 boot behavior: restore last-known user mode + Manual coil state.
 * Factory-fresh / invalid blob → Manual/OFF (bbu_ctrl_init default).
 * Auto re-derives IDLE/RUNNING from sensors; the cycle is not persisted. */
static void boot_state_restore(void)
{
    nvs_handle_t h;
    if (nvs_open(PARAMS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    boot_blob_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, BOOT_KEY, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(blob) ||
        blob.magic != BOOT_MAGIC || blob.ver != BOOT_VER) {
        return; /* factory-fresh: Manual / coil OFF */
    }
    if (blob.mode == BBU_MODE_AUTO || blob.mode == BBU_MODE_OFF) {
        bbu_ctrl_request_mode(&s_ctrl, (bbu_mode_t)blob.mode);
        TLOG("boot: restored %s (coil OFF)\n",
               bbu_mode_name(s_ctrl.user_mode));
    } else if (blob.mode == BBU_MODE_MANUAL) {
        if (blob.relay) {
            bbu_ctrl_manual_relay(&s_ctrl, true);
        } else {
            bbu_ctrl_request_mode(&s_ctrl, BBU_MODE_MANUAL);
        }
        TLOG("boot: restored Manual coil=%s\n", blob.relay ? "ON" : "OFF");
    }
    /* blob.mode == TESTING cannot be persisted; anything else → default */
}

static void print_help(void);
static void cmd_comms(char *line); /* defined below; used in handle_line */

static void hw_relay(bool on)
{
    gpio_set_level(PIN_RELAY, on ? 1 : 0);
    s_hw_relay = on;
}

static void apply_ctrl(void)
{
    hw_relay(s_ctrl.relay_on);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        int pat = s_led_pat;
        if (pat == LED_RUNNING) {
            gpio_set_level(PIN_HEART, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else if (pat == LED_ALERT) {
            gpio_set_level(PIN_HEART, 1);
            vTaskDelay(pdMS_TO_TICKS(300));
            gpio_set_level(PIN_HEART, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            gpio_set_level(PIN_HEART, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(PIN_HEART, 0);
            vTaskDelay(pdMS_TO_TICKS(900));
        }
    }
}

static esp_err_t ads_write_u16(uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(s_ads, buf, sizeof(buf), 50);
}

static esp_err_t ads_read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t raw[2];
    esp_err_t err = i2c_master_transmit_receive(s_ads, &reg, 1, raw, 2, 50);
    if (err != ESP_OK) {
        return err;
    }
    *out = ((uint16_t)raw[0] << 8) | raw[1];
    return ESP_OK;
}

static int counts_to_mv(int16_t counts)
{
    return (int)(((int32_t)counts * ADS1115_LSB_UV) / 1000);
}

static esp_err_t ads_read_ch_locked(int ch, int16_t *counts)
{
    if (ch < 0 || ch > ADS_CH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t cfg = (uint16_t)(ADS1115_CFG_SS | ((0x4 + ch) << 12));
    esp_err_t err = ads_write_u16(ADS1115_REG_CONFIG, cfg);
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < 20; i++) {
        uint16_t live;
        err = ads_read_u16(ADS1115_REG_CONFIG, &live);
        if (err != ESP_OK) {
            return err;
        }
        if (live & 0x8000) {
            uint16_t conv;
            err = ads_read_u16(ADS1115_REG_CONV, &conv);
            if (err != ESP_OK) {
                return err;
            }
            *counts = (int16_t)conv;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ads_read_ch(int ch, int16_t *counts)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    esp_err_t err = ads_read_ch_locked(ch, counts);
    xSemaphoreGive(s_mu);
    return err;
}

static bool burst_ch(int ch, int *mid_mv, int *rms_mv, int *pp_mv, int *sat,
                     bool quiet)
{
    int32_t sum = 0;
    int16_t samples[CT_BURST_N];
    int got = 0;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < CT_BURST_N; i++) {
        int16_t c;
        esp_err_t err = ads_read_ch_locked(ch, &c);
        if (err != ESP_OK) {
            xSemaphoreGive(s_mu);
            if (!quiet) {
                TLOG("A%d burst err at %d: %s\n", ch, i, esp_err_to_name(err));
            }
            return false;
        }
        samples[i] = c;
        sum += c;
        got++;
    }
    xSemaphoreGive(s_mu);

    int32_t mean = sum / got;
    double acc = 0.0;
    int16_t lo = samples[0];
    int16_t hi = samples[0];
    for (int i = 0; i < got; i++) {
        int32_t d = (int32_t)samples[i] - mean;
        acc += (double)d * (double)d;
        if (samples[i] < lo) {
            lo = samples[i];
        }
        if (samples[i] > hi) {
            hi = samples[i];
        }
    }
    int rms_c = (int)(sqrt(acc / got) + 0.5);
    *mid_mv = counts_to_mv((int16_t)mean);
    *rms_mv = (int)(((int32_t)rms_c * ADS1115_LSB_UV) / 1000);
    *pp_mv = counts_to_mv((int16_t)(hi - lo));
    *sat = (hi >= 32767 || lo <= -32768) ? 1 : 0;
    return true;
}

static void print_help(void)
{
    TLOG(
        "commands:\n"
        "  on / off / t     relay (forces Manual)\n"
        "  auto / manual / test / halt\n"
        "  r / r0..r3       sample (TPO/TPU/AMB °C+mV; A0 mV)\n"
        "  s / s0..s3       64-sample burst\n"
        "  sim              show overrides\n"
        "  sim tpo 55       inject TPO °C (desk loop proof)\n"
        "  sim tpu 30       inject TPU °C\n"
        "  sim clear        use real NTCs again\n"
        "  comms [on|off]   ESP-NOW client (DN003); 'comms' = status\n"
        "  ident T I        provision node_type/id (NVS)\n"
        "  tel <sec>        telemetry capture period (default 15)\n"
        "  ring <n>         resize FIFO ring EMPTY (bench decimation)\n"
        "  hel <ch> [n]     bench: HELLO burst on fixed channel\n"
        "  prog / st / scan / enc / h\n"
        "GPIO8: idle 100/900, RUNNING steady, alert 300/300.\n"
        "Boots last saved mode (factory: Manual). CT is loaded/not.\n"
        "reboot: soft restart (NVS state restores).\n");
}

static void cmd_scan(i2c_master_bus_handle_t bus)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    TLOG("I2C:");
    int found = 0;
    for (uint16_t a = 0x03; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            printf(" 0x%02X", a);
            found++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    xSemaphoreGive(s_mu);
    if (!found) {
        printf(" (none)");
    }
    TLOG("\n");
}

static void print_ntc(const char *name, ntc_sample_t s)
{
    if (!s.ok) {
        printf("%s=FAULT (%d mV)", name, s.mv);
        return;
    }
    printf("%s=%.1f C (%d mV)", name, (double)s.c, s.mv);
}

static ntc_sample_t live_or_sim(int ch, ntc_sample_t hw)
{
    if (ch == 1 && s_sim_tpo) {
        return s_fake_tpo;
    }
    if (ch == 2 && s_sim_tpu) {
        return s_fake_tpu;
    }
    return hw;
}

static void print_sample(int ch, int16_t c)
{
    int mv = counts_to_mv(c);
    if (ch == 0) {
        TLOG("A0=%d mV counts=%d", mv, (int)c);
        return;
    }
    const char *name = (ch == 1) ? "TPO" : (ch == 2) ? "TPU" : "AMB";
    ntc_sample_t s = live_or_sim(ch, ntc_from_mv(mv));
    print_ntc(name, s);
    if ((ch == 1 && s_sim_tpo) || (ch == 2 && s_sim_tpu)) {
        printf(" [sim]");
    }
}

static void cmd_read_ch(int ch)
{
    int16_t c;
    esp_err_t err = ads_read_ch(ch, &c);
    if (err != ESP_OK) {
        TLOG("A%d err %s\n", ch, esp_err_to_name(err));
        return;
    }
    TLOG("relay=%s  ", s_hw_relay ? "ON" : "OFF");
    print_sample(ch, c);
    TLOG("\n");
}

static void cmd_read_all(void)
{
    TLOG("relay=%s mode=%s", s_hw_relay ? "ON" : "OFF",
           bbu_mode_name(s_ctrl.mode));
    for (int ch = 0; ch <= ADS_CH_MAX; ch++) {
        int16_t c;
        esp_err_t err = ads_read_ch(ch, &c);
        if (err != ESP_OK) {
            TLOG("  A%d err %s\n", ch, esp_err_to_name(err));
            return;
        }
        printf("  ");
        print_sample(ch, c);
    }
    TLOG("\n");
}

static void cmd_burst(int ch)
{
    int mid_mv, rms_mv, pp_mv, sat;
    if (!burst_ch(ch, &mid_mv, &rms_mv, &pp_mv, &sat, false)) {
        return;
    }
    TLOG("relay=%s  A%d  n=%d  mid=%d mV  rms=%d mV  pp=%d mV%s",
           s_hw_relay ? "ON" : "OFF", ch, CT_BURST_N, mid_mv, rms_mv, pp_mv,
           sat ? "  SAT" : "");
    if (ch >= 1) {
        const char *name = (ch == 1) ? "TPO" : (ch == 2) ? "TPU" : "AMB";
        printf("  ");
        print_ntc(name, live_or_sim(ch, ntc_from_mv(mid_mv)));
    }
    TLOG("\n");
}

static void cmd_status(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    TLOG("mode=%s user=%s cycle=%s relay=%s led=%s run=%lus cycle=%lus\n",
           bbu_mode_name(s_ctrl.mode),
           bbu_mode_name(s_ctrl.user_mode),
           bbu_cycle_name(s_ctrl.cycle),
           s_ctrl.relay_on ? "ON" : "OFF",
           s_led_pat == LED_ALERT ? "alert" :
           s_led_pat == LED_RUNNING ? "steady" : "idle",
           (unsigned long)s_ctrl.run_s,
           (unsigned long)s_ctrl.cycle_s);
    if (s_ctrl.warn_stuck) {
        TLOG("WARN stuck-on (CT present while relay OFF)\n");
    }
    if (s_ctrl.warn_maxrun) {
        TLOG("WARN max_run_time_min exceeded\n");
    }
    if (s_ctrl.warn_noct) {
        TLOG("WARN no CT (commanded ON, no current) — loop unchanged\n");
    }
    if (s_sim_tpo || s_sim_tpu) {
        TLOG("sim");
        if (s_sim_tpo) {
            printf(" TPO=%.1f", (double)s_fake_tpo.c);
        }
        if (s_sim_tpu) {
            printf(" TPU=%.1f", (double)s_fake_tpu.c);
        }
        TLOG("\n");
    }
    xSemaphoreGive(s_mu);
    params_print();
}

static void cmd_sim(char *line)
{
    if (strcmp(line, "sim") == 0 || strcmp(line, "sim show") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        TLOG("sim tpo=%s tpu=%s\n",
               s_sim_tpo ? "on" : "off", s_sim_tpu ? "on" : "off");
        if (s_sim_tpo) {
            TLOG("  TPO=%.1f C\n", (double)s_fake_tpo.c);
        }
        if (s_sim_tpu) {
            TLOG("  TPU=%.1f C\n", (double)s_fake_tpu.c);
        }
        xSemaphoreGive(s_mu);
        return;
    }
    if (strcmp(line, "sim clear") == 0 || strcmp(line, "sim off") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_sim_tpo = false;
        s_sim_tpu = false;
        xSemaphoreGive(s_mu);
        TLOG("sim off (real NTCs)\n");
        return;
    }
    char which[8];
    char val[16];
    if (sscanf(line, "sim %7s %15s", which, val) == 2) {
        char *end = NULL;
        float c = strtof(val, &end);
        if (end == val) {
            TLOG("sim tpo|tpu <degC>\n");
            return;
        }
        ntc_sample_t fake = { .mv = 0, .c = c, .ok = true };
        if (c < NTC_TMIN_C || c > NTC_TMAX_C) {
            fake.ok = false;
            fake.range_fault = true;
        }
        if (strcmp(which, "tpo") == 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            s_fake_tpo = fake;
            s_sim_tpo = true;
            xSemaphoreGive(s_mu);
            TLOG("sim TPO=%.1f%s\n", (double)c, fake.ok ? "" : " FAULT");
            return;
        }
        if (strcmp(which, "tpu") == 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            s_fake_tpu = fake;
            s_sim_tpu = true;
            xSemaphoreGive(s_mu);
            TLOG("sim TPU=%.1f%s\n", (double)c, fake.ok ? "" : " FAULT");
            return;
        }
    }
    TLOG("sim | sim tpo N | sim tpu N | sim clear\n");
}

static int parse_ch_suffix(const char *line, char cmd)
{
    if (line[0] != cmd || line[1] < '0' || line[1] > '0' + ADS_CH_MAX || line[2] != '\0') {
        return -1;
    }
    return (int)(line[1] - '0');
}

static bool handle_prog(char *line)
{
    if (strcmp(line, "exit") == 0 || strcmp(line, "q") == 0 || strcmp(line, "prog") == 0) {
        s_prog = false;
        TLOG("programming off\n");
        return true;
    }
    if (strcmp(line, "list") == 0 || strcmp(line, "p") == 0 || line[0] == '\0') {
        params_print();
        return true;
    }
    if (strcmp(line, "default") == 0 || strcmp(line, "defaults") == 0) {
        params_set_defaults();
        TLOG("defaults in RAM (save to persist)\n");
        params_print();
        return true;
    }
    if (strcmp(line, "save") == 0) {
        esp_err_t err = params_save();
        TLOG("save %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }

    char name[32];
    char val[32];
    if (sscanf(line, "%31s %31s", name, val) == 2) {
        if (params_set(name, val)) {
            TLOG("set %s\n", name);
            params_print();
        }
        return true;
    }
    TLOG("prog: list | NAME VALUE | save | default | exit\n");
    return true;
}

static void handle_line(char *line, i2c_master_bus_handle_t bus)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && isspace((unsigned char)line[n - 1])) {
        line[--n] = '\0';
    }
    if (n == 0) {
        if (s_prog) {
            params_print();
        }
        return;
    }
    for (char *p = line; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    if (s_prog) {
        handle_prog(line);
        return;
    }

    int ch;
    if (strcmp(line, "on") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_manual_relay(&s_ctrl, true);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("relay ON  mode=Manual\n");
    } else if (strcmp(line, "off") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_manual_relay(&s_ctrl, false);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("relay OFF  mode=Manual\n");
    } else if (strcmp(line, "t") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_manual_relay(&s_ctrl, !s_ctrl.relay_on);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("relay %s  mode=Manual\n", s_hw_relay ? "ON" : "OFF");
    } else if (strcmp(line, "r") == 0) {
        cmd_read_all();
    } else if ((ch = parse_ch_suffix(line, 'r')) >= 0) {
        cmd_read_ch(ch);
    } else if (strcmp(line, "s") == 0) {
        cmd_burst(0);
    } else if ((ch = parse_ch_suffix(line, 's')) >= 0) {
        cmd_burst(ch);
    } else if (strcmp(line, "enc") == 0) {
        int a = 0, b = 0, sw = 0;
        enc_levels(&a, &b, &sw);
        TLOG("enc A=%d B=%d SW=%d net=%d (SW 0=pressed)\n",
               a, b, sw, enc_net());
    } else if (strcmp(line, "scan") == 0) {
        cmd_scan(bus);
    } else if (strcmp(line, "st") == 0 || strcmp(line, "status") == 0) {
        cmd_status();
    } else if (strcmp(line, "auto") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_request_mode(&s_ctrl, BBU_MODE_AUTO);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("mode Auto (coil OFF, min_off then start if TPO cold)\n");
    } else if (strcmp(line, "manual") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_request_mode(&s_ctrl, BBU_MODE_MANUAL);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("mode Manual\n");
    } else if (strcmp(line, "test") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_request_mode(&s_ctrl, BBU_MODE_TESTING);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("mode Test (15 min then Auto)\n");
    } else if (strcmp(line, "halt") == 0) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_request_mode(&s_ctrl, BBU_MODE_OFF);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        TLOG("mode Off (coil stays OFF until mode changes)\n");
    } else if (strncmp(line, "sim", 3) == 0) {
        cmd_sim(line);
    } else if (strncmp(line, "comms", 5) == 0 ||
               strncmp(line, "ident", 5) == 0 ||
               strncmp(line, "tel ", 4) == 0 ||
               strncmp(line, "ring ", 5) == 0) {
        cmd_comms(line);
    } else if (strncmp(line, "hel ", 4) == 0) {
        unsigned int ch = 0, cnt = 20;
        sscanf(line + 4, "%u %u", &ch, &cnt);
        if (ch >= 1 && ch <= 13 && cnt >= 1 && cnt <= 300) {
            comms_bench_hello_burst((uint8_t)ch, (int)cnt);
        } else {
            TLOG("hel <ch 1-13> [count=20]  (run `comms off` first)\n");
        }
    } else if (strcmp(line, "reboot") == 0) {
        TLOG("rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else if (strcmp(line, "prog") == 0) {
        s_prog = true;
        TLOG("programming on  (list | NAME VALUE | save | default | exit)\n");
        params_print();
    } else if (strcmp(line, "h") == 0 || strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        print_help();
    } else {
        TLOG("unknown '%s'  (h for help)\n", line);
    }
}

static void log_events(uint32_t ev)
{
    if (ev & BBU_EVT_TEST_END) {
        TLOG("Test expired → Auto\n");
    }
    if (ev & BBU_EVT_MODE) {
        TLOG("mode %s\n", bbu_mode_name(s_ctrl.mode));
    }
    if (ev & BBU_EVT_CYCLE) {
        TLOG("%s relay=%s\n",
               bbu_cycle_name(s_ctrl.cycle),
               s_ctrl.relay_on ? "ON" : "OFF");
    }
    if (ev & BBU_EVT_WARN_STUCK) {
        TLOG("WARN stuck-on (CT present, relay OFF)\n");
    }
    if (ev & BBU_EVT_WARN_MAX) {
        TLOG("WARN max_run_time_min exceeded (still running)\n");
    }
    if (ev & BBU_EVT_WARN_NOCT) {
        TLOG("WARN no CT (commanded ON, no current) — loop unchanged\n");
    }
}

/* ---- comms integration (DESIGN_NOTE_003 client, issue 011) ---- */

static uint8_t mode_wire(bbu_mode_t m)
{
    switch (m) {
    case BBU_MODE_MANUAL:   return BBU_MODE_W_MANUAL;
    case BBU_MODE_AUTO:     return BBU_MODE_W_AUTO;
    case BBU_MODE_TPO_ONLY: return BBU_MODE_W_AUTO; /* internal Auto variant */
    case BBU_MODE_TESTING:  return BBU_MODE_W_TEST;
    case BBU_MODE_OFF:
    case BBU_MODE_FAULT:    return BBU_MODE_W_OFF;
    default:                return BBU_MODE_W_OFF;
    }
}

static bool mode_from_wire(uint8_t w, bbu_mode_t *out)
{
    switch (w) {
    case BBU_MODE_W_MANUAL: *out = BBU_MODE_MANUAL; return true;
    case BBU_MODE_W_AUTO:   *out = BBU_MODE_AUTO;   return true;
    case BBU_MODE_W_TEST:   *out = BBU_MODE_TESTING; return true;
    case BBU_MODE_W_OFF:    *out = BBU_MODE_OFF;    return true;
    default:                return false;
    }
}

/* Param ids 8/9/10 live outside params.c — the hooks keep ONE validated
 * setter path: PARAM_SET and the serial UI both land here. */
static bool hook_param_set(uint8_t id, int32_t v)
{
    switch (id) {
    case BBU_PARAM_USER_MODE: {
        bbu_mode_t m;
        if (!mode_from_wire((uint8_t)v, &m)) {
            return false;
        }
        xSemaphoreTake(s_mu, portMAX_DELAY);
        bbu_ctrl_request_mode(&s_ctrl, m);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        return true;
    }
    case BBU_PARAM_MANUAL_RELAY:
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_ctrl.mode != BBU_MODE_MANUAL) {
            bbu_ctrl_request_mode(&s_ctrl, BBU_MODE_MANUAL);
        }
        bbu_ctrl_manual_relay(&s_ctrl, v != 0);
        apply_ctrl();
        xSemaphoreGive(s_mu);
        return true;
    case BBU_PARAM_COMMS_ENABLE:
        comms_enable(v != 0);
        return true;
    default:
        return false;
    }
}

static bool hook_param_get(uint8_t id, int32_t *v)
{
    switch (id) {
    case BBU_PARAM_USER_MODE:
        xSemaphoreTake(s_mu, portMAX_DELAY);
        *v = mode_wire(s_ctrl.user_mode);
        xSemaphoreGive(s_mu);
        return true;
    case BBU_PARAM_MANUAL_RELAY:
        xSemaphoreTake(s_mu, portMAX_DELAY);
        *v = s_ctrl.relay_on ? 1 : 0;
        xSemaphoreGive(s_mu);
        return true;
    case BBU_PARAM_COMMS_ENABLE:
        *v = comms_enabled() ? 1 : 0;
        return true;
    default:
        return false;
    }
}

/* Local (serial/UI) changes propagate as PARAM_CHANGED events (DN003). */
static void param_changed_cb(uint8_t id, int32_t raw)
{
    uint8_t ev[8];
    size_t n = comms_encode_param_value(id, raw, ev + 1);
    if (n == 0) {
        return;
    }
    ev[0] = id;
    ev[1 + n] = PARAM_SRC_LOCAL_UI;
    comms_offer_event(EVENT_PARAM_CHANGED, ev, (uint8_t)(2 + n));
}

static uint8_t ntc_fault_bits(const ntc_sample_t *s, uint8_t open_bit,
                              uint8_t short_bit)
{
    if (s->ok) {
        return 0;
    }
    if (s->range_fault) {
        /* plausible mV but outside -5–110 °C: unusable → OPEN bit */
        return (uint8_t)(1u << open_bit);
    }
    /* rail fault: open sits at the low rail, short at the high rail */
    int mid = (NTC_RAIL_LO_MV + NTC_RAIL_HI_MV) / 2;
    return (uint8_t)(1u << (s->mv < mid ? open_bit : short_bit));
}

static void cmd_comms(char *line)
{
    if (strcmp(line, "comms") == 0) {
        comms_status_print();
        return;
    }
    if (strcmp(line, "comms on") == 0) {
        comms_enable(true);
        return;
    }
    if (strcmp(line, "comms off") == 0) {
        comms_enable(false);
        return;
    }
    unsigned int t, i;
    if (sscanf(line, "ident %u %u", &t, &i) == 2) {
        comms_set_ident((uint8_t)t, (uint8_t)i);
        return;
    }
    unsigned int v;
    if (sscanf(line, "tel %u", &v) == 1) {
        comms_set_sample_period_s(v);
        return;
    }
    if (sscanf(line, "ring %u", &v) == 1) {
        comms_ring_resize((uint16_t)v);
        return;
    }
    TLOG("comms [on|off] | ident <type> <id> | tel <sec> | ring <samples>\n");
}

static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
#if CT_FITTED
        int mid0 = 0, rms0 = 0, pp0 = 0, sat0 = 0;
        bool ct_ok = burst_ch(0, &mid0, &rms0, &pp0, &sat0, true);
        bool ct_on = ct_ok && (rms0 >= CT_ON_RMS_MV);
#else
        bool ct_on = false; /* A0 ignored (014) */
#endif

        int16_t c1 = 0, c2 = 0, c3 = 0;
        ntc_sample_t tpo = { .ok = false, .rail_fault = true };
        ntc_sample_t tpu = { .ok = false, .rail_fault = true };
        ntc_sample_t amb = { .ok = false, .rail_fault = true };
        if (ads_read_ch(1, &c1) == ESP_OK) {
            tpo = ntc_from_mv(counts_to_mv(c1));
        }
        if (ads_read_ch(2, &c2) == ESP_OK) {
            tpu = ntc_from_mv(counts_to_mv(c2));
        }
        if (ads_read_ch(3, &c3) == ESP_OK) {
            amb = ntc_from_mv(counts_to_mv(c3));
        }

        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_sim_tpo) {
            tpo = s_fake_tpo;
        }
        if (s_sim_tpu) {
            tpu = s_fake_tpu;
        }
        bbu_sense_t sense = {
            .tpo = tpo,
            .tpu = tpu,
            .ct_fitted = CT_FITTED,
            .ct_present = ct_on,
            .dt_s = 1,
        };
        uint32_t ev = bbu_ctrl_tick(&s_ctrl, &sense, params_get());
        apply_ctrl();
        ui_live_t live = { .tpo = tpo, .tpu = tpu, .amb = amb,
                           .ct_fitted = CT_FITTED, .ct_present = ct_on };
        ui_set_live(&live);

        /* comms sample + fault-transition events (DN003 EVENT registry) */
        uint8_t faults = ntc_fault_bits(&tpo, BBU_FAULT_TPO_OPEN,
                                        BBU_FAULT_TPO_SHORT) |
                         ntc_fault_bits(&tpu, BBU_FAULT_TPU_OPEN,
                                        BBU_FAULT_TPU_SHORT) |
                         ntc_fault_bits(&amb, BBU_FAULT_AMB_OPEN,
                                        BBU_FAULT_AMB_SHORT);
        static uint8_t s_prev_faults;
        if (faults != s_prev_faults) {
            uint8_t raised = faults & ~s_prev_faults;
            uint8_t cleared = s_prev_faults & ~faults;
            for (uint8_t f = 0; f < BBU_FAULT_COUNT; f++) {
                if (raised & (1u << f)) {
                    comms_offer_event(EVENT_FAULT_RAISED, &f, 1);
                }
                if (cleared & (1u << f)) {
                    comms_offer_event(EVENT_FAULT_CLEARED, &f, 1);
                }
            }
            s_prev_faults = faults;
        }
        comms_sample_t sample = {
            .mode_w = mode_wire(s_ctrl.mode),
            .relay_state = s_ctrl.relay_on ? 1 : 0,
#if CT_FITTED
            .ct_state = s_ctrl.relay_on ? (ct_on ? BBU_CT_STATE_RUNNING
                                                 : BBU_CT_STATE_NO_CURRENT_WARN)
                                        : (ct_on ? BBU_CT_STATE_NO_CURRENT_WARN
                                                 : BBU_CT_STATE_OFF),
#else
            .ct_state = BBU_CT_STATE_NOT_FITTED,
#endif
            .t_tpo_x10 = tpo.ok ? (int16_t)(tpo.c * 10.0f) : -999,
            .t_tpu_x10 = tpu.ok ? (int16_t)(tpu.c * 10.0f) : -999,
            .t_amb_x10 = amb.ok ? (int16_t)(amb.c * 10.0f) : -999,
            .fault_flags = faults,
        };
        comms_offer_sample(&sample);

        bool alert = s_ctrl.warn_stuck || s_ctrl.warn_maxrun ||
                     s_ctrl.warn_noct ||
                     (s_ctrl.mode == BBU_MODE_FAULT) ||
                     (s_ctrl.mode == BBU_MODE_TPO_ONLY) ||
                     !tpo.ok;
        if (alert) {
            s_led_pat = LED_ALERT;
        } else if (s_ctrl.relay_on) {
            s_led_pat = LED_RUNNING;
        } else {
            s_led_pat = LED_IDLE;
        }
        log_events(ev);
        xSemaphoreGive(s_mu);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    gpio_set_level(PIN_RELAY, 0);
    gpio_set_level(PIN_HEART, 0);
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_RELAY) | (1ULL << PIN_HEART),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    hw_relay(false);

    s_mu = xSemaphoreCreateMutex();
    configASSERT(s_mu);
    bbu_ctrl_init(&s_ctrl);

    params_init();
    /* comms needs params (param table) and NVS up; hooks give it the
     * control struct without a compile-time dependency cycle. */
    params_register_ext_setters(hook_param_set, hook_param_get);
    params_register_changed_cb(param_changed_cb);
    bbu_ctrl_register_persist_cb(persist_boot_cb);
    boot_state_restore();
    apply_ctrl();
    comms_init();
    xTaskCreate(heartbeat_task, "heart", 2048, NULL, 1, NULL);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_ads));

    TLOG("\n# node-bbu  mode=%s  loop=DESIGN_NOTE_002  beta=3950  "
           "relay=GPIO%d heart=GPIO%d\n",
           bbu_mode_name(s_ctrl.user_mode),
           (int)PIN_RELAY, (int)PIN_HEART);
    cmd_scan(bus);
    params_print();
    print_help();
    ui_init(s_mu, &s_ctrl, params_get, apply_ctrl);
    xTaskCreate(ui_task, "ui", 8192, NULL, 1, NULL);
    xTaskCreate(monitor_task, "mon", 4096, NULL, 2, NULL);
    printf("> "); fflush(stdout); fsync(fileno(stdout));

    char line[CMD_LINE_MAX];
    size_t len = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            if (len > 0) {
                TLOG("cmd: %s", line);
            }
            handle_line(line, bus);
            len = 0;
            printf(s_prog ? "PROG> " : "> "); fflush(stdout); fsync(fileno(stdout));
            fflush(stdout);
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            /* backspace / delete: erase locally so typing isn't blind */
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
                fsync(fileno(stdout)); /* USB-CDC: push hw FIFO to host */
            }
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
            if (c >= 0x20 && c < 0x7f) {
                putchar(c);
                fflush(stdout);
                /* the USB-CDC FIFO only drains to the host on '\n' or
                 * fsync — without this, typing stays invisible */
                fsync(fileno(stdout));
            }
        }
    }
}
