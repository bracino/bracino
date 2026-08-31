/*
 * bench-espnow-master — DN003 wire-contract test harness (issue 013).
 *
 * THIS IS NOT THE REAL GATEWAY. It speaks just enough DN003 to exercise the
 * node client: HELLO→HELLO_ACK(+TIME_SYNC), HEARTBEAT/TELEMETRY_BATCH/EVENT
 * logging, BATCH_ACK ("durable write" = a serial log line), CONFIG_GET on
 * unknown config_ver, PARAM_SET on keypress. No WiFi AP association, no
 * MQTT, no state machine, no NVS role table — fine for wire-contract
 * validation, NOT evidence of DN004 compliance.
 *
 * Bench knobs (compile-time, edit and rebuild):
 *   BENCH_NO_ACK_S   suppress BATCH_ACKs for the first N s after boot
 *                    (forces node retransmit / stop-and-wait drill). 0=off.
 *   BENCH_DROP_PCT   randomly drop % of received TELEMETRY_BATCH frames. 0=off.
 *
 * Serial console (115200): node also logs here.
 *   n <unix_s>   set epoch (TIME_SYNC values are 0 until you do this;
 *                a node will NOT transmit telemetry before it gets a
 *                non-zero TIME_SYNC — that is the DN003 invariant)
 *   c <1..13>    hop channel (outage simulation)
 *   t            send standalone TIME_SYNC to all registered nodes
 *   p            canned PARAM_SET: setpoint ±0.5 °C to first node
 *   s            registry + counters (also the GPIO27 button)
 *   h            help
 *
 * Button on GPIO27 (pull-up, press = GND): same as 's'. Reserved for the
 * maintenance SoftAP in the real gateway firmware (DN004) — this pin was
 * chosen because GPIO12/0/2/15 are strapping pins on the classic ESP32.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "espnow_schema.h"
#include "bracino_log.h"

#define PIN_BUTTON     GPIO_NUM_27
#define DEFAULT_CH     6
#define MAX_NODES      4
#define RX_QUEUE_LEN   8
#define FRAG_TIMEOUT_MS 2000

#ifndef BENCH_NO_ACK_S
#define BENCH_NO_ACK_S 0
#endif
#ifndef BENCH_DROP_PCT
#define BENCH_DROP_PCT 0
#endif

/* ---- helpers ---- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    const uint8_t *p;
    int len;
} tlv_cur_t;

static bool tlv_next(tlv_cur_t *c, uint8_t *tag, const uint8_t **val, uint8_t *vlen)
{
    if (c->len < 2) {
        return false;
    }
    uint8_t t = c->p[0];
    uint8_t l = c->p[1];
    if (c->len < 2 + l) {
        return false;
    }
    *tag = t;
    *vlen = l;
    *val = c->p + 2;
    c->p += 2 + l;
    c->len -= 2 + l;
    return true;
}

/* ---- state ---- */

typedef struct {
    uint8_t mac[6];
    int len;
    uint8_t data[ESPNOW_MAX_PAYLOAD];
} rx_msg_t;

typedef struct {
    bool have;
    uint64_t epoch_total_ms; /* gateway epoch (ms since Unix epoch) at anchor */
    uint32_t node_clock_ms;  /* node_clock_ms in the HELLO that anchored it */
    uint8_t boot_session;
} anchor_t;

typedef struct {
    uint8_t mac[6];
    uint8_t type, id;
    uint16_t liveness_s;
    uint8_t last_boot;
    uint16_t last_seq;
    uint8_t config_ver;
    bool config_fetched;
    uint32_t last_seen_ms;
    bool flagged_unreach;      /* liveness warning latched (harness-level) */
    uint16_t last_batch_seq;
    uint8_t last_batch_boot;   /* dedupe retransmitted batches (re-ACK,
                                * don't re-print the sample block) */
    anchor_t anchor;
    /* CONFIG_DESC reassembly */
    uint8_t frag_total, frag_next;
    uint32_t frag_last_ms;
    uint8_t desc_buf[600];
    size_t desc_len;
} node_rec_t;

static node_rec_t s_nodes[MAX_NODES];
static int s_node_cnt;

static uint64_t s_epoch_set_ms;   /* 0 = time never set */
static uint32_t s_clock_set_ms;
static uint32_t s_tx_seq;
static uint16_t s_param_admin_seq;
static int32_t s_param_setpoint_raw = 600; /* 60.0 °C x10; p toggles ±5 */

static QueueHandle_t s_rx_q;
static SemaphoreHandle_t s_send_mu;  /* serialize esp_now_send + cb pairing */
static SemaphoreHandle_t s_tx_done;  /* TX completion: BINARY sem — the cb
                                      * runs in the WiFi task and giving a
                                      * MUTEX from a non-owner asserts
                                      * (xTaskPriorityDisinherit) */
static SemaphoreHandle_t s_print_mu; /* console vs proc task */
static volatile bool s_btn_hit;
static uint32_t s_boot_ms = 0;

