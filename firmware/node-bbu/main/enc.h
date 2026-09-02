#pragma once

#include <stdbool.h>
#include <stdint.h>

/* v0.08/v0.09: ENC_A GPIO0, ENC_B GPIO1, ENC_SW GPIO5. Internal pull-ups.
 * v0.09 adds 10–100 nF A/B caps to ground (bounce); firmware adds the
 * C3 IO-MUX per-pad glitch filter + a raw (framework-free) ISR — see
 * bench WDT postmortem in docs/gotchas.md. */
#define ENC_PIN_A   0
#define ENC_PIN_B   1
#define ENC_PIN_SW  5
#define ENC_EDGE_MASK ((1u << ENC_PIN_A) | (1u << ENC_PIN_B))

void enc_init(void);          /* A/B ISR + 5 ms switch timer */
void enc_poll(void);          /* also driven by timer; safe to call */
int enc_take_steps(void);     /* detents since last take; signed */
int enc_net(void);            /* running detent total, not cleared */
void enc_levels(int *a, int *b, int *sw);
bool enc_take_click(void);
bool enc_take_hold(void);     /* ~0.8 s stable press; goes home */
uint32_t enc_isr_suppressed(void); /* noise edges dropped by ISR rate cap */
