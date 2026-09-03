#pragma once

#include <stdbool.h>
#include <stdint.h>

/* v0.08/v0.09: ENC_A GPIO0, ENC_B GPIO1, ENC_SW GPIO5. Internal pull-ups.
 * v0.09 adds 10–100 nF A/B/SW caps to ground. Firmware polls A/B/SW on a
 * 5 ms timer (gray-code table); no GPIO ISR — see docs/gotchas.md. */
#define ENC_PIN_A   0
#define ENC_PIN_B   1
#define ENC_PIN_SW  5

void enc_init(void);          /* 5 ms A/B + switch timer */
void enc_poll(void);          /* also driven by timer; safe to call */
int enc_take_steps(void);     /* detents since last take; signed */
int enc_net(void);            /* running detent total, not cleared */
void enc_levels(int *a, int *b, int *sw);
bool enc_take_click(void);
bool enc_take_hold(void);     /* ~0.8 s stable press; goes home */
uint32_t enc_skipped(void);   /* invalid gray-code samples (bounce / skip) */