static struct {
    uint32_t rx[13];
    uint32_t acks_sent, acks_suppressed, drops;
    uint32_t tx_ok, tx_fail;
} s_ct;

static const char *MSG_NAMES[13] = {
    "INVALID", "HELLO", "HELLO_ACK", "HEARTBEAT", "TIME_SYNC",
    "TELEMETRY_BATCH", "BATCH_ACK", "EVENT", "CONFIG_GET", "CONFIG_DESC",
    "PARAM_GET", "PARAM_SET", "PARAM_ACK",
};

/* ---- epoch / translation ---- */

static bool time_set(void)
{
    return s_epoch_set_ms != 0;
}

static uint64_t epoch_total_ms(void)
{
    return s_epoch_set_ms + (uint64_t)(now_ms() - s_clock_set_ms);
}

static void epoch_to_str(uint64_t total_ms, char *out, size_t outlen)
{
    time_t sec = (time_t)(total_ms / 1000);
    struct tm tm;
    gmtime_r(&sec, &tm);
    strftime(out, outlen, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *fault_name(uint8_t bit)
{
    switch (bit) {
    case BBU_FAULT_TPO_OPEN:  return "TPO_OPEN";
    case BBU_FAULT_TPO_SHORT: return "TPO_SHORT";
    case BBU_FAULT_TPU_OPEN:  return "TPU_OPEN";
    case BBU_FAULT_TPU_SHORT: return "TPU_SHORT";
    case BBU_FAULT_AMB_OPEN:  return "AMB_OPEN";
    case BBU_FAULT_AMB_SHORT: return "AMB_SHORT";
    default:                  return "?";
    }
}

static const char *ct_name(uint8_t v)
{
    switch (v) {
    case 0: return "OFF";
    case 1: return "RUNNING";
    case 2: return "NO_CURRENT_WARN";
    default: return "?";
    }
}

static const char *mode_name(uint8_t v)
{
    switch (v) {
    case BBU_MODE_W_MANUAL: return "MANUAL";
    case BBU_MODE_W_AUTO:   return "AUTO";
    case BBU_MODE_W_TEST:   return "TEST";
    case BBU_MODE_W_OFF:    return "OFF";
    default:                return "?";
    }
}

static void param_value_str(uint8_t type, const uint8_t *v, char *out, size_t outlen)
{
    switch (type) {
    case PTYPE_U32:
        snprintf(out, outlen, "%lu", (unsigned long)rd_u32(v));
        break;
    case PTYPE_I16_X10:
        snprintf(out, outlen, "%.1f", (double)((int16_t)rd_u16(v)) / 10.0);
        break;
    case PTYPE_ENUM:
        snprintf(out, outlen, "%u", v[0]);
        break;
    default:
        snprintf(out, outlen, "?");
        break;
    }
}

/* ---- radio ---- */

static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_ct.tx_ok++;
    } else {
        s_ct.tx_fail++;
    }
    xSemaphoreGive(s_tx_done); /* binary sem — NEVER the mutex (see above) */
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (data == NULL || len <= 0 || len > ESPNOW_MAX_PAYLOAD) {
        return;
    }
    rx_msg_t rx;
    memcpy(rx.mac, info->src_addr, 6);
    rx.len = len;
    memcpy(rx.data, data, (size_t)len);
    if (xQueueSend(s_rx_q, &rx, 0) != pdTRUE) {
        /* overflow: drop (bench) */
    }
}

/* Channel set with loud failure logging + keep unicast peers on the new
 * channel (peer TX channel can latch at add_peer time). */
static void set_channel_checked(uint8_t ch)
{
    esp_wifi_set_promiscuous(true);
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (err != ESP_OK) {
        TLOG("!! set_channel(%u) FAILED: %s\n", ch, esp_err_to_name(err));
        return;
    }
    for (int i = 0; i < s_node_cnt; i++) {
        if (!esp_now_is_peer_exist(s_nodes[i].mac)) {
            continue;
        }
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, s_nodes[i].mac, 6);
        peer.channel = ch;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_mod_peer(&peer);
    }
}

static esp_err_t radio_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); /* ESP-NOW rx latency */
    set_channel_checked(DEFAULT_CH);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    return ESP_OK;
}

static void ensure_peer(const uint8_t *mac)
{
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_add_peer(&peer);
}

static bool send_wait(const uint8_t *mac, const uint8_t *buf, size_t len)
{
    xSemaphoreTakeRecursive(s_send_mu, portMAX_DELAY);
    xSemaphoreTake(s_tx_done, 0); /* drain stale completion */
    ensure_peer(mac);
    bool ok = esp_now_send(mac, buf, len) == ESP_OK;
    if (ok) {
        ok = xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(500)) == pdTRUE;
    }
    xSemaphoreGiveRecursive(s_send_mu);
    return ok;
}

/* ---- envelope build (gateway side) ---- */

