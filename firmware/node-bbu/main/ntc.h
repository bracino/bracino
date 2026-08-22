#pragma once

#include <stdbool.h>

/* NRBE 10 kΩ, β=3950 assumed (2026-08-22). 10 kΩ to GND, NTC to +3.3 V. */
#define NTC_BETA       3950.0f
#define NTC_R25_OHM    10000.0f
#define NTC_RDIV_OHM   10000.0f
#define NTC_VCC_MV     3300
#define NTC_RAIL_LO_MV 50
#define NTC_RAIL_HI_MV 3200
#define NTC_TMIN_C     0.0f
#define NTC_TMAX_C     95.0f

typedef struct {
    int mv;
    float c;
    bool ok;          /* converted and inside 0–95 °C */
    bool rail_fault;  /* open / short rails */
    bool range_fault; /* converted but outside 0–95 °C */
} ntc_sample_t;

ntc_sample_t ntc_from_mv(int mv);
