/* net.c — uplink: WiFi STA, SNTP, MQTT, DN004 state machine, NVS, time.
 *
 * Owns the gateway mode (WAIT_BACKEND ↔ ACTIVE). ESP-NOW enable/disable is
 * called from here (single owner per DN004); the wire chassis in main.c
 * never decides. Health gate per DN004: WiFi associated AND broker
 * connected AND time valid AND backend writing (commit service health
 * topic fresh). N=3 consecutive healthy 1 Hz checks to enter ACTIVE; a
 * single confirmed failure exits.
 *
 * Backend-writing check: the commit service (server/commit-service)
 * publishes retained bracino/gateway/health every 30 s. Age > 90 s =
 * backend down = never enter / leave ACTIVE. This is service liveness,
 * independent of traffic flow, so it cannot livelock (a stalled acker
 * with zero traffic is still detected).
 *
 * Time: SNTP primary; MQTT time-set fallback (bracino/gateway/time);
 * serial `n` bench fallback; NVS {epoch, uptime} checkpoint so a reboot
 * does not skew gw_ts before the next external source arrives.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw.h"
#include "bracino_log.h"

#define GW_NVS_NS        "gw"
#define HEALTH_STALE_MS  90000   /* 3 × 30 s commit-service period */
#define ACTIVE_ENTRY_N   3       /* DN004 asymmetric hysteresis */
#define STA_RETRY_S      10
#define TIME_TOPIC       "bracino/gateway/time"
#define HEALTH_TOPIC     "bracino/gateway/health"
#define STATUS_TOPIC     "bracino/gateway/status"
#define COMMIT_TOPIC     "bracino/gateway/commit"
#define CHECKPOINT_S     300
#define MQTT_TIMEOUT_MS  10000

#define FW_VERSION       "gw-015.1"

volatile gw_mode_t gw_mode = GW_WAIT_BACKEND;

/* ---- health flags (net.c state; read via getters) ---- */

static volatile bool s_wifi_up;
static volatile bool s_broker_up;
static volatile uint32_t s_health_last_ms;  /* uptime ms of last health msg */

/* ---- time ---- */

static volatile bool s_time_valid;
static const char *s_time_src = "none";

bool gw_time_valid(void) { return s_time_valid; }

uint64_t gw_epoch_ms(void)
{
    if (!s_time_valid) {
        return 0;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000;
}

void gw_time_set_unix(uint64_t unix_s, uint16_t ms)
{
    struct timeval tv = {
        .tv_sec = (time_t)unix_s,
        .tv_usec = (suseconds_t)ms * 1000,
    };
    settimeofday(&tv, NULL);
    s_time_valid = true;
}

const char *gw_time_source(void) { return s_time_src; }

void gw_time_mark_serial(void) { s_time_src = "serial"; }

static void time_set_src(uint64_t unix_s, uint16_t ms, const char *src)
{
    if (!s_time_valid) {
        gw_time_set_unix(unix_s, ms);
        s_time_src = src;
        TLOG("time set from %s\n", src);
    } else {
        uint64_t cur = gw_epoch_ms();
        int64_t diff = (int64_t)(unix_s * 1000 + ms) - (int64_t)cur;
        if (diff < -5000 || diff > 5000) {
            TLOG("!! time anomaly: %s disagrees by %lld ms — applying\n",
                 src, (long long)diff);
        }
        gw_time_set_unix(unix_s, ms);
        s_time_src = src;
    }
}

/* ---- config values (loaded once; softAP save + reboot re-loads) ---- */

static char s_ssid[33], s_pass[65];
static char s_bhost[64];
static uint32_t s_bport;
static char s_muser[33], s_mpass[65];   /* MQTT auth (NVS muser/mpass) */

/* ---- NVS config ---- */

static nvs_handle_t s_nvs;

static void nvs_open_safe(void)
{
    ESP_ERROR_CHECK(nvs_open(GW_NVS_NS, NVS_READWRITE, &s_nvs));
}

void gw_nvs_get_str(const char *key, char *out, size_t outlen, const char *def)
{
    size_t len = outlen;
    if (nvs_get_str(s_nvs, key, out, &len) != ESP_OK) {
        snprintf(out, outlen, "%s", def ? def : "");
    }
}

void gw_nvs_set_str(const char *key, const char *val)
{
    ESP_ERROR_CHECK(nvs_set_str(s_nvs, key, val));
    ESP_ERROR_CHECK(nvs_commit(s_nvs));
}

uint32_t gw_nvs_get_u32(const char *key, uint32_t def)
{
    uint32_t v = def;
    nvs_get_u32(s_nvs, key, &v);
    return v;
}

void gw_nvs_set_u32(const char *key, uint32_t v)
{
    nvs_set_u32(s_nvs, key, v);
    nvs_commit(s_nvs);
}

bool gw_wifi_up(void) { return s_wifi_up; }
bool gw_broker_up(void) { return s_broker_up; }

uint32_t gw_health_age_ms(void)
{
    if (s_health_last_ms == 0) {
        return UINT32_MAX;
    }
    return gw_now_ms() - s_health_last_ms;
}

const char *gw_net_ssid(void) { return s_ssid; }

void gw_net_broker_str(char *out, size_t n)
{
    snprintf(out, n, "%s:%lu", s_bhost, (unsigned long)s_bport);
}

/* NULL when anonymous (no muser in NVS) — status page + serial use it */
const char *gw_net_mqtt_user(void) { return s_muser[0] ? s_muser : NULL; }

static void load_config(void)
{
    gw_nvs_get_str("ssid", s_ssid, sizeof(s_ssid), "");
    gw_nvs_get_str("pass", s_pass, sizeof(s_pass), "");
    gw_nvs_get_str("bhost", s_bhost, sizeof(s_bhost), "192.168.1.215");
    s_bport = gw_nvs_get_u32("bport", 1883);
    gw_nvs_get_str("muser", s_muser, sizeof(s_muser), "");
    gw_nvs_get_str("mpass", s_mpass, sizeof(s_mpass), "");
}

static void checkpoint_time(void)
{
    if (!s_time_valid) {
        return;
    }
    gw_nvs_set_u32("epoch_s", (uint32_t)time(NULL));
    gw_nvs_set_u32("epoch_up", gw_now_ms() / 1000);
}

static void restore_time(void)
{
    uint32_t es = 0, eu = 0;
    bool have_es = nvs_get_u32(s_nvs, "epoch_s", &es) == ESP_OK;
    bool have_eu = nvs_get_u32(s_nvs, "epoch_up", &eu) == ESP_OK;
    if (!have_es || !have_eu || es == 0) {
        return;
    }
    uint32_t elapsed_s = gw_now_ms() / 1000 - eu;
    gw_time_set_unix((uint64_t)es + elapsed_s, 0);
    s_time_src = "nvs";
    TLOG("time restored from NVS checkpoint (+%lu s)\n",
         (unsigned long)elapsed_s);
}

/* ---- WiFi events ---- */

static esp_netif_t *s_sta_netif;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_started;
static uint32_t s_last_conn_try_ms;

static void start_mqtt(void);

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        TLOG("wifi disconnected\n");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        s_wifi_up = true;
        s_last_conn_try_ms = 0;
        TLOG("wifi up: ip=" IPSTR "\n", IP2STR(&ev->ip_info.ip));
        if (!s_mqtt_started) {
            start_mqtt();
        }
    }
}