static size_t gw_env_build(uint8_t *buf, uint8_t node_type, uint8_t node_id,
                           uint8_t msg_type, const void *payload, uint8_t plen)
{
    espnow_envelope_t *e = (espnow_envelope_t *)buf;
    e->proto_ver = ESPNOW_PROTO_VER;
    e->node_type = node_type;
    e->node_id = node_id;
    e->msg_type = msg_type;
    e->seq = (uint16_t)s_tx_seq++;
    e->node_clock_ms = now_ms();
    e->flags = 0;
    e->boot_session = 1;
    e->payload_len = plen;
    if (plen) {
        memcpy(e->payload, payload, plen);
    }
    return ESPNOW_ENV_SIZE + plen;
}

/* current primary channel, for loud logging (0 on read failure) */
static uint8_t wifi_prim_ch(void)
{
    uint8_t prim;
    wifi_second_chan_t sc;
    return (esp_wifi_get_channel(&prim, &sc) == ESP_OK) ? prim : 0;
}

static size_t time_sync_tlv(uint8_t *tlv)
{
    tlv[0] = TLV_TIME_SYNC;
    tlv[1] = 6;
    if (time_set()) {
        uint64_t ms = epoch_total_ms();
        /* value layout: epoch_s u32 LE at tlv+2, epoch_ms u16 LE at tlv+6.
         * (Writing ms at tlv+4 clobbers the u32's high half — the first
         * HELLO anchored a garbage epoch because of exactly that.) */
        wr_u32(tlv + 2, (uint32_t)(ms / 1000));
        wr_u16(tlv + 6, (uint16_t)(ms % 1000));
    } else {
        wr_u32(tlv + 2, 0);
        wr_u16(tlv + 4, 0); /* epoch 0: node will buffer, not transmit */
    }
    return 8;
}

/* ---- registry ---- */

static node_rec_t *node_find_or_add(const uint8_t *mac, uint8_t type, uint8_t id)
{
    for (int i = 0; i < s_node_cnt; i++) {
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) {
            return &s_nodes[i];
        }
    }
    if (s_node_cnt >= MAX_NODES) {
        return NULL;
    }
    node_rec_t *n = &s_nodes[s_node_cnt++];
    memset(n, 0, sizeof(*n));
    memcpy(n->mac, mac, 6);
    n->type = type;
    n->id = id;
    n->last_seq = 0;
    return n;
}

static void time_sync_send(node_rec_t *n)
{
    /* Standalone TIME_SYNC payload is the RAW 6-byte time_sync_t (DN003) —
     * NOT the TLV-wrapped form (HELLO_ACK carries the TLV; this doesn't).
     * Sending 8 B here made the node ignore every push (len check 13+6). */
    time_sync_t ts = { 0 };
    if (time_set()) {
        uint64_t ms = epoch_total_ms();
        ts.epoch_s = (uint32_t)(ms / 1000);
        ts.epoch_ms = (uint16_t)(ms % 1000);
    }
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = gw_env_build(buf, n->type, n->id, MSG_TIME_SYNC, &ts, sizeof(ts));
    uint8_t prim;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&prim, &sc);
    if (send_wait(n->mac, buf, len)) {
        TLOG("> TIME_SYNC -> node(%u,%u) on ch %u\n", n->type, n->id, prim);
    }
}

static void param_set_send(node_rec_t *n)
{
    s_param_setpoint_raw += (s_param_admin_seq & 1) ? 5 : -5; /* ±0.5 °C */
    s_param_admin_seq++;

    uint8_t tlv[24];
    size_t n_tlv = 0;
    tlv[n_tlv++] = TLV_PARAM_ID;  tlv[n_tlv++] = 1; tlv[n_tlv++] = BBU_PARAM_TPO_SETPOINT_C;
    tlv[n_tlv++] = TLV_PARAM_VALUE; tlv[n_tlv++] = 2; wr_u16(tlv + n_tlv, (uint16_t)s_param_setpoint_raw); n_tlv += 2;
    tlv[n_tlv++] = TLV_ADMIN_SEQ; tlv[n_tlv++] = 4; wr_u32(tlv + n_tlv, s_param_admin_seq); n_tlv += 4;
    tlv[n_tlv++] = TLV_TTL_S;     tlv[n_tlv++] = 1; tlv[n_tlv++] = 60;

    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = gw_env_build(buf, n->type, n->id, MSG_PARAM_SET, tlv, (uint8_t)n_tlv);
    if (send_wait(n->mac, buf, len)) {
        TLOG("> PARAM_SET setpoint=%.1f admin_seq=%lu -> node(%u,%u)\n",
               (double)s_param_setpoint_raw / 10.0,
               (unsigned long)s_param_admin_seq, n->type, n->id);
    } else {
        TLOG("> PARAM_SET send failed\n");
    }
}

/* ---- receive dispatch ---- */

