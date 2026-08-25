#include "enc.h"

#include "driver/gpio.h"

static int s_last_a = 1;
static int s_cand_a = 1;
static int s_a_stable;
static int s_steps;
static int s_net;
static int s_sw_down;
static bool s_click;
static bool s_hold;
static bool s_held_sent;
static int s_a;
static int s_b;
static int s_sw;

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
    s_last_a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    s_cand_a = s_last_a;
    s_a = s_last_a;
    s_b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    s_sw = gpio_get_level(ENC_PIN_SW) ? 1 : 0;
}

void enc_poll(void)
{
    int a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    int b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    s_a = a;
    s_b = b;

    /* Debounce A (~10 ms at 5 ms poll), then one step per A edge. */
    if (a != s_cand_a) {
        s_cand_a = a;
        s_a_stable = 0;
    } else if (s_a_stable < 3) {
        s_a_stable++;
        if (s_a_stable == 2 && s_cand_a != s_last_a) {
            if (s_cand_a != b) {
                s_steps++;
                s_net++;
            } else {
                s_steps--;
                s_net--;
            }
            s_last_a = s_cand_a;
        }
    }

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
    int n = s_steps;
    s_steps = 0;
    return n;
}

int enc_net(void)
{
    return s_net;
}

void enc_levels(int *a, int *b, int *sw)
{
    if (a) {
        *a = s_a;
    }
    if (b) {
        *b = s_b;
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
