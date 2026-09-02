#include "enc.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

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

/* Switch timing is wall-clock so a slow TFT redraw cannot starve hold/click. */
#define SW_POLL_US       5000LL
#define SW_DEBOUNCE_N    3          /* 15 ms stable */
#define SW_CLICK_MIN_US  25000LL    /* ignore taps shorter than this */
#define SW_HOLD_US       800000LL   /* long-press → home */

enum { SW_IDLE = 0, SW_PRESSED, SW_HELD };

static volatile int32_t s_raw;
static int32_t s_raw_taken;
static int s_net;
static volatile uint8_t s_ab;
static volatile uint32_t s_isr_supp; /* edges dropped by the storm limiter */

/* ISR rate cap: mechanical rotation is ≤40 edges/s per pin (4 edges/detent);
 * a bouncing/floating wire is kHz+. Without this cap an ISR storm starves
 * IDLE (TWDT fires, everything frozen) — seen on the bench 2026-09-02.
 * 1 ms min edge spacing absorbs contact bounce and costs nothing. */
#define ENC_MIN_EDGE_US 1000LL
static volatile int64_t s_last_edge_us;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int s_sw_cand = 1;
static int s_sw_cand_n;
static int s_sw_stable = 1;
static int s_sw_phase;
static int64_t s_press_us;
static bool s_click;
static bool s_hold;
static int s_sw = 1;

static esp_timer_handle_t s_timer;

static void IRAM_ATTR enc_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_last_edge_us < ENC_MIN_EDGE_US) {
        s_isr_supp++;
        return; /* bounce/noise storm: drop the edge */
    }
    s_last_edge_us = now;
    int a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    int b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    uint8_t level = (uint8_t)((a << 1) | b);
    uint8_t idx = (uint8_t)((s_ab << 2) | level);
    s_ab = level;
    int8_t d = k_quad[idx & 15];
    if (d) {
        s_raw += d;
    }
}

void enc_poll(void)
{
    int raw = gpio_get_level(ENC_PIN_SW) ? 1 : 0; /* 0 = pressed */
    int64_t now = esp_timer_get_time();

    if (raw == s_sw_cand) {
        if (s_sw_cand_n < SW_DEBOUNCE_N) {
            s_sw_cand_n++;
        }
    } else {
        s_sw_cand = raw;
        s_sw_cand_n = 1;
    }

    portENTER_CRITICAL(&s_mux);

    if (s_sw_cand_n >= SW_DEBOUNCE_N && s_sw_cand != s_sw_stable) {
        int prev = s_sw_stable;
        s_sw_stable = s_sw_cand;
        s_sw = s_sw_stable;

        if (prev == 1 && s_sw_stable == 0) {
            s_sw_phase = SW_PRESSED;
            s_press_us = now;
        } else if (prev == 0 && s_sw_stable == 1) {
            if (s_sw_phase == SW_PRESSED &&
                (now - s_press_us) >= SW_CLICK_MIN_US) {
                s_click = true;
            }
            s_sw_phase = SW_IDLE;
        }
    } else {
        s_sw = s_sw_stable;
    }

    if (s_sw_stable == 0 && s_sw_phase == SW_PRESSED &&
        (now - s_press_us) >= SW_HOLD_US) {
        s_hold = true;
        s_sw_phase = SW_HELD;
    }

    portEXIT_CRITICAL(&s_mux);
}

static void enc_timer_cb(void *arg)
{
    (void)arg;
    enc_poll();
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
    s_sw_cand = s_sw_stable = s_sw =
        gpio_get_level(ENC_PIN_SW) ? 1 : 0;
    s_sw_cand_n = SW_DEBOUNCE_N;
    s_sw_phase = SW_IDLE;
    s_press_us = 0;
    s_click = false;
    s_hold = false;

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr service %s", esp_err_to_name(err));
        return;
    }
    gpio_set_intr_type(ENC_PIN_A, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(ENC_PIN_B, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(ENC_PIN_A, enc_isr, NULL);
    gpio_isr_handler_add(ENC_PIN_B, enc_isr, NULL);

    /* Independent of UI/TFT: never miss press/release during a redraw. */
    const esp_timer_create_args_t targs = {
        .callback = &enc_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "enc_sw",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer %s", esp_err_to_name(err));
        return;
    }
    err = esp_timer_start_periodic(s_timer, SW_POLL_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer start %s", esp_err_to_name(err));
    }
}

int enc_take_steps(void)
{
    /* Ignore turn while the switch is down — click, don't scroll. */
    if (s_sw_stable == 0) {
        s_raw_taken = s_raw;
        return 0;
    }
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
        portENTER_CRITICAL(&s_mux);
        *sw = s_sw;
        portEXIT_CRITICAL(&s_mux);
    }
}

bool enc_take_click(void)
{
    bool v;
    portENTER_CRITICAL(&s_mux);
    v = s_click;
    s_click = false;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

bool enc_take_hold(void)
{
    bool v;
    portENTER_CRITICAL(&s_mux);
    v = s_hold;
    s_hold = false;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

uint32_t enc_isr_suppressed(void)
{
    return s_isr_supp;
}