static void handle_hello(const rx_msg_t *m, const espnow_envelope_t *e)
{
    tlv_cur_t c = { m->data + ESPNOW_ENV_SIZE, m->len - ESPNOW_ENV_SIZE };
    uint8_t tag, vlen;
    const uint8_t *val;
    uint8_t config_ver = 0;
    while (tlv_next(&c, &tag, &val, &vlen)) {
        if (tag == TLV_CONFIG_VER && vlen == 1) {
            config_ver = val[0];
        }
    }
    node_rec_t *n = node_find_or_add(m->mac, e->node_type, e->node_id);
    if (n == NULL) {
        TLOG("node(%u,%u) HELLO but registry full — no ack\n", e->node_type, e->node_id);
        return;
    }
    n->liveness_s = 2;
    n->last_seen_ms = now_ms();
    n->last_boot = e->boot_session;
    TLOG("HELLO node(%u,%u) mac " MACSTR " cfg_ver=%u seq=%u (driver ch %u)\n",
           e->node_type, e->node_id, MAC2STR(m->mac), config_ver, e->seq,
           wifi_prim_ch());

    /* HELLO_ACK: always carries a fresh TIME_SYNC (DN003). */
    uint8_t tlv[8];
    size_t tlen = time_sync_tlv(tlv);
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = gw_env_build(buf, e->node_type, e->node_id, MSG_HELLO_ACK,
                              tlv, (uint8_t)tlen);
    if (!send_wait(n->mac, buf, len)) {
        TLOG("HELLO_ACK send failed\n");
        return;
    }
    /* Anchor: gateway time now, vs node clock in the HELLO envelope. */
    n->anchor.have = true;
    n->anchor.epoch_total_ms = time_set() ? epoch_total_ms() : 0;
    n->anchor.node_clock_ms = e->node_clock_ms;
    n->anchor.boot_session = e->boot_session;

    bool known = n->config_fetched && n->config_ver == config_ver;
    n->config_ver = config_ver;
    if (!known) {
        TLOG("config_ver=%u unknown/changed -> CONFIG_GET\n", config_ver);
        uint8_t get_buf[ESPNOW_ENV_SIZE];
        size_t glen = gw_env_build(get_buf, n->type, n->id, MSG_CONFIG_GET, NULL, 0);
        send_wait(n->mac, get_buf, glen);
    }
}

static void print_batch(const rx_msg_t *m, const espnow_envelope_t *e, node_rec_t *n)
{
    bool dup = n != NULL && e->seq == n->last_batch_seq &&
               e->boot_session == n->last_batch_boot;
    const telemetry_batch_hdr_t *h =
        (const telemetry_batch_hdr_t *)(m->data + ESPNOW_ENV_SIZE);
    size_t plen = m->len - ESPNOW_ENV_SIZE;
    if (plen < 11) {
        TLOG("TELEMETRY_BATCH malformed (short header)\n");
        return;
    }
    uint16_t count = rd_u16((const uint8_t *)&h->count);
    size_t sample_sz = sizeof(bbu_telemetry_v1_t);
    size_t expected = 11 + (size_t)count * sample_sz;
    if (count > 0 && plen < expected) {
        TLOG("TELEMETRY_BATCH malformed (count %u, plen %u)\n",
               count, (unsigned)plen);
        return;
    }

    char t0s[24] = "?", t1s[24] = "?";
    if (n->anchor.have) {
        int64_t d0 = (int64_t)(int32_t)(h->start_ms - n->anchor.node_clock_ms);
        int64_t d1 = (int64_t)(int32_t)(h->end_ms - n->anchor.node_clock_ms);
        epoch_to_str(n->anchor.epoch_total_ms + (uint64_t)d0, t0s, sizeof(t0s));
        epoch_to_str(n->anchor.epoch_total_ms + (uint64_t)d1, t1s, sizeof(t1s));
    }
    if (dup) {
        TLOG("  (dup batch seq=%u re-ACKed)\n", e->seq);
        return;
    }
    if (n != NULL) {
        n->last_batch_seq = e->seq;
        n->last_batch_boot = e->boot_session;
    }
    TLOG("$ committed node(%u,%u) seq=%u span=[%lu..%lu] ms=%u count=%u "
           "utc=[%s .. %s] boot=%u\n",
           e->node_type, e->node_id, e->seq,
           (unsigned long)h->start_ms, (unsigned long)h->end_ms,
           count, (unsigned)plen - 11, t0s, t1s, e->boot_session);

    const uint8_t *p = m->data + ESPNOW_ENV_SIZE + 11;
    for (uint16_t i = 0; i < count; i++, p += sample_sz) {
        const bbu_telemetry_v1_t *s = (const bbu_telemetry_v1_t *)p;
        TLOG("  [%2u] mode=%-6s relay=%u ct=%-15s tpo=%.1f tpu=%.1f amb=%.1f"
               " faults=%02x",
               i, mode_name(s->mode), s->relay_state, ct_name(s->ct_state),
               (double)s->t_tpo_x10 / 10.0, (double)s->t_tpu_x10 / 10.0,
               (double)s->t_amb_x10 / 10.0, s->fault_flags);
        if (s->fault_flags) {
            printf(" (");
            for (uint8_t b = 0; b < BBU_FAULT_COUNT; b++) {
                if (s->fault_flags & (1u << b)) {
                    printf("%s ", fault_name(b));
                }
            }
            TLOG(")");
        }
        TLOG("\n");
    }
}

