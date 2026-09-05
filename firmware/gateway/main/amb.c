/* amb.c — exterior ambient NTC on ADC1_CH7 (issue 015, low priority).
 *
 * Divider (bench-proven with an Arduino sketch, human 2026-09-04):
 *
 *     3.33 V (measured rail)
 *       |
 *      NTC (10k-class, beta 3950)
 *       |
 *       +---- GPIO33 (tap -> ADC)
 *       |
 *     9810 ohm (measured)
 *       |
 *      GND
 *
 * Warming raises tap voltage — same topology/polarity as the node NTCs.
 *
 * IMPORTANT anchor: this part measured R_ntc ≈ 8.11 kΩ at 27 °C, ~17 %
 * below a nominal 10 k β3950 spec — the part is NOT behaving as a 10 k
 * reference. So the conversion anchors empirically on the measured pair
 * (T0 = 27.0 °C, R0 = 8100 Ω) and uses beta only for slope. Error away
 * from the anchor is beta-dominated, ±1–2 °C across the outdoor range —
 * accepted for a nice-to-have. TODO(human): ice/boil two-point like the
 * node NTCs got; re-anchor R0/T0 (and possibly beta) from that data.
 *
 * Publishes every 30 s to bracino/gateway/telemetry (DN004 addendum:
 * gateway-local measurements, QoS 0, non-retained). Not node-batch data —
 * no BATCH_ACK/watermark involvement; dedupe downstream is on gw_ts.
 * Faults (open/short) are reported in-band, never a garbage temperature.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "amb.h"
#include "gw.h"
#include "bracino_log.h"

#define PIN_NTC        GPIO_NUM_33
#define V_RAIL         3.33f      /* measured on this board */
#define R_FIXED        9810.0f    /* measured */
#define R0_OHM         8100.0f    /* measured at T0 (see header) */
#define T0_C           27.0f
#define BETA           3950.0f
#define SAMPLE_PERIOD_S 30
#define NUM_SAMPLES    64

/* fault thresholds on tap voltage (NTC high side) */
#define V_SHORT        3.25f      /* tap ~ rail: NTC ~0 ohm (or fixed R open) */
#define V_OPEN         0.05f      /* tap ~ 0: NTC open / broken lead */

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static volatile float s_temp_c;
static char s_fault[8];            /* "" = ok */

const char *amb_fault(void) { return s_fault[0] ? s_fault : NULL; }

float amb_temp_c(void) { return s_temp_c; }

static bool adc_read_avg_mv(int *out_mv)
{
    int raw = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        int r;
        if (adc_oneshot_read(s_unit, ADC_CHANNEL_7, &r) != ESP_OK) {
            return false;
        }
        raw += r;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    raw /= NUM_SAMPLES;

    if (s_cali) {
        int mv;
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            *out_mv = mv;
            return true;
        }
    }
    /* no eFuse calibration on this board: rail-referenced fallback */
    *out_mv = (int)((float)raw * V_RAIL / 4095.0f * 1000.0f);
    return true;
}

static void convert(int mv, float *temp_c, const char **fault)
{
    float v = mv / 1000.0f;
    *fault = NULL;
    if (v >= V_SHORT) {
        *fault = "SHORT";
        return;
    }
    if (v <= V_OPEN) {
        *fault = "OPEN";
        return;
    }
    float r_ntc = R_FIXED * (V_RAIL - v) / v;
    float t_k = 1.0f / (1.0f / (T0_C + 273.15f) +
                        logf(r_ntc / R0_OHM) / BETA);
    *temp_c = t_k - 273.15f;
}

static void amb_task(void *arg)
{
    (void)arg;
    int mv = 0;
    for (;;) {
        if (adc_read_avg_mv(&mv)) {
            const char *f = NULL;
            float t = 0.0f;
            convert(mv, &t, &f);
            if (f) {
                strncpy(s_fault, f, sizeof(s_fault) - 1);
            } else {
                s_fault[0] = '\0';
                s_temp_c = t;
            }

            if (gw_time_valid() && gw_broker_up()) {
                char ts[32], json[160];
                time_t sec = time(NULL);
                struct tm tm;
                gmtime_r(&sec, &tm);
                strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
                snprintf(json, sizeof(json),
                         "{\"t_amb\":%.1f,\"fault\":%s,\"gw_ts\":\"%s\"}",
                         (double)s_temp_c,
                         f ? (f[0] == 'O' ? "\"OPEN\"" : "\"SHORT\"")
                           : "null",
                         ts);
                gw_mqtt_publish("bracino/gateway/telemetry", json, 0, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_S * 1000));
    }
}

void amb_start(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_unit));

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_unit, ADC_CHANNEL_7,
                                               &chan_cfg));

    /* eFuse two-point calibration if this chip has it; NULL otherwise
     * and the rail-referenced fallback applies */
    if (adc_cali_create_scheme_line_fitting(
            &(adc_cali_line_fitting_config_t){
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            },
            &s_cali) != ESP_OK) {
        s_cali = NULL;
        TLOG("amb: no adc line-fitting calibration — rail-referenced\n");
    }

    s_fault[0] = '\0';
    s_temp_c = 0.0f;
    xTaskCreate(amb_task, "amb", 8192, NULL, 1, NULL);
}