/* ---- MQTT ---- */

bool gw_mqtt_publish(const char *topic, const char *json, int qos, bool retain)
{
    if (s_mqtt == NULL) {
        return false;
    }
    return esp_mqtt_client_publish(s_mqtt, topic, json, 0, qos,
                                   retain ? 1 : 0) >= 0;
}

/* tiny JSON int extractor for the three flat topics we subscribe */
static bool json_u64(const char *j, const char *key, uint64_t *out)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(p + 1, &end, 10);
    if (end == p + 1 || errno != 0) {
        return false;
    }
    *out = v;
    return true;
}

static void on_commit(const char *j)
{
    uint64_t nt = 0, ni = 0, w = 0;
    if (!json_u64(j, "node_type", &nt) || !json_u64(j, "node_id", &ni) ||
        !json_u64(j, "capture_ms_end", &w)) {
        TLOG("!! commit watermark malformed: %s\n", j);
        return;
    }
    gw_node_t *n = gw_node_by_role((uint8_t)nt, (uint8_t)ni);
    if (n == NULL) {
        return;
    }
    n->have_commit = true;
    n->commit_ms = (uint32_t)w;
    gw_ack_if_covered(n, (uint32_t)w);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED: {
        s_broker_up = true;
        s_health_last_ms = 0;  /* re-evaluate after reconnect */
        TLOG("broker connected\n");
        esp_mqtt_client_subscribe(s_mqtt, COMMIT_TOPIC, 1);
        esp_mqtt_client_subscribe(s_mqtt, HEALTH_TOPIC, 1);
        esp_mqtt_client_subscribe(s_mqtt, TIME_TOPIC, 1);
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "{\"online\":true,\"fw\":\"%s\",\"uptime_s\":%lu}",
                 FW_VERSION, (unsigned long)(gw_now_ms() / 1000));
        esp_mqtt_client_publish(s_mqtt, STATUS_TOPIC, msg, 0, 1, 1);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_broker_up = false;
        TLOG("broker disconnected\n");
        break;
    case MQTT_EVENT_DATA: {
        /* small messages only — drop anything fragmented */
        if (ev->current_data_offset != 0 ||
            ev->data_len != ev->total_data_len) {
            return;
        }
        char topic[64], j[192];
        size_t tl = ev->topic_len < sizeof(topic) - 1
                        ? ev->topic_len : sizeof(topic) - 1;
        memcpy(topic, ev->topic, tl);
        topic[tl] = '\0';
        size_t dl = ev->data_len < (int)sizeof(j) - 1
                        ? ev->data_len : (int)sizeof(j) - 1;
        memcpy(j, ev->data, dl);
        j[dl] = '\0';

        if (strcmp(topic, COMMIT_TOPIC) == 0) {
            on_commit(j);
        } else if (strcmp(topic, HEALTH_TOPIC) == 0) {
            s_health_last_ms = gw_now_ms();
        } else if (strcmp(topic, TIME_TOPIC) == 0) {
            uint64_t ms = 0;
            if (json_u64(j, "epoch_ms", &ms) && ms > 100000000000ULL) {
                time_set_src(ms / 1000, ms % 1000, "mqtt");
            }
        }
        break;
    }
    default:
        break;
    }
}

