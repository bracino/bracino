/* amb.h — gateway-local exterior ambient temperature (issue 015).
 *
 * Hardware: 10 kΩ-class NTC from 3.33 V rail to tap, 9.81 kΩ to GND,
 * tap on GPIO33 (ADC1_CH7 — ADC1, since ADC2 cannot sample while WiFi
 * is up and the gateway is always on WiFi).
 */
#pragma once

void amb_start(void);   /* sampling + publish task */

/* latest reading for the status page; NULL-safe before first sample */
const char *amb_fault(void);    /* NULL = ok, else "OPEN"/"SHORT" */
float amb_temp_c(void);         /* valid only when amb_fault() == NULL */