static void handle_batch(const rx_msg_t *m, const espnow_envelope_t *e, node_rec_t *n)
{
    /* BENCH_DROP_PCT: randomly drop frames (frame-loss drill) */
    if (BENCH_DROP_PCT > 0 && (rand() % 100) < BENCH_DROP_PCT) {
        s_ct.drops++;
        TLOG("TELEMETRY_BATCH seq=%u DROPPED (bench knob)\n", e->seq);
        return;
    }

    print_batch(m, e, n); /* "durable write" = the committed log line above */

    if (!time_set()) {
        s_ct.acks_suppressed++;
        return; /* no epoch: never ack (node keeps buffering) */
    }
    if (BENCH_NO_ACK_S > 0 &&
        (int32_t)(now_ms() - s_boot_ms) < (int32_t)BENCH_NO_ACK_S * 1000) {
        s_ct.acks_suppressed++;
        TLOG("  (ack suppressed by BENCH_NO_ACK_S — retransmit drill)\n");
        return;
    }

    const telemetry_batch_hdr_t *h =
        (const telemetry_batch_hdr_t *)(m->data + ESPNOW_ENV_SIZE);
    uint8_t payload[4];
    wr_u32(payload, h->end_ms); /* watermark = last sample capture_ms */
    uint8_t buf[ESPNOW_ENV_SIZE + 4];
    size_t len = gw_env_build(buf, e->node_type, e->node_id, MSG_BATCH_ACK,
                              payload, 4);
    if (send_wait(n->mac, buf, len)) {
        s_ct.acks_sent++;
    }
}

static void handle_event(const rx_msg_t *m)
{
    if (m->len < ESPNOW_ENV_SIZE + 2) {
        return;
    }
    uint8_t tag = m->data[ESPNOW_ENV_SIZE];
    uint8_t vlen = m->data[ESPNOW_ENV_SIZE + 1];
    const uint8_t *v = m->data + ESPNOW_ENV_SIZE + 2;
    const char *name = tag == EVENT_FAULT_RAISED  ? "FAULT_RAISED" :
                       tag == EVENT_FAULT_CLEARED ? "FAULT_CLEARED" :
                       tag == EVENT_PARAM_CHANGED ? "PARAM_CHANGED" :
                       tag == EVENT_CONFIG_CHANGED ? "CONFIG_CHANGED" :
                       tag == EVENT_BATTERY_WARN  ? "BATTERY_WARN" : "?";
    TLOG("EVENT %s value=", name);
    for (int i = 0; i < vlen; i++) {
        printf("%02x ", v[i]);
    }
    if (tag == EVENT_PARAM_CHANGED && vlen >= 1) {
        printf("(param %u)", v[0]);
    }
    TLOG("\n");
}

static void handle_config_desc(const rx_msg_t *m, const espnow_envelope_t *e,
                               node_rec_t *n)
{
    if (m->len < ESPNOW_ENV_SIZE + CONFIG_DESC_FRAG_OVH) {
        return;
    }
    const uint8_t *payload = m->data + ESPNOW_ENV_SIZE;
    uint8_t idx = payload[0];
    uint8_t total = payload[1];
    size_t chunk = (size_t)m->len - ESPNOW_ENV_SIZE - CONFIG_DESC_FRAG_OVH;

    if (idx == 0) {
        n->frag_total = total;
        n->frag_next = 0;
        n->desc_len = 0;
    }
    if (total != n->frag_total || idx != n->frag_next) {
        TLOG("CONFIG_DESC fragment out of order (got %u, want %u) — dropped\n",
               idx, n->frag_next);
        n->frag_total = 0; /* reset; node retransmits on CONFIG_GET retry */
        return;
    }
    if (n->desc_len + chunk > sizeof(n->desc_buf)) {
        TLOG("CONFIG_DESC too big\n");
        n->frag_total = 0;
        return;
    }
    memcpy(n->desc_buf + n->desc_len, payload + CONFIG_DESC_FRAG_OVH, chunk);
    n->desc_len += chunk;
    n->frag_next++;
    n->frag_last_ms = now_ms();

    if (idx + 1 < total) {
        TLOG("CONFIG_DESC frag %u/%u (%u B, more)\n", idx, total, (unsigned)chunk);
        return;
    }

    TLOG("CONFIG_DESC complete (%u B):\n", (unsigned)n->desc_len);
    tlv_cur_t c = { n->desc_buf, (int)n->desc_len };
    uint8_t tag, vlen;
    const uint8_t *val;
    while (tlv_next(&c, &tag, &val, &vlen)) {
        if (tag != TLV_PARAM_DESCRIPTOR || vlen < 16) {
            TLOG("  (unknown tag %u len %u)\n", tag, vlen);
            continue;
        }
        char mn[16], mx[16], st[16], vname[32];
        param_value_str(val[1], (const uint8_t *)&val[3], mn, sizeof(mn));
        param_value_str(val[1], (const uint8_t *)&val[7], mx, sizeof(mx));
        param_value_str(val[1], (const uint8_t *)&val[11], st, sizeof(st));
        size_t name_len = vlen - 15;
        if (name_len >= sizeof(vname)) {
            name_len = sizeof(vname) - 1;
        }
        memcpy(vname, val + 15, name_len);
        vname[name_len] = '\0';
        TLOG("  id=%2u %-24s type=%u flags=%u min=%s max=%s step=%s\n",
               val[0], vname, val[1], val[2], mn, mx, st);
    }
    n->frag_total = 0; /* done — disarm the reassembly watchdog */
    n->config_fetched = true;
}

