/* softap.c — maintenance SoftAP, button-invoked (DN004 rule 7 + addenda).
 *
 * GPIO27 button: external pull-up, pressed = LOW. Long-press ≥3 s opens;
 * another long-press or the 10-minute timer closes. The AP is an overlay,
 * not a gateway state: AP+STA share the radio channel by construction, so
 * on the provisioned network the AP coexists with ACTIVE and ESP-NOW keeps
 * flowing. Pre-provisioning, the AP pins its own channel while ESP-NOW is
 * already off (no WiFi → no ESP-NOW) — it pauses nothing that wasn't
 * already paused.
 *
 * Pages (embedded C strings, no filesystem):
 *   GET  /      status — the LED table as text (diagnose 4/5-blink here)
 *   GET  /prov  provisioning form (WiFi SSID/pass, broker host/port)
 *   POST /save  write NVS, respond, reboot (deterministic re-apply)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "amb.h"

#include "gw.h"
#include "amb.h"
#include "bracino_log.h"

#define PIN_BUTTON     GPIO_NUM_27 /* pull-up; pressed = LOW */
#define LONG_PRESS_MS  3000
#define AP_WINDOW_MS   600000      /* 10 min */
#define AP_SSID        "bracino-gateway01"
#define AP_PASS        "bracinoAdmin"

static httpd_handle_t s_httpd;
static uint32_t s_opened_ms;
static bool s_ap_open;

/* ---- tiny HTML helpers (C strings, no filesystem) ---- */

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width'>"
    "<title>bracino-gateway</title>"
    "<style>body{font-family:monospace;margin:1.5em}"
    "table{border-collapse:collapse}td,th{border:1px solid #999;"
    "padding:2px 8px;text-align:left}code{background:#eee;padding:0 4px}"
    "input{font-family:monospace}</style></head><body>";

static const char PAGE_TAIL[] = "</body></html>";

static void http_send(httpd_req_t *req, const char *head, const char *body,
                      const char *tail)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, head, strlen(head));
    if (body) {
        httpd_resp_send_chunk(req, body, strlen(body));
    }
    httpd_resp_send_chunk(req, tail, strlen(tail));
    httpd_resp_send_chunk(req, NULL, 0);
}

