#include "enc.h"

#include "driver/gpio.h"

static const int8_t k_tab[16] = {
    0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

static uint8_t s_st;
static int s_acc;
static int s_steps;
static int s_sw_down;
static bool s_click;
static bool s_hold;
static bool s_held_sent;

void enc_init(void)
{
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
    s_st = (uint8_t)((a << 1) | b);
}

void enc_poll(void)
{
    int a = gpio_get_level(ENC_PIN_A) ? 1 : 0;
    int b = gpio_get_level(ENC_PIN_B) ? 1 : 0;
    s_st = (uint8_t)(((s_st << 2) | (a << 1) | b) & 0x0F);
    s_acc += k_tab[s_st];
    while (s_acc >= 4) {
        s_acc -= 4;
        s_steps++;
    }
    while (s_acc <= -4) {
        s_acc += 4;
        s_steps--;
    }

    int sw = gpio_get_level(ENC_PIN_SW) ? 1 : 0; /* 0 = pressed */
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