static void handle_param_ack(const rx_msg_t *m)
{
    tlv_cur_t c = { m->data + ESPNOW_ENV_SIZE, m->len - ESPNOW_ENV_SIZE };
    uint8_t tag, vlen;
    const uint8_t *val;
    uint8_t id = 0, result = 0;
    char prev_s[24] = "?", new_s[24] = "?";
    bool have_prev = false, have_new = false;
    while (tlv_next(&c, &tag, &val, &vlen)) {
        switch (tag) {
        case TLV_PARAM_ID:
            if (vlen == 1) {
                id = val[0];
            }
            break;
        case TLV_PARAM_RESULT:
            if (vlen == 1) {
                result = val[0];
            }
            break;
        case TLV_PREV_VALUE:
            /* type unknown on this end: try x10 float display */
            param_value_str(PTYPE_I16_X10, val, prev_s, sizeof(prev_s));
            have_prev = true;
            break;
        case TLV_NEW_VALUE:
            param_value_str(PTYPE_I16_X10, val, new_s, sizeof(new_s));
            have_new = true;
            break;
        default:
            break;
        }
    }
    static const char *RES[] = { "OK", "REJECTED_RANGE", "REJECTED_TYPE", "EXPIRED" };
    TLOG("PARAM_ACK id=%u result=%s prev=%s new=%s\n", id,
           result < 4 ? RES[result] : "?",
           have_prev ? prev_s : "-", have_new ? new_s : "-");
}

static void handle_rx(const rx_msg_t *m)
{
    const espnow_envelope_t *e = (const espnow_envelope_t *)m->data;
    if (m->len < ESPNOW_ENV_SIZE || e->proto_ver != ESPNOW_PROTO_VER ||
        e->msg_type == MSG_INVALID || e->msg_type > MSG_PARAM_ACK) {
        return; /* malformed: drop */
    }
    s_ct.rx[e->msg_type]++;

    if (e->seq != 0) {
        /* seq gaps are informational only (DN003) */
    }

    node_rec_t *n = NULL;
    for (int i = 0; i < s_node_cnt; i++) {
        if (memcmp(s_nodes[i].mac, m->mac, 6) == 0) {
            n = &s_nodes[i];
            break;
        }
    }
    if (n != NULL && n->flagged_unreach) {
        n->flagged_unreach = false;
        TLOG("node(%u,%u) frames resumed\n", e->node_type, e->node_id);
    }

    switch (e->msg_type) {
    case MSG_HELLO:
        handle_hello(m, e);
        break;
    case MSG_HEARTBEAT:
        if (n) {
            n->last_seen_ms = now_ms();
            TLOG("HB node(%u,%u) seq=%u clk=%lu ms\n", e->node_type, e->node_id,
                   e->seq, (unsigned long)e->node_clock_ms);
        } else {
            TLOG("HB from unregistered MAC " MACSTR " node(%u,%u) "
                   "— waiting for HELLO\n", MAC2STR(m->mac),
                   e->node_type, e->node_id);
        }
        break;
    case MSG_TELEMETRY_BATCH:
        if (n) {
            n->last_seen_ms = now_ms();
            handle_batch(m, e, n);
        } else {
            TLOG("TELEMETRY_BATCH seq=%u from unregistered MAC " MACSTR
                   " — dropped (node re-HELLOs after ack timeouts)\n",
                   e->seq, MAC2STR(m->mac));
        }
        break;
    case MSG_EVENT:
        if (n) {
            n->last_seen_ms = now_ms();
        }
        handle_event(m);
        break;
    case MSG_CONFIG_DESC:
        if (n) {
            handle_config_desc(m, e, n);
        }
        break;
    case MSG_PARAM_ACK:
        handle_param_ack(m);
        break;
    default:
        break; /* TIME_SYNC/HELLO_ACK/etc are gateway→node: not for us */
    }
}

static void registry_print(void); /* console + button/proc task */