static void esc(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 6 < outlen; i++) {
        char c = in[i];
        if (c == '<') {
            out[o++] = '&'; out[o++] = 'l'; out[o++] = 't'; out[o++] = ';';
        } else if (c == '>') {
            out[o++] = '&'; out[o++] = 'g'; out[o++] = 't'; out[o++] = ';';
        } else if (c == '&') {
            out[o++] = '&'; out[o++] = 'a'; out[o++] = 'm'; out[o++] = 'p';
            out[o++] = ';';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* ---- status page: the LED table as text ---- */

static void append_status(char *b, size_t cap)
{
    static const char *PAT[] = {
        "SOLID — all clear (flicker = frames flowing)",
        "1 blink — no WiFi (STA not associated)",
        "2 blinks — WiFi but no broker (MQTT down)",
        "3 blinks — no valid time (no NTP, no MQTT time-set)",
        "4 blinks — no acks (broker up, commit watermarks stale)",
        "5 blinks — no node seen (liveness timeout)",
    };
    char ssid_e[72], broker[72];
    esc(gw_net_ssid(), ssid_e, sizeof(ssid_e));
    gw_net_broker_str(broker, sizeof(broker));

    int pat = 5;
    if (!gw_wifi_up()) {
        pat = 1;
    } else if (!gw_broker_up()) {
        pat = 2;
    } else if (!gw_time_valid()) {
        pat = 3;
    } else if (gw_health_age_ms() > 90000) {
        pat = 4;
    } else {
        bool seen = false;
        for (int i = 0; i < gw_node_cnt; i++) {
            if (gw_nodes[i].in_use &&
                gw_now_ms() - gw_nodes[i].last_seen_ms < 60000) {
                seen = true;
            }
        }
        if (!seen) {
            pat = 5;
        }
    }

    char amb[40];
    if (amb_fault()) {
        snprintf(amb, sizeof(amb), "FAULT %s", amb_fault());
    } else {
        snprintf(amb, sizeof(amb), "%.1f C", (double)amb_temp_c());
    }

    snprintf(b + strlen(b), cap - strlen(b),
             "<h2>gateway %s</h2><p>LED: <b>%s</b></p>"
             "<table>"
             "<tr><th>wifi</th><td>%s (ssid '%s')</td></tr>"
             "<tr><th>broker</th><td>%s</td></tr>"
             "<tr><th>time</th><td>%s (src %s)</td></tr>"
             "<tr><th>backend health</th><td>%s</td></tr>"
             "<tr><th>ext. ambient</th><td>%s</td></tr>"
             "<tr><th>mode</th><td>%s</td></tr>"
             "<tr><th>uptime</th><td>%lu s</td></tr>"
             "</table><h3>nodes</h3><table><tr><th>role</th><th>mac</th>"
             "<th>last seen</th><th>anchor</th></tr>",
             PAT[pat], PAT[pat],
             gw_wifi_up() ? "up" : "DOWN", ssid_e,
             gw_broker_up() ? "connected" : broker,
             gw_time_valid() ? "valid" : "INVALID", gw_time_source(),
             gw_health_age_ms() == UINT32_MAX
                 ? "never seen"
                 : (gw_health_age_ms() < 90000 ? "fresh" : "STALE"),
             amb,
             gw_mode == GW_ACTIVE ? "ACTIVE" : "WAIT_BACKEND",
             (unsigned long)(gw_now_ms() / 1000));
    for (int i = 0; i < gw_node_cnt; i++) {
        gw_node_t *n = &gw_nodes[i];
        char anchor[32] = "?";
        if (n->anchor.have && n->anchor.epoch_total_ms) {
            time_t s = (time_t)(n->anchor.epoch_total_ms / 1000);
            struct tm tm;
            gmtime_r(&s, &tm);
            strftime(anchor, sizeof(anchor), "%Y-%m-%d %H:%M:%S", &tm);
        }
        snprintf(b + strlen(b), cap - strlen(b),
                 "<tr><td>(%u,%u)</td><td>" MACSTR "</td>"
                 "<td>%lu s ago</td><td>%s</td></tr>",
                 n->type, n->id, MAC2STR(n->mac),
                 n->last_seen_ms
                     ? (unsigned long)((gw_now_ms() - n->last_seen_ms) / 1000)
                     : 0UL,
                 anchor);
    }
    snprintf(b + strlen(b), cap - strlen(b),
             "</table><h3>counters</h3><p>tx_ok=%lu tx_fail=%lu "
             "acks=%lu held=%lu samples=%lu</p>"
             "<p><a href='/prov'>provisioning</a></p>",
             (unsigned long)gw_ct.tx_ok, (unsigned long)gw_ct.tx_fail,
             (unsigned long)gw_ct.acks_sent, (unsigned long)gw_ct.acks_held,
             (unsigned long)gw_ct.samples_published);
}

static esp_err_t h_status(httpd_req_t *req)
{
    static char body[8192];
    body[0] = '\0';
    append_status(body, sizeof(body));
    http_send(req, PAGE_HEAD, body, PAGE_TAIL);
    return ESP_OK;
}

/* ---- provisioning page ---- */

static const char PROV_FORM[] =
    "<h2>provisioning</h2>"
    "<form method='post' action='/save'>"
    "<table>"
    "<tr><th>WiFi SSID</th><td><input name='ssid' value='%s'></td></tr>"
    "<tr><th>WiFi password</th><td><input name='pass' type='password'></td></tr>"
    "<tr><th>Broker host</th><td><input name='bhost' value='%s'></td></tr>"
    "<tr><th>Broker port</th><td><input name='bport' value='%s'></td></tr>"
    "</table><p><input type='submit' value='save + reboot'></p>"
    "</form><p>Saving reboots the gateway. The node keeps buffering "
    "until the gateway returns (DN003).</p>";

static esp_err_t h_prov(httpd_req_t *req)
{
    static char body[1024];
    char ssid_e[72], bh_e[72], bp[8];
    char bh[72];
    gw_net_broker_str(bh, sizeof(bh));
    esc(gw_net_ssid(), ssid_e, sizeof(ssid_e));
    esc(bh, bh_e, sizeof(bh_e));
    snprintf(bp, sizeof(bp), "%lu",
             (unsigned long)gw_nvs_get_u32("bport", 1883));
    snprintf(body, sizeof(body), PROV_FORM, ssid_e, bh_e, bp);
    http_send(req, PAGE_HEAD, body, PAGE_TAIL);
    return ESP_OK;
}

/* ---- POST /save ---- */

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

static bool form_field(char *q, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    while (*q) {
        char *amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg > klen && strncmp(q, key, klen) == 0 && q[klen] == '=') {
            size_t vlen = seg - klen - 1;
            if (vlen >= outlen) {
                vlen = outlen - 1;
            }
            memcpy(out, q + klen + 1, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return true;
        }
        q += seg;
        if (*q == '&') {
            q++;
        }
    }
    return false;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500)); /* let the response flush */
    esp_restart();
}

