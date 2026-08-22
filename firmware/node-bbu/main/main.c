/*
 * node-bbu breadboard bring-up: relay toggle + ADS1115 A0–A3 + GPIO8 LED.
 *
 * Bench only. No control loop, no network. Serial commands drive the
 * contact so a CT pot can be set against known AC loads, and sample
 * the three NTC dividers. GPIO8 blinks so the MCU is visibly alive
 * on buck power with USB unplugged.
 *
 * Schematic v0.08 (BOM/netlist): GPIO10 → R1 2k → Q1 base; Q1 C = module IN.
 * GPIO10 high = coil ON. Boot holds GPIO10 low (coil OFF).
 * GPIO8 → D1 → R7 2.2k → GND (heartbeat).
 * Do not put the real BBU pump on this sketch.
 *
 *   GPIO10  RELAY (via Q1)
 *   GPIO8   heartbeat LED (to GND through ~330 Ω–1 kΩ)
 *   GPIO6   ADS1115 SCL
 *   GPIO7   ADS1115 SDA
 *   ADS1115 A0 = ZMCT103C OUT
 *   ADS1115 A1–A3 = TH1/R4, TH2/R5, TH3/R6
 *   ADS1115 ADDR pulled low on module → I2C 0x48
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_RELAY          GPIO_NUM_10
#define PIN_HEART          GPIO_NUM_8
#define PIN_I2C_SCL        GPIO_NUM_6
#define PIN_I2C_SDA        GPIO_NUM_7
#define I2C_HZ             100000
#define ADS1115_ADDR       0x48
#define HEART_PERIOD_MS    500

#define ADS1115_REG_CONV   0x00
#define ADS1115_REG_CONFIG 0x01

/* OS=1, PGA ±4.096 V, single-shot, 860 SPS, comparator off; MUX in 14:12. */
#define ADS1115_CFG_SS     0x838B
#define ADS1115_LSB_UV     125   /* ±4.096 V / 32768 */
#define ADS1115_FSR_MV     4096

#define CT_BURST_N         64    /* ~75 ms of 50 Hz at 860 SPS */
#define CMD_LINE_MAX       64
#define ADS_CH_MAX         3

static i2c_master_dev_handle_t s_ads;
static bool s_relay_on;