static void proc_task(void *arg)
{
    (void)arg;
    for (;;) {
        rx_msg_t m;
        if (xQueueReceive(s_rx_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            xSemaphoreTakeRecursive(s_print_mu, portMAX_DELAY);
            handle_rx(&m);
            xSemaphoreGiveRecursive(s_print_mu);
        }
        if (s_btn_hit) {
            s_btn_hit = false;
            xSemaphoreTakeRecursive(s_print_mu, portMAX_DELAY);
            registry_print();
            xSemaphoreGiveRecursive(s_print_mu);
        }
        /* reassembly timeout watchdog */
        for (int i = 0; i < s_node_cnt; i++) {
            node_rec_t *n = &s_nodes[i];
            if (n->frag_total != 0 &&
                (now_ms() - n->frag_last_ms) > FRAG_TIMEOUT_MS) {
                TLOG("CONFIG_DESC reassembly timeout — reset\n");
                n->frag_total = 0;
            }
        }
        /* harness-level liveness (the real thing is DN004 scope): warn once
         * when a registered node goes quiet past 3x its declared cadence */
        for (int i = 0; i < s_node_cnt; i++) {
            node_rec_t *n = &s_nodes[i];
            uint32_t expect_ms = (uint32_t)n->liveness_s * 3u * 1000u;
            if (n->liveness_s == 0 || n->last_seen_ms == 0) {
                continue;
            }
            if (!n->flagged_unreach &&
                (now_ms() - n->last_seen_ms) > expect_ms) {
                n->flagged_unreach = true;
                TLOG("!! node(%u,%u) silent for %lus — unreachable "
                     "(expect ~%us cadence)\n", n->type, n->id,
                     (unsigned long)((now_ms() - n->last_seen_ms) / 1000),
                     n->liveness_s);
            }
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    int last = 1;
    for (;;) {
        int v = gpio_get_level(PIN_BUTTON);
        if (last == 1 && v == 0) { /* pressed (pull-up, press = GND) */
            vTaskDelay(pdMS_TO_TICKS(30)); /* debounce */
            if (gpio_get_level(PIN_BUTTON) == 0) {
                s_btn_hit = true;
                last = 0;
            }
        } else {
            last = v;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- console ---- */

static void registry_print(void)
{
    TLOG("registry (%d node%s):\n", s_node_cnt, s_node_cnt == 1 ? "" : "s");
    for (int i = 0; i < s_node_cnt; i++) {
        node_rec_t *n = &s_nodes[i];
        TLOG("  node(%u,%u) mac=" MACSTR " cfg_ver=%u fetched=%d "
               "last_seen=%lus ago boot=%u\n",
               n->type, n->id, MAC2STR(n->mac), n->config_ver,
               n->config_fetched, (unsigned long)((now_ms() - n->last_seen_ms) / 1000),
               n->last_boot);
        if (n->anchor.have) {
            char ts[24] = "?";
            if (n->anchor.epoch_total_ms) {
                epoch_to_str(n->anchor.epoch_total_ms, ts, sizeof(ts));
            }
            TLOG("    anchor: gw_time=%s @ node_clock=%lu ms (boot %u)\n",
                   ts, (unsigned long)n->anchor.node_clock_ms, n->anchor.boot_session);
        }
    }
    TLOG("counters: rx=");
    for (int t = 1; t <= MSG_PARAM_ACK; t++) {
        if (s_ct.rx[t]) {
            printf("%s=%lu ", MSG_NAMES[t], (unsigned long)s_ct.rx[t]);
        }
    }
    TLOG("\n  acks_sent=%lu suppressed=%lu drops=%lu tx_ok=%lu tx_fail=%lu\n",
           (unsigned long)s_ct.acks_sent, (unsigned long)s_ct.acks_suppressed,
           (unsigned long)s_ct.drops, (unsigned long)s_ct.tx_ok,
           (unsigned long)s_ct.tx_fail);
    if (!time_set()) {
        TLOG("  EPOCH NOT SET — nodes will buffer telemetry but not "
               "transmit it (DN003). Use: n <unix_s>\n");
    }
}

static volatile uint32_t s_sniff_cnt;
static volatile uint32_t s_sniff_printed;
static void sniff_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    s_sniff_cnt++;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (s_sniff_printed >= 40 || len > 130) {
        return; /* count-only for big/noisy frames */
    }
    s_sniff_printed++;
    const uint8_t *p = pkt->payload;
    /* 802.11: addr2 (transmitter) at offset 10 for data frames */
    TLOG("  snif len=%3u type=%u a2=" MACSTR " fc=%02x%02x\n",
           len, (unsigned)type, MAC2STR(p + 10), p[1], p[0]);
}

static void print_help(void)
{
    TLOG(
        "bench master: HELLO_ACK+TIME_SYNC, BATCH_ACK-after-log, "
        "CONFIG_GET, canned PARAM_SET\n"
        "  n <unix_s>  set epoch (required before telemetry will flow!)\n"
        "  c <1..13>   hop channel (outage drill)\n"
        "  t           push TIME_SYNC to all nodes\n"
        "  p           PARAM_SET setpoint +/-0.5 C to first node\n"
        "  s           registry + counters (GPIO27 button too)\n"
        "  k           show driver's actual current channel\n"
        "  w [sec]         promiscuous frame count + MACs (RF sanity)\n"
        "  h           this help\n"
        "knobs (edit + rebuild): BENCH_NO_ACK_S=%d BENCH_DROP_PCT=%d\n",
        BENCH_NO_ACK_S, BENCH_DROP_PCT);
}

void app_main(void)
{
    s_boot_ms = now_ms();
    s_rx_q = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_msg_t));
    configASSERT(s_rx_q);
    s_send_mu = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_send_mu);
    s_tx_done = xSemaphoreCreateBinary();
    configASSERT(s_tx_done);
    s_print_mu = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_print_mu);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_ERROR_CHECK(radio_start());
    TLOG("\n# bench-espnow-master ch=%d button=GPIO%d NO_ACK=%ds DROP=%d%%\n",
           DEFAULT_CH, PIN_BUTTON, BENCH_NO_ACK_S, BENCH_DROP_PCT);

    xTaskCreate(proc_task, "proc", 8192, NULL, 2, NULL);
    xTaskCreate(button_task, "btn", 2048, NULL, 1, NULL);
    print_help();
    printf("> "); fflush(stdout);
    fflush(stdout);

    char line[80];
    size_t len = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
                line[--len] = '\0';
            }
            if (len > 0) {
                TLOG("cmd: %s", line);
            }
            if (strncmp(line, "n ", 2) == 0) {
                char *end = NULL;
                unsigned long long sec = strtoull(line + 2, &end, 10);
                if (end != line + 2 && sec > 0) {
                    s_epoch_set_ms = (uint64_t)sec * 1000ULL;
                    s_clock_set_ms = now_ms();
                    char ts[24];
                    epoch_to_str(s_epoch_set_ms, ts, sizeof(ts));
                    TLOG("epoch set: %s UTC — TIME_SYNC now carries it\n", ts);
                    if (end && *end != '\0') {
                        TLOG("  (note: ignored trailing '%s' — integer seconds only)\n", end);
                    }
                } else {
                    TLOG("usage: n <unix_seconds>\n");
                }
            } else if (line[0] == 'c' && line[1] == ' ') {
                int ch = atoi(line + 2);
                if (ch >= 1 && ch <= 13) {
                    set_channel_checked((uint8_t)ch);
                    uint8_t prim;
                    wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE; /* driver may leave this unwritten */
                    esp_wifi_get_channel(&prim, &sc);
                    TLOG("channel -> %d (driver says %u; nodes must rescan)\n",
                           ch, prim);
                } else {
                    TLOG("c 1..13\n");
                }
            } else if (strcmp(line, "k") == 0) {
                uint8_t prim;
                wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE; /* driver may leave this unwritten */
                esp_wifi_get_channel(&prim, &sc);
                TLOG("driver channel: primary=%u second=%d\n",
                       prim, (int)sc);
            } else if (line[0] == 'w') {
                int sec = 5;
                if (line[1] == ' ') {
                    sec = atoi(line + 2);
                }
                if (sec < 1 || sec > 60) {
                    sec = 5;
                }
                uint8_t prim;
                wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE; /* driver may leave this unwritten */
                esp_wifi_get_channel(&prim, &sc);
                s_sniff_cnt = 0;
                s_sniff_printed = 0;
                esp_wifi_set_promiscuous_rx_cb(sniff_cb);
                esp_wifi_set_promiscuous(true);
                TLOG("sniffing ch %u for %d s (≤130 B frames, ≤40 lines)...\n",
                       prim, sec);
                vTaskDelay(pdMS_TO_TICKS(sec * 1000));
                esp_wifi_set_promiscuous(false);
                esp_wifi_set_promiscuous_rx_cb(NULL);
                TLOG("sniff done: %lu frames total\n",
                       (unsigned long)s_sniff_cnt);
            } else if (strcmp(line, "t") == 0) {
                for (int i = 0; i < s_node_cnt; i++) {
                    time_sync_send(&s_nodes[i]);
                }
                if (s_node_cnt == 0) {
                    TLOG("no nodes registered yet\n");
                }
            } else if (strcmp(line, "p") == 0) {
                if (s_node_cnt > 0) {
                    param_set_send(&s_nodes[0]);
                } else {
                    TLOG("no nodes registered yet\n");
                }
            } else if (strcmp(line, "s") == 0) {
                registry_print();
            } else if (strcmp(line, "h") == 0 || strcmp(line, "help") == 0 ||
                       line[0] == '\0') {
                print_help();
            } else if (line[0] != '\0') {
                TLOG("unknown '%s' (h for help)\n", line);
            }
            len = 0;
            printf("> "); fflush(stdout);
            fflush(stdout);
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
            if (c >= 0x20 && c < 0x7f) {
                putchar(c);
                fflush(stdout);
            }
        }
    }
}
