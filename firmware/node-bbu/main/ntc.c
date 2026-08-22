#include "ntc.h"

#include <math.h>

ntc_sample_t ntc_from_mv(int mv)
{
    ntc_sample_t s = {
        .mv = mv,
        .c = 0.0f,
        .ok = false,
        .rail_fault = false,
        .range_fault = false,
    };

    if (mv <= NTC_RAIL_LO_MV || mv >= NTC_RAIL_HI_MV) {
        s.rail_fault = true;
        return s;
    }

    float v = (float)mv;
    float r = NTC_RDIV_OHM * ((float)NTC_VCC_MV / v - 1.0f);
    if (r <= 0.0f) {
        s.rail_fault = true;
        return s;
    }

    float inv_t = (1.0f / 298.15f) + (logf(r / NTC_R25_OHM) / NTC_BETA);
    s.c = (1.0f / inv_t) - 273.15f;

    if (s.c < NTC_TMIN_C || s.c > NTC_TMAX_C) {
        s.range_fault = true;
        return s;
    }

    s.ok = true;
    return s;
}
