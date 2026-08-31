/*
 * bracino_log.h — uptime-stamped log line for correlating two bench
 * consoles: "[sec.mmm] message". NOT part of the wire schema; bench/dev
 * helper that rides along in the shared component so node and master
 * include one identical definition.
 */
#pragma once

#include <stdio.h>
#include "esp_timer.h"

#define TLOG(...)                                                             \
    do {                                                                       \
        int64_t _t_us = esp_timer_get_time();                                  \
        printf("[%6lld.%03lld] ", (long long)(_t_us / 1000000LL),              \
               (long long)((_t_us % 1000000LL) / 1000LL));                     \
        printf(__VA_ARGS__);                                                   \
    } while (0)
