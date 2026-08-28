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

/* Switch: stable-level debounce, then click-on-release / hold-on-long-press.
 * Old edge counter reset on any bounce-high → false click, then the still-held
 * press could reach hold and bounce every submenu straight back to Home. */
#define SW_DEBOUNCE_N  4    /* 20 ms stable at 5 ms poll */
#define SW_CLICK_MIN   2    /* min ticks after stable press before click */
#define SW_HOLD_N      160  /* ~0.8 s after stable press */

enum { SW_IDLE = 0, SW_PRESSED, SW_HELD };

static int s_sw_cand = 1;      /* candidate level being timed */
static int s_sw_cand_n;        /* consecutive samples of s_sw_cand */
static int s_sw_stable = 1;    /* debounced level */
static int s_sw_phase;         /* IDLE / PRESSED / HELD */
static int s_sw_down;          /* ticks while stably pressed (pre-hold) */
static bool s_click;
static bool s_hold;
static int s_sw = 1;

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
    s_sw_cand = s_sw_stable = s_sw =
        gpio_get_level(ENC_PIN_SW) ? 1 : 0;
    s_sw_cand_n = SW_DEBOUNCE_N;
    s_sw_phase = SW_IDLE;
    s_sw_down = 0;
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
}

void enc_poll(void)
{
    int raw = gpio_get_level(ENC_PIN_SW) ? 1 : 0; /* 0 = pressed */

    /* Require SW_DEBOUNCE_N identical samples before accepting a level change. */
    if (raw == s_sw_cand) {
        if (s_sw_cand_n < SW_DEBOUNCE_N) {
            s_sw_cand_n++;
        }
    } else {
        s_sw_cand = raw;
        s_sw_cand_n = 1;
    }

    if (s_sw_cand_n >= SW_DEBOUNCE_N && s_sw_cand != s_sw_stable) {
        int prev = s_sw_stable;
        s_sw_stable = s_sw_cand;
        s_sw = s_sw_stable;

        if (prev == 1 && s_sw_stable == 0) {
            /* stable press edge */
            s_sw_phase = SW_PRESSED;
            s_sw_down = 0;
        } else if (prev == 0 && s_sw_stable == 1) {
            /* stable release edge */
            if (s_sw_phase == SW_PRESSED && s_sw_down >= SW_CLICK_MIN) {
                s_click = true;
            }
            s_sw_phase = SW_IDLE;
            s_sw_down = 0;
        }
    } else {
        s_sw = s_sw_stable;
    }

    /* Hold runs only on a continuous stable press — bounce cannot reset it. */
    if (s_sw_stable == 0 && s_sw_phase == SW_PRESSED) {
        if (s_sw_down < 10000) {
            s_sw_down++;
        }
        if (s_sw_down >= SW_HOLD_N) {
            s_hold = true;
            s_sw_phase = SW_HELD;
        }
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
