#pragma once

#include <stdbool.h>
#include <stdint.h>

/* v0.08: ENC_A GPIO0, ENC_B GPIO1, ENC_SW GPIO5. Internal pull-ups. */
#define ENC_PIN_A   0
#define ENC_PIN_B   1
#define ENC_PIN_SW  5

void enc_init(void);
void enc_poll(void);          /* call every few ms */
int enc_take_steps(void);     /* detents since last take; signed */
bool enc_take_click(void);
bool enc_take_hold(void);     /* ~0.8 s press; goes home */