static void start_mqtt(void)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%lu", s_bhost,
             (unsigned long)s_bport);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        /* single shared MQTT user (t520 mosquitto, allow_anonymous
         * false); empty NVS muser = anonymous, keeps bench drills
         * working against an auth-less broker */
        .credentials.username = s_muser[0] ? s_muser : NULL,
        .credentials.authentication.password = s_mpass[0] ? s_mpass : NULL,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = STATUS_TOPIC,
            .msg = "{\"online\":false}",
            .msg_len = 18,
            .qos = 1,
            .retain = 1,
        },
        .task.stack_size = 8192,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
    s_mqtt_started = true;
    TLOG("mqtt -> %s\n", uri);
}

void gw_net_mqtt_restart(void)
{
    if (s_mqtt) {
        s_broker_up = false;
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
    s_mqtt_started = false;
    load_config();
    if (s_wifi_up) {
        start_mqtt();
    }
}

/* SNTP sync poll → time validity */
static void sntp_poll(void)
{
    static bool sntp_started;
    if (!sntp_started && s_wifi_up) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            sntp_started = true;
        } else {
            TLOG("sntp init failed (will retry)\n");
        }
    }
    if (sntp_started && !s_time_valid &&
        esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_time_valid = true;
        s_time_src = "sntp";
        TLOG("time set from sntp\n");
    }
}

/* ---- state machine task ---- */

static void apply_sta_config(void)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

static void state_task(void *arg)
{
    (void)arg;
    int healthy_cnt = 0;
    uint32_t last_hour_sync_ms = 0;
    uint32_t last_checkpoint_ms = 0;

    for (;;) {
        uint32_t now = gw_now_ms();
        bool backend_ok = gw_broker_up() &&
                          gw_health_age_ms() < HEALTH_STALE_MS;
        bool healthy = s_wifi_up && s_broker_up && gw_time_valid() &&
                       backend_ok;

        switch (gw_mode) {
        case GW_WAIT_BACKEND:
            healthy_cnt = healthy ? healthy_cnt + 1 : 0;
            if (healthy_cnt >= ACTIVE_ENTRY_N) {
                healthy_cnt = 0;
                TLOG("ACTIVE: chain healthy — ESP-NOW up\n");
                gw_espnow_enable();
                gw_mode = GW_ACTIVE;
                last_hour_sync_ms = now; /* hourly timer starts now */
            } else if (s_ssid[0] != '\0' && !s_wifi_up &&
                       now - s_last_conn_try_ms > STA_RETRY_S * 1000) {
                s_last_conn_try_ms = now;
                esp_wifi_connect();
            }
            break;

        case GW_ACTIVE:
            if (!healthy) {
                TLOG("WAIT_BACKEND: chain unhealthy — ESP-NOW down "
                     "(wifi=%d broker=%d time=%d backend_ok=%d)\n",
                     s_wifi_up, s_broker_up, s_time_valid, backend_ok);
                gw_espnow_disable();
                gw_mode = GW_WAIT_BACKEND;
                healthy_cnt = 0;
            } else if (now - last_hour_sync_ms > 3600000) {
                last_hour_sync_ms = now;
                TLOG("hourly TIME_SYNC push\n");
                for (int i = 0; i < gw_node_cnt; i++) {
                    if (gw_nodes[i].in_use) {
                        gw_time_sync_send(&gw_nodes[i]);
                    }
                }
            }
            break;
        }

        if (now - last_checkpoint_ms > CHECKPOINT_S * 1000) {
            checkpoint_time();
            last_checkpoint_ms = now;
        }

        sntp_poll();

        vTaskDelay(pdMS_TO_TICKS(100));  /* 1 Hz-ish gate */
    }
}

/* ---- init ---- */

void gw_net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    nvs_open_safe();
    load_config();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    (void)s_sta_netif;
    /* AP netif: static 192.168.5.1/24 — the IDF default 192.168.4.1
     * collides with Starlink routers, which use the same base address
     * (bench 2026-09-05: phone join/leave loop + unreachable pages). */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    esp_netif_ip_info_t ap_ip = { 0 };
    IP4_ADDR(&ap_ip.ip, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip.gw, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); /* ESP-NOW rx latency */

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_ssid[0] != '\0') {
        apply_sta_config();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    TLOG("wifi started (sta %s)\n", s_ssid[0] ? s_ssid : "UNPROVISIONED");

    restore_time();

    xTaskCreate(state_task, "gw_state", 12288, NULL, 4, NULL);
}