static esp_err_t h_save(httpd_req_t *req)
{
    static char body[512];
    char ssid[33], pass[65], bhost[64], bport[8];

    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_OK;
    }
    int recvd = httpd_req_recv(req, body, len);
    if (recvd <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
        return ESP_OK;
    }
    body[recvd] = '\0';

    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "bhost", bhost, sizeof(bhost));
    form_field(body, "bport", bport, sizeof(bport));

    if (ssid[0]) {
        gw_nvs_set_str("ssid", ssid);
    }
    if (pass[0]) {
        gw_nvs_set_str("pass", pass);
    }
    if (bhost[0]) {
        gw_nvs_set_str("bhost", bhost);
    }
    if (bport[0]) {
        gw_nvs_set_u32("bport", (uint32_t)atoi(bport));
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s<p>Saved. Rebooting to apply…</p>", PAGE_HEAD);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, msg, strlen(msg));

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 3, NULL);
    return ESP_OK;
}

/* ---- AP open/close ---- */

bool gw_softap_open(void)
{
    if (s_ap_open) {
        return true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = { 0 };
    strcpy((char *)ap.ap.ssid, AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    strcpy((char *)ap.ap.password, AP_PASS);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1; /* effective channel = STA channel when associated */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        TLOG("!! softAP httpd start failed\n");
        return false;
    }
    httpd_uri_t ur_status = {
        .uri = "/", .method = HTTP_GET, .handler = h_status,
    };
    httpd_uri_t ur_prov = {
        .uri = "/prov", .method = HTTP_GET, .handler = h_prov,
    };
    httpd_uri_t ur_save = {
        .uri = "/save", .method = HTTP_POST, .handler = h_save,
    };
    httpd_register_uri_handler(s_httpd, &ur_status);
    httpd_register_uri_handler(s_httpd, &ur_prov);
    httpd_register_uri_handler(s_httpd, &ur_save);

    s_opened_ms = gw_now_ms();
    s_ap_open = true;
    TLOG("softAP '" AP_SSID "' OPEN (10-min window)\n");
    return true;
}

static void softap_close(void)
{
    if (!s_ap_open) {
        return;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_open = false;
    TLOG("softAP closed\n");
}

void gw_softap_toggle(void)
{
    if (s_ap_open) {
        softap_close();
    } else {
        gw_softap_open();
    }
}

/* ---- button task: poll (no ISR — GPIO framework storms, gotchas) ---- */

static void softap_button_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    bool fired = false;
    uint32_t press_start = 0;
    for (;;) {
        bool down = gpio_get_level(PIN_BUTTON) == 0;
        uint32_t now = gw_now_ms();
        if (down && !pressed) {
            pressed = true;
            fired = false;
            press_start = now;
        } else if (down && pressed && !fired &&
                   now - press_start > LONG_PRESS_MS) {
            fired = true;
            gw_softap_toggle();
        } else if (!down) {
            pressed = false;
        }
        /* re-sample: toggle() may have blocked in open(), which set
         * s_opened_ms AFTER the loop-top 'now'. The stale 'now' makes
         * the unsigned delta wrap (~4e9 > AP_WINDOW_MS) and insta-
         * closes the AP on the very iteration that opened it. */
        now = gw_now_ms();
        if (s_ap_open && now - s_opened_ms > AP_WINDOW_MS) {
            softap_close();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void gw_softap_start_button(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  /* pressed = LOW */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    xTaskCreate(softap_button_task, "btn", 3072, NULL, 2, NULL);
}
