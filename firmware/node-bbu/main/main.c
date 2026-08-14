/*
 * node-bbu breadboard bring-up: relay toggle + ADS1115 A0 (ZMCT103C).
 *
 * Bench only. No control loop, no network. Relay starts OFF (GPIO10 high;
 * JQC-3FE-S-Z module is low-level trigger). Serial commands drive the
 * contact so a CT pot can be set against known AC loads.
 *
 * Schematic v0.06 (bbu_controller_prototype_kicad):
 *   GPIO10  RELAY IN   (active-low)
 *   GPIO6   ADS1115 SCL
 *   GPIO7   ADS1115 SDA
 *   ADS1115 A0 = ZMCT103C OUT
 *   ADS1115 ADDR pulled low on module → I2C 0x48
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "bringup"

#define PIN_RELAY          GPIO_NUM_10
#define PIN_I2C_SCL        GPIO_NUM_6
#define PIN_I2C_SDA        GPIO_NUM_7
#define I2C_HZ             100000
#define ADS1115_ADDR       0x48

#define ADS1115_REG_CONV   0x00
#define ADS1115_REG_CONFIG 0x01

/* AIN0 vs GND, FSR ±4.096 V, 860 SPS, single-shot, comparator off. */
#define ADS1115_CFG_A0_SS  0xC38B
#define ADS1115_LSB_UV     125   /* ±4.096 V / 32768 */
#define ADS1115_FSR_MV     4096

#define CT_BURST_N         64    /* ~75 ms of 50 Hz at 860 SPS */
#define CMD_LINE_MAX       64

static i2c_master_dev_handle_t s_ads;
static bool s_relay_on;

static void relay_set(bool on)
{
    /* Module IN is active-low. GPIO10 also drives the unused on-board WS2812. */
    gpio_set_level(PIN_RELAY, on ? 0 : 1);
    s_relay_on = on;
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

static esp_err_t ads_read_a0(int16_t *counts)
{
    esp_err_t err = ads_write_u16(ADS1115_REG_CONFIG, ADS1115_CFG_A0_SS);
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < 20; i++) {
        uint16_t cfg;
        err = ads_read_u16(ADS1115_REG_CONFIG, &cfg);
        if (err != ESP_OK) {
            return err;
        }
        if (cfg & 0x8000) {
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
        "  on          relay ON  (GPIO10 low)\n"
        "  off         relay OFF (GPIO10 high)  [default at boot]\n"
        "  t            toggle relay\n"
        "  r            one A0 sample (mV + counts)\n"
        "  s            64-sample A0 burst: mid / rms / p-p  (use this for the pot)\n"
        "  scan         I2C probe 0x03..0x77\n"
        "  h            this help\n"
        "CT pot: start with no AC through the core (rms ~ 0). Add a known load,\n"
        "then turn the pot so p-p stays well inside ±%d mV at the intended current.\n",
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

static void cmd_read_once(void)
{
    int16_t c;
    esp_err_t err = ads_read_a0(&c);
    if (err != ESP_OK) {
        printf("A0 err %s\n", esp_err_to_name(err));
        return;
    }
    printf("relay=%s  A0=%d mV  counts=%d\n",
           s_relay_on ? "ON" : "OFF", counts_to_mv(c), (int)c);
}

static void cmd_burst(void)
{
    int32_t sum = 0;
    int16_t samples[CT_BURST_N];
    int got = 0;

    for (int i = 0; i < CT_BURST_N; i++) {
        int16_t c;
        esp_err_t err = ads_read_a0(&c);
        if (err != ESP_OK) {
            printf("A0 burst err at %d: %s\n", i, esp_err_to_name(err));
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

    printf("relay=%s  n=%d  mid=%d mV  rms=%d mV  pp=%d mV%s\n",
           s_relay_on ? "ON" : "OFF", got, mid_mv, rms_mv, pp_mv,
           sat ? "  SAT" : "");
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
        cmd_read_once();
    } else if (strcmp(line, "s") == 0) {
        cmd_burst();
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
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_RELAY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    relay_set(false);

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

    printf("\n# node-bbu bring-up  relay=GPIO%d (active-low)  I2C SDA=%d SCL=%d  ADS=0x%02X A0\n",
           (int)PIN_RELAY, (int)PIN_I2C_SDA, (int)PIN_I2C_SCL, ADS1115_ADDR);
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
