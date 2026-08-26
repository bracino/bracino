#include "enc.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

#define TAG "enc"

/*
 * 4-state gray-code table. Invalid bounce edges are 0.
 * Four valid edges per detent on a typical mechanical encoder.
 */
static const int8_t k_quad[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
};

static volatile int32_t s_raw;
static int32_t s_raw_taken;
static int s_net;
static volatile uint8_t s_ab;

static int s_sw_down;
static bool s_click;
static bool s_hold;
static bool s_held_sent;
static int s_sw;

static void IRAM_ATTR enc_isr(void *arg)
{
    (void)arg;
    int a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    int b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    uint8_t now = (uint8_t)((a << 1) | b);
    uint8_t idx = (uint8_t)((s_ab << 2) | now);
    s_ab = now;
    int8_t d = k_quad[idx & 15];
    if (d) {
        s_raw += d;
    }
}

void enc_init(void)
{
    gpio_reset_pin(ENC_PIN_A);
    gpio_reset_pin(ENC_PIN_B);
    gpio_reset_pin(ENC_PIN_SW);
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ENC_PIN_A) | (1ULL << ENC_PIN_B) |
                        (1ULL << ENC_PIN_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    int b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    s_ab = (uint8_t)((a << 1) | b);
    s_sw = gpio_get_level(ENC_PIN_SW) ? 1 : 0;

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr service %s", esp_err_to_name(err));
        return;
    }
    gpio_set_intr_type(ENC_PIN_A, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(ENC_PIN_B, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(ENC_PIN_A, enc_isr, NULL);
    gpio_isr_handler_add(ENC_PIN_B, enc_isr, NULL);
}

void enc_poll(void)
{
    int sw = gpio_get_level(ENC_PIN_SW) ? 1 : 0; /* 0 = pressed */
    s_sw = sw;
    if (sw == 0) {
        if (s_sw_down < 10000) {
            s_sw_down++;
        }
        if (s_sw_down == 160 && !s_held_sent) { /* ~0.8 s at 5 ms */
            s_hold = true;
            s_held_sent = true;
        }
    } else {
        if (s_sw_down >= 6 && s_sw_down < 160 && !s_held_sent) {
            s_click = true;
        }
        s_sw_down = 0;
        s_held_sent = false;
    }
}

int enc_take_steps(void)
{
    int32_t raw = s_raw;
    int32_t delta = raw - s_raw_taken;
    int steps = (int)(delta / 4);
    s_raw_taken += (int32_t)steps * 4;
    s_net += steps;
    return steps;
}

int enc_net(void)
{
    return s_net;
}

void enc_levels(int *a, int *b, int *sw)
{
    if (a) {
        *a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    }
    if (b) {
        *b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    }
    if (sw) {
        *sw = s_sw;
    }
}

bool enc_take_click(void)
{
    bool v = s_click;
    s_click = false;
    return v;
}

bool enc_take_hold(void)
{
    bool v = s_hold;
    s_hold = false;
    return v;
}