static void relay_set(bool on)
{
    /* Q1: GPIO10 high sinks module IN (active-low). On-board WS2812 unused. */
    gpio_set_level(PIN_RELAY, on ? 1 : 0);
    s_relay_on = on;
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    bool on = false;
    for (;;) {
        on = !on;
        gpio_set_level(PIN_HEART, on ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(HEART_PERIOD_MS));
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

static esp_err_t ads_read_ch(int ch, int16_t *counts)
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

static int counts_to_mv(int16_t counts)
{
    return (int)(((int32_t)counts * ADS1115_LSB_UV) / 1000);
}

static void print_help(void)
{
    printf(
        "commands:\n"
        "  on          relay ON  (GPIO10 high)\n"
        "  off         relay OFF (GPIO10 low)   [default at boot]\n"
        "  t            toggle relay\n"
        "  r            one sample A0–A3 (mV + counts)\n"
        "  r0..r3       one sample that channel\n"
        "  s            64-sample A0 burst: mid / rms / p-p  (CT pot)\n"
        "  s0..s3       64-sample burst on that channel\n"
        "  scan         I2C probe 0x03..0x77\n"
        "  h            this help\n"
        "GPIO8 ~1 Hz = firmware alive (USB not required).\n"
        "CT pot: start with no AC through the core (rms ~ 0). Add a known load,\n"
        "then turn the pot so p-p stays well inside ±%d mV at the intended current.\n"
        "A1–A3 mid is the NTC divider tap (DC); rms is leftover noise.\n",
        ADS1115_FSR_MV);
}

static void cmd_scan(i2c_master_bus_handle_t bus)
{
    printf("I2C:");
    int found = 0;
    for (uint16_t a = 0x03; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            printf(" 0x%02X", a);
            found++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!found) {
        printf(" (none)");
    }
    printf("\n");
}

static void print_sample(int ch, int16_t c)
{
    printf("A%d=%d mV counts=%d", ch, counts_to_mv(c), (int)c);
}

static void cmd_read_ch(int ch)
{
    int16_t c;
    esp_err_t err = ads_read_ch(ch, &c);
    if (err != ESP_OK) {
        printf("A%d err %s\n", ch, esp_err_to_name(err));
        return;
    }
    printf("relay=%s  ", s_relay_on ? "ON" : "OFF");
    print_sample(ch, c);
    printf("\n");
}

static void cmd_read_all(void)
{
    printf("relay=%s", s_relay_on ? "ON" : "OFF");
    for (int ch = 0; ch <= ADS_CH_MAX; ch++) {
        int16_t c;
        esp_err_t err = ads_read_ch(ch, &c);
        if (err != ESP_OK) {
            printf("  A%d err %s\n", ch, esp_err_to_name(err));
            return;
        }
        printf("  ");
        print_sample(ch, c);
    }
    printf("\n");
}

static void cmd_burst(int ch)
{
    int32_t sum = 0;
    int16_t samples[CT_BURST_N];
    int got = 0;

    for (int i = 0; i < CT_BURST_N; i++) {
        int16_t c;
        esp_err_t err = ads_read_ch(ch, &c);
        if (err != ESP_OK) {
            printf("A%d burst err at %d: %s\n", ch, i, esp_err_to_name(err));
            return;
        }
        samples[i] = c;
        sum += c;
        got++;
    }

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
    int mid_mv = counts_to_mv((int16_t)mean);
    int rms_mv = (int)(((int32_t)rms_c * ADS1115_LSB_UV) / 1000);
    int pp_mv = counts_to_mv((int16_t)(hi - lo));
    int sat = (hi >= 32767 || lo <= -32768) ? 1 : 0;

    printf("relay=%s  A%d  n=%d  mid=%d mV  rms=%d mV  pp=%d mV%s\n",
           s_relay_on ? "ON" : "OFF", ch, got, mid_mv, rms_mv, pp_mv,
           sat ? "  SAT" : "");
}

static int parse_ch_suffix(const char *line, char cmd)
{
    if (line[0] != cmd || line[1] < '0' || line[1] > '0' + ADS_CH_MAX || line[2] != '\0') {
        return -1;
    }
    return (int)(line[1] - '0');
}

static void handle_line(char *line, i2c_master_bus_handle_t bus)
{
    /* trim */
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && isspace((unsigned char)line[n - 1])) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }
    for (char *p = line; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    int ch;
    if (strcmp(line, "on") == 0) {
        relay_set(true);
        printf("relay ON\n");
    } else if (strcmp(line, "off") == 0) {
        relay_set(false);
        printf("relay OFF\n");
    } else if (strcmp(line, "t") == 0) {
        relay_set(!s_relay_on);
        printf("relay %s\n", s_relay_on ? "ON" : "OFF");
    } else if (strcmp(line, "r") == 0) {
        cmd_read_all();
    } else if ((ch = parse_ch_suffix(line, 'r')) >= 0) {
        cmd_read_ch(ch);
    } else if (strcmp(line, "s") == 0) {
        cmd_burst(0);
    } else if ((ch = parse_ch_suffix(line, 's')) >= 0) {
        cmd_burst(ch);
    } else if (strcmp(line, "scan") == 0) {
        cmd_scan(bus);
    } else if (strcmp(line, "h") == 0 || strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        print_help();
    } else {
        printf("unknown '%s'  (h for help)\n", line);
    }
}

void app_main(void)
{
    /* Latch relay low before the pad becomes an output so Q1 stays off. */
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
    relay_set(false);
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

    printf("\n# node-bbu bring-up  relay=GPIO%d (Q1, high=ON)  heart=GPIO%d  I2C SDA=%d SCL=%d  ADS=0x%02X A0-A3\n",
           (int)PIN_RELAY, (int)PIN_HEART, (int)PIN_I2C_SDA, (int)PIN_I2C_SCL, ADS1115_ADDR);
    cmd_scan(bus);
    print_help();
    printf("> ");
    fflush(stdout);

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
            handle_line(line, bus);
            len = 0;
            printf("> ");
            fflush(stdout);
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
}
