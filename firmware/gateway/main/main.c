/* gateway main.c — DN003 wire chassis + registry + LED + console.
 *
 * Adapted from firmware/bench-espnow-master (issue 013) — the wire code
 * that was validated end-to-end against the real node: HELLO→HELLO_ACK
 * (+TIME_SYNC anchor), HEARTBEAT, TELEMETRY_BATCH decode with computed
 * per-sample timestamps (DN003: "written in order with computed
 * timestamps"; decimated interior points interpolate across the declared
 * span — the last sample lands exactly on end_ms so the commit watermark
 * covers batches exactly), EVENT, CONFIG_GET/DESC reassembly.
 *
 * Issue 015 changes vs bench:
 *   - BATCH_ACK is gated on the commit service's watermark (never
 *     "received"). Held batches wait for bracino/gateway/commit to cover
 *     end_ms; a retransmitted already-committed batch re-acks immediately.
 *   - Telemetry publishes as DN004 flat JSON (QoS 0) per sample.
 *   - ESP-NOW enable/disable is driven by net.c's DN004 state machine
 *     (ACTIVE only); this file never decides.
 *   - Channel is STA-derived: peers join on the current STA channel,
 *     whatever the provisioned AP uses. No channel constants.
 *   - LED (GPIO2): root-cause-first status table + frame-flicker overlay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "gw.h"
#include "bracino_log.h"

/* ---- pins ---- */

#define PIN_LED GPIO_NUM_2
#define LED_ACTIVE_HIGH 1
#define PIN_BUTTON GPIO_NUM_27 /* softap.c — chosen because GPIO12/0/2/15
                                * are strapping pins on classic ESP32 */

#define RX_QUEUE_LEN 8

/* ---- helpers ---- */

uint32_t gw_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* True if a <= b in 32-bit wraparound order (node_clock_ms domain). */
static bool wrap_le(uint32_t a, uint32_t b)
{
    return ((b - a) & 0xFFFFFFFFu) < 0x80000000u;
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

volatile uint32_t gw_frame_cnt;
gw_ct_t gw_ct;
gw_node_t gw_nodes[GW_MAX_NODES];
int gw_node_cnt;

typedef struct {
    uint8_t mac[6];
    int len;
    uint8_t data[ESPNOW_MAX_PAYLOAD];
} rx_msg_t;

static QueueHandle_t s_rx_q;
static SemaphoreHandle_t s_send_mu;
static SemaphoreHandle_t s_tx_done;
static SemaphoreHandle_t s_print_mu;

static const char *MSG_NAMES[13] = {
    "INVALID", "HELLO", "HELLO_ACK", "HEARTBEAT", "TIME_SYNC",
    "TELEMETRY_BATCH", "BATCH_ACK", "EVENT", "CONFIG_GET", "CONFIG_DESC",
    "PARAM_GET", "PARAM_SET", "PARAM_ACK",
};

/* ---- enum names (DN004: strings at the MQTT boundary) ---- */

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

static const char *ct_name(uint8_t v)
{
    switch (v) {
    case BBU_CT_STATE_OFF:            return "OFF";
    case BBU_CT_STATE_RUNNING:        return "RUNNING";
    case BBU_CT_STATE_NO_CURRENT_WARN: return "NO_CURRENT_WARN";
    case BBU_CT_STATE_NOT_FITTED:     return "NOT_FITTED";
    default:                          return "?";
    }
}

static const char *event_name(uint8_t tag)
{
    switch (tag) {
    case EVENT_FAULT_RAISED:  return "FAULT_RAISED";
    case EVENT_FAULT_CLEARED: return "FAULT_CLEARED";
    case EVENT_PARAM_CHANGED: return "PARAM_CHANGED";
    case EVENT_CONFIG_CHANGED: return "CONFIG_CHANGED";
    case EVENT_BATTERY_WARN:  return "BATTERY_WARN";
    default:                  return "?";
    }
}

/* ---- ISO UTC from epoch ms ---- */

static void iso_utc(uint64_t epoch_ms, char *out, size_t outlen)
{
    time_t sec = (time_t)(epoch_ms / 1000);
    struct tm tm;
    gmtime_r(&sec, &tm);
    strftime(out, outlen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ---- registry ---- */

gw_node_t *gw_node_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < gw_node_cnt; i++) {
        if (memcmp(gw_nodes[i].mac, mac, 6) == 0) {
            return &gw_nodes[i];
        }
    }
    return NULL;
}

gw_node_t *gw_node_by_role(uint8_t type, uint8_t id)
{
    for (int i = 0; i < gw_node_cnt; i++) {
        if (gw_nodes[i].in_use && gw_nodes[i].type == type &&
            gw_nodes[i].id == id) {
            return &gw_nodes[i];
        }
    }
    return NULL;
}

gw_node_t *gw_node_find_or_add(const uint8_t *mac, uint8_t type, uint8_t id)
{
    gw_node_t *n = gw_node_by_mac(mac);
    if (n != NULL) {
        return n;
    }
    if (gw_node_cnt >= GW_MAX_NODES) {
        return NULL;
    }
    n = &gw_nodes[gw_node_cnt++];
    memset(n, 0, sizeof(*n));
    memcpy(n->mac, mac, 6);
    n->type = type;
    n->id = id;
    n->in_use = true;
    return n;
}

/* ---- radio ---- */

static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status == ESP_NOW_SEND_SUCCESS) {
        gw_ct.tx_ok++;
    } else {
        gw_ct.tx_fail++;
    }
    xSemaphoreGive(s_tx_done); /* binary sem — NEVER the mutex */
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
    xQueueSend(s_rx_q, &rx, 0); /* overflow: drop (node retransmits) */
}

void gw_espnow_enable(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
}

void gw_espnow_disable(void)
{
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit(); /* peers die with it; re-added per-send */
}

/* Peers join on the CURRENT channel (STA-derived — never a constant).
 * channel=0 tells ESP-NOW to use the interface's channel. */
static void ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

bool gw_send_wait(const uint8_t *mac, const uint8_t *buf, size_t len)
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

static uint32_t s_tx_seq;

size_t gw_env_build(uint8_t *buf, uint8_t node_type, uint8_t node_id,
                    uint8_t msg_type, const void *payload, uint8_t plen)
{
    espnow_envelope_t *e = (espnow_envelope_t *)buf;
    e->proto_ver = ESPNOW_PROTO_VER;
    e->node_type = node_type;
    e->node_id = node_id;
    e->msg_type = msg_type;
    e->seq = (uint16_t)s_tx_seq++;
    e->node_clock_ms = gw_now_ms();
    e->flags = 0;
    e->boot_session = 1;
    e->payload_len = plen;
    if (plen) {
        memcpy(e->payload, payload, plen);
    }
    return ESPNOW_ENV_SIZE + plen;
}

/* HELLO_ACK TIME_SYNC TLV: epoch_s u32 LE @+2, epoch_ms u16 LE @+6.
 * (NOT @+4 — that clobbers the u32's high half; bench bring-up found it.) */
static size_t time_sync_tlv(uint8_t *tlv)
{
    tlv[0] = TLV_TIME_SYNC;
    tlv[1] = 6;
    uint64_t ms = gw_epoch_ms();
    wr_u32(tlv + 2, (uint32_t)(ms / 1000));
    wr_u16(tlv + 6, (uint16_t)(ms % 1000));
    return 8;
}

void gw_time_sync_send(gw_node_t *n)
{
    /* standalone TIME_SYNC payload is the RAW 6-byte struct — NOT the
     * TLV-wrapped form (bench bring-up: 8 B here made the node ignore it) */
    time_sync_t ts = { 0 };
    uint64_t ms = gw_epoch_ms();
    ts.epoch_s = (uint32_t)(ms / 1000);
    ts.epoch_ms = (uint16_t)(ms % 1000);
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = gw_env_build(buf, n->type, n->id, MSG_TIME_SYNC, &ts, sizeof(ts));
    if (!gw_time_valid()) {
        memset(&ts, 0, sizeof(ts)); /* epoch 0: node buffers, doesn't tx */
    }
    if (gw_send_wait(n->mac, buf, len)) {
        TLOG("> TIME_SYNC -> node(%u,%u)\n", n->type, n->id);
    }
}

/* ---- ack bookkeeping (main.c + net.c watermark path) ---- */

static void send_batch_ack(gw_node_t *n, uint32_t w)
{
    uint8_t payload[4];
    wr_u32(payload, w);
    uint8_t buf[ESPNOW_ENV_SIZE + 4];
    size_t len = gw_env_build(buf, n->type, n->id, MSG_BATCH_ACK, payload, 4);
    if (gw_send_wait(n->mac, buf, len)) {
        gw_ct.acks_sent++;
    }
}

void gw_ack_if_covered(gw_node_t *n, uint32_t w)
{
    if (n->have_pending && wrap_le(n->pending_ms, w)) {
        n->have_pending = false;
        send_batch_ack(n, w);
        TLOG("acked node(%u,%u) batch end=%lu on watermark %lu "
             "(held %lu s)\n", n->type, n->id,
             (unsigned long)n->pending_ms, (unsigned long)w,
             (unsigned long)((gw_now_ms() - n->pending_since_ms) / 1000));
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
    gw_node_t *n = gw_node_find_or_add(m->mac, e->node_type, e->node_id);
    if (n == NULL) {
        TLOG("node(%u,%u) HELLO but registry full — no ack\n",
             e->node_type, e->node_id);
        return;
    }
    n->liveness_s = 2;
    n->last_seen_ms = gw_now_ms();

    /* node reboot: clock restarted — void stale commit/pending state */
    if (n->last_boot != e->boot_session) {
        n->have_commit = false;
        n->have_pending = false;
        n->last_boot = e->boot_session;
    }

    TLOG("HELLO node(%u,%u) mac " MACSTR " cfg_ver=%u seq=%u boot=%u\n",
         e->node_type, e->node_id, MAC2STR(m->mac), config_ver, e->seq,
         e->boot_session);

    /* HELLO_ACK: always carries a fresh TIME_SYNC (DN003). Epoch 0 when
     * time invalid — the node buffers instead of transmitting. */
    uint8_t tlv[8];
    size_t tlen = time_sync_tlv(tlv);
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = gw_env_build(buf, e->node_type, e->node_id, MSG_HELLO_ACK,
                              tlv, (uint8_t)tlen);
    if (!gw_send_wait(n->mac, buf, len)) {
        TLOG("HELLO_ACK send failed\n");
        return;
    }
    /* Anchor: gateway wall time now, vs node clock in the HELLO envelope. */
    n->anchor.have = true;
    n->anchor.epoch_total_ms = gw_epoch_ms(); /* 0 if time invalid */
    n->anchor.node_clock_ms = e->node_clock_ms;
    n->anchor.boot_session = e->boot_session;

    bool known = n->config_fetched && n->config_ver == config_ver;
    n->config_ver = config_ver;
    if (!known) {
        uint8_t get_buf[ESPNOW_ENV_SIZE];
        size_t glen = gw_env_build(get_buf, n->type, n->id, MSG_CONFIG_GET,
                                   NULL, 0);
        gw_send_wait(n->mac, get_buf, glen);
    }
}

static void publish_telemetry(gw_node_t *n, const espnow_envelope_t *e,
                              const telemetry_batch_hdr_t *h,
                              const uint8_t *samples, uint16_t count)
{
    if (!gw_time_valid() || !n->anchor.have || n->anchor.epoch_total_ms == 0) {
        return; /* no anchor: nothing sane to stamp; batch stays unacked */
    }
    uint32_t span = h->end_ms - h->start_ms; /* unsigned wrap = node clock wrap-safe */
    char topic[48], json[320], ts[32];

    for (uint16_t i = 0; i < count; i++) {
        const bbu_telemetry_v1_t *s =
            (const bbu_telemetry_v1_t *)(samples + (size_t)i * sizeof(*s));
        /* DN003 "computed timestamps": interpolate across the declared
         * span; i = count-1 lands exactly on end_ms. */
        uint32_t cap = count > 1
                           ? h->start_ms + (uint32_t)((uint64_t)span * i /
                                                      (count - 1))
                           : h->end_ms;
        int64_t d = (int64_t)(int32_t)(cap - n->anchor.node_clock_ms);
        uint64_t ems = n->anchor.epoch_total_ms + (uint64_t)d;
        iso_utc(ems, ts, sizeof(ts));

        snprintf(json, sizeof(json),
                 "{\"mode\":\"%s\",\"relay_state\":%u,\"ct_state\":\"%s\","
                 "\"t_tpo\":%.1f,\"t_tpu\":%.1f,\"t_amb\":%.1f,"
                 "\"fault_flags\":%u,\"boot_session\":%u,"
                 "\"capture_ms\":%lu,\"node_ts\":\"%s\","
                 "\"batch_end_ms\":%lu,\"batch_count\":%u}",
                 mode_name(s->mode), s->relay_state, ct_name(s->ct_state),
                 (double)s->t_tpo_x10 / 10.0, (double)s->t_tpu_x10 / 10.0,
                 (double)s->t_amb_x10 / 10.0, s->fault_flags,
                 e->boot_session, (unsigned long)cap, ts,
                 (unsigned long)h->end_ms, h->count);
        snprintf(topic, sizeof(topic), "bracino/node/%u/%u/telemetry",
                 e->node_type, e->node_id);
        if (gw_mqtt_publish(topic, json, 0, false)) {
            gw_ct.samples_published++;
        }
    }
}

static void handle_batch(const rx_msg_t *m, const espnow_envelope_t *e,
                         gw_node_t *n)
{
    if (m->len < ESPNOW_ENV_SIZE + 11) {
        TLOG("TELEMETRY_BATCH malformed (short header)\n");
        return;
    }
    const telemetry_batch_hdr_t *h =
        (const telemetry_batch_hdr_t *)(m->data + ESPNOW_ENV_SIZE);
    uint16_t count = rd_u16((const uint8_t *)&h->count);
    size_t plen = m->len - ESPNOW_ENV_SIZE;
    size_t expected = 11 + (size_t)count * sizeof(bbu_telemetry_v1_t);
    if (count > 0 && plen < expected) {
        TLOG("TELEMETRY_BATCH malformed (count %u, plen %u)\n",
             count, (unsigned)plen);
        return;
    }

    bool dup = e->seq == n->last_batch_seq &&
               e->boot_session == n->last_batch_boot;
    n->last_batch_seq = e->seq;
    n->last_batch_boot = e->boot_session;

    if (dup) {
        /* retransmit after lost ack: re-ack if already committed,
         * otherwise keep waiting (pending slot is untouched) */
        if (n->have_commit && wrap_le(h->end_ms, n->commit_ms)) {
            send_batch_ack(n, n->commit_ms);
        } else {
            gw_ct.acks_held++;
        }
        return;
    }

    publish_telemetry(n, e, h, m->data + ESPNOW_ENV_SIZE + 11, count);

    if (!gw_time_valid()) {
        gw_ct.acks_held++;
        return; /* defensive — a never-anchored node doesn't transmit */
    }
    if (n->have_commit && wrap_le(h->end_ms, n->commit_ms)) {
        send_batch_ack(n, n->commit_ms); /* watermark already covers it */
    } else {
        n->have_pending = true;
        n->pending_ms = h->end_ms;
        n->pending_since_ms = gw_now_ms();
        gw_ct.acks_held++;
    }
}

static void handle_event(const rx_msg_t *m, const espnow_envelope_t *e)
{
    if (m->len < ESPNOW_ENV_SIZE + 2) {
        return;
    }
    uint8_t tag = m->data[ESPNOW_ENV_SIZE];
    uint8_t vlen = m->data[ESPNOW_ENV_SIZE + 1];
    const uint8_t *v = m->data + ESPNOW_ENV_SIZE + 2;

    char json[224] = { 0 };
    if (tag == EVENT_FAULT_RAISED || tag == EVENT_FAULT_CLEARED) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"%s\",\"fault_id\":%u}", event_name(tag),
                 vlen >= 1 ? v[0] : 0);
    } else if (tag == EVENT_PARAM_CHANGED && vlen >= 1) {
        char hex[3 * 12 + 1] = "";
        for (uint8_t i = 1; i < vlen && i < 13; i++) {
            char b[8];
            snprintf(b, sizeof(b), "%02x ", v[i]);
            strncat(hex, b, sizeof(hex) - strlen(hex) - 1);
        }
        snprintf(json, sizeof(json),
                 "{\"event\":\"PARAM_CHANGED\",\"param_id\":%u,\"value_hex\":\"%s\"}",
                 v[0], hex);
    } else if (tag == EVENT_CONFIG_CHANGED && vlen >= 1) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"CONFIG_CHANGED\",\"config_ver\":%u}", v[0]);
    } else if (tag == EVENT_BATTERY_WARN && vlen >= 1) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"BATTERY_WARN\",\"level_pct\":%u}", v[0]);
    } else {
        char hex[3 * 8 + 1] = "";
        for (uint8_t i = 0; i < vlen && i < 8; i++) {
            char b[8];
            snprintf(b, sizeof(b), "%02x ", v[i]);
            strncat(hex, b, sizeof(hex) - strlen(hex) - 1);
        }
        snprintf(json, sizeof(json),
                 "{\"event\":\"UNKNOWN_%02x\",\"value_hex\":\"%s\"}", tag, hex);
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "bracino/node/%u/%u/event",
             e->node_type, e->node_id);
    gw_mqtt_publish(topic, json, 1, false);
    TLOG("EVENT %s\n", json);
}

static void handle_config_desc(const rx_msg_t *m, const espnow_envelope_t *e,
                               gw_node_t *n)
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
        n->frag_total = 0;
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
    n->frag_last_ms = gw_now_ms();

    if (idx + 1 < total) {
        TLOG("CONFIG_DESC frag %u/%u (%u B, more)\n", idx, total,
             (unsigned)chunk);
        return;
    }

    TLOG("CONFIG_DESC complete (%u B):\n", (unsigned)n->desc_len);
    tlv_cur_t c = { n->desc_buf, (int)n->desc_len };
    uint8_t tag, vlen;
    const uint8_t *val;
    while (tlv_next(&c, &tag, &val, &vlen)) {
        if (tag != TLV_PARAM_DESCRIPTOR || vlen < 16) {
            continue;
        }
        /* pass 1: log the table; retained …/config topic is pass 2 */
        TLOG("  param id=%2u type=%u flags=%u min=%lu max=%lu step=%lu\n",
             val[0], val[1], val[2], (unsigned long)rd_u32(val + 3),
             (unsigned long)rd_u32(val + 7), (unsigned long)rd_u32(val + 11));
    }
    n->frag_total = 0;
    n->config_fetched = true;
}

static void handle_rx(const rx_msg_t *m)
{
    const espnow_envelope_t *e = (const espnow_envelope_t *)m->data;
    if (m->len < ESPNOW_ENV_SIZE || e->proto_ver != ESPNOW_PROTO_VER ||
        e->msg_type == MSG_INVALID || e->msg_type > MSG_PARAM_ACK) {
        return;
    }
    gw_ct.rx[e->msg_type]++;

    gw_node_t *n = gw_node_by_mac(m->mac);
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
            n->last_seen_ms = gw_now_ms();
        } else {
            TLOG("HB from unregistered MAC " MACSTR " — waiting for HELLO\n",
                 MAC2STR(m->mac));
        }
        break;
    case MSG_TELEMETRY_BATCH:
        if (n) {
            n->last_seen_ms = gw_now_ms();
            handle_batch(m, e, n);
        } else {
            TLOG("BATCH from unregistered MAC — dropped (node re-HELLOs)\n");
        }
        break;
    case MSG_EVENT:
        if (n) {
            n->last_seen_ms = gw_now_ms();
        }
        handle_event(m, e);
        break;
    case MSG_CONFIG_DESC:
        if (n) {
            handle_config_desc(m, e, n);
        }
        break;
    default:
        break; /* BATCH_ACK/HELLO_ACK/TIME_SYNC are gateway→node */
    }
}

/* ---- proc task: queue drain + liveness warnings ---- */

static void proc_task(void *arg)
{
    (void)arg;
    for (;;) {
        rx_msg_t m;
        if (xQueueReceive(s_rx_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            xSemaphoreTakeRecursive(s_print_mu, portMAX_DELAY);
            gw_frame_cnt++;
            handle_rx(&m);
            xSemaphoreGiveRecursive(s_print_mu);
        }

        for (int i = 0; i < gw_node_cnt; i++) {
            gw_node_t *n = &gw_nodes[i];
            if (!n->in_use || n->liveness_s == 0 || n->last_seen_ms == 0) {
                continue;
            }
            uint32_t expect_ms = (uint32_t)n->liveness_s * 3u * 1000u;
            if (!n->flagged_unreach &&
                gw_now_ms() - n->last_seen_ms > expect_ms) {
                n->flagged_unreach = true;
                TLOG("!! node(%u,%u) silent for %lus\n", n->type, n->id,
                     (unsigned long)((gw_now_ms() - n->last_seen_ms) / 1000));
            }
        }
    }
}

/* ---- LED: root-cause-first status table + frame-flicker overlay ---- */

static bool node_seen_within(uint32_t ms)
{
    for (int i = 0; i < gw_node_cnt; i++) {
        if (gw_nodes[i].in_use &&
            gw_now_ms() - gw_nodes[i].last_seen_ms < ms) {
            return true;
        }
    }
    return false;
}

static bool any_pending_stale(uint32_t ms)
{
    for (int i = 0; i < gw_node_cnt; i++) {
        if (gw_nodes[i].in_use && gw_nodes[i].have_pending &&
            gw_now_ms() - gw_nodes[i].pending_since_ms > ms) {
            return true;
        }
    }
    return false;
}

/* LED table (015, settled): 1=no wifi · 2=no broker · 3=no time ·
 * 4=no acks · 5=no node seen · solid=all clear (flicker on frames) */
static int led_eval(void)
{
    if (!gw_wifi_up()) {
        return 1;
    }
    if (!gw_broker_up()) {
        return 2;
    }
    if (!gw_time_valid()) {
        return 3;
    }
    if (gw_health_age_ms() > 90000 || any_pending_stale(5000)) {
        return 4;
    }
    if (!node_seen_within(60000)) {
        return 5;
    }
    return 0;
}

static void led_write(bool on)
{
#if LED_ACTIVE_HIGH
    gpio_set_level(PIN_LED, on);
#else
    gpio_set_level(PIN_LED, !on);
#endif
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t seen_frame = 0;
    uint32_t blip_until = 0;
    for (;;) {
        int pat = led_eval();
        if (pat == 0) {
            if (gw_frame_cnt != seen_frame) {
                seen_frame = gw_frame_cnt;
                blip_until = gw_now_ms() + 60;
            }
            led_write(gw_now_ms() >= blip_until);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            led_write(false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            for (int i = 0; i < pat; i++) {
                led_write(true);
                vTaskDelay(pdMS_TO_TICKS(150));
                led_write(false);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }
    }
}

/* ---- console ---- */

static void registry_print(void)
{
    uint8_t prim = 0;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&prim, &sc);

    TLOG("mode=%s ch=%u time=%s (src %s)\n",
         gw_mode == GW_ACTIVE ? "ACTIVE" : "WAIT_BACKEND", prim,
         gw_time_valid() ? "valid" : "INVALID", gw_time_source());
    char bstr[72];
    gw_net_broker_str(bstr, sizeof(bstr));
    TLOG("wifi=%d ssid='%s' broker=%d (%s) health_age=%lus\n",
         gw_wifi_up(), gw_net_ssid(), gw_broker_up(), bstr,
         gw_health_age_ms() == UINT32_MAX
             ? 0
             : (unsigned long)(gw_health_age_ms() / 1000));
    for (int i = 0; i < gw_node_cnt; i++) {
        gw_node_t *n = &gw_nodes[i];
        TLOG("  node(%u,%u) mac=" MACSTR " cfg_ver=%u last_seen=%lus ago "
             "boot=%u\n",
             n->type, n->id, MAC2STR(n->mac), n->config_ver,
             n->last_seen_ms ? (unsigned long)((gw_now_ms() - n->last_seen_ms) / 1000) : 0,
             n->last_boot);
        if (n->anchor.have) {
            char ts[32] = "?";
            if (n->anchor.epoch_total_ms) {
                time_t sec = (time_t)(n->anchor.epoch_total_ms / 1000);
                struct tm tm;
                gmtime_r(&sec, &tm);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            }
            TLOG("    anchor: gw_time=%s @ node_clock=%lu ms (boot %u)\n",
                 ts, (unsigned long)n->anchor.node_clock_ms,
                 n->anchor.boot_session);
        }
        TLOG("    commit: have=%d ms=%ld pending=%d end=%ld held %lus\n",
             n->have_commit, (long)n->commit_ms, n->have_pending,
             (long)n->pending_ms,
             n->have_pending
                 ? (unsigned long)((gw_now_ms() - n->pending_since_ms) / 1000)
                 : 0);
    }
    TLOG("counters: rx=");
    for (int t = 1; t <= MSG_PARAM_ACK; t++) {
        if (gw_ct.rx[t]) {
            printf("%s=%lu ", MSG_NAMES[t], (unsigned long)gw_ct.rx[t]);
        }
    }
    TLOG("\n  tx_ok=%lu tx_fail=%lu acks_sent=%lu held=%lu "
         "samples_pub=%lu\n",
         (unsigned long)gw_ct.tx_ok, (unsigned long)gw_ct.tx_fail,
         (unsigned long)gw_ct.acks_sent, (unsigned long)gw_ct.acks_held,
         (unsigned long)gw_ct.samples_published);
    if (!gw_time_valid()) {
        TLOG("  TIME INVALID — nodes buffer but don't transmit (DN003). "
             "Set via sntp/mqtt/serial: n <unix_s>\n");
    }
}

static void print_help(void)
{
    TLOG(
        "gateway (015): ESP-NOW bridge, commit-watermark-gated BATCH_ACK\n"
        "  n <unix_s>  set epoch (fallback; sntp/mqtt preferred)\n"
        "  b <host> [port]  set broker (NVS; applies immediately)\n"
        "  t           push TIME_SYNC to all nodes\n"
        "  s           status (also GPIO27 long-press → softAP pages)\n"
        "  h           this help\n");
}

static void cmd_set_broker(char *args)
{
    char host[64] = "";
    int port = 1883;
    if (sscanf(args, "%63s %d", host, &port) < 1) {
        TLOG("usage: b <host> [port]\n");
        return;
    }
    gw_nvs_set_str("bhost", host);
    gw_nvs_set_u32("bport", (uint32_t)port);
    TLOG("broker -> %s:%d (NVS)\n", host, port);
    gw_net_mqtt_restart();
}

static void console_loop(void)
{
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
                TLOG("cmd: %s\n", line);
            }
            if (strncmp(line, "n ", 2) == 0) {
                char *end = NULL;
                unsigned long long sec = strtoull(line + 2, &end, 10);
                if (end != line + 2 && sec > 0) {
                    gw_time_set_unix(sec, 0);
                    /* s_time_src is net.c-static; time_set_unix leaves it
                     * — reflect "serial" via a small export below */
                    gw_time_mark_serial();
                    char ts[32];
                    time_t s = (time_t)sec;
                    struct tm tm;
                    gmtime_r(&s, &tm);
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
                    TLOG("epoch set: %s UTC\n", ts);
                } else {
                    TLOG("usage: n <unix_seconds>\n");
                }
            } else if (line[0] == 'b' && line[1] == ' ') {
                cmd_set_broker(line + 2);
            } else if (strcmp(line, "t") == 0) {
                for (int i = 0; i < gw_node_cnt; i++) {
                    if (gw_nodes[i].in_use) {
                        gw_time_sync_send(&gw_nodes[i]);
                    }
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

/* ---- app_main ---- */

void app_main(void)
{
    s_rx_q = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_msg_t));
    configASSERT(s_rx_q);
    s_send_mu = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_send_mu);
    s_tx_done = xSemaphoreCreateBinary();
    configASSERT(s_tx_done);
    s_print_mu = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_print_mu);

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    led_write(false);

    /* uplink first (wifi/broker/time/state machine) */
    gw_net_init();

    xTaskCreate(proc_task, "proc", 12288, NULL, 3, NULL);
    xTaskCreate(led_task, "led", 3072, NULL, 1, NULL);
    gw_softap_start_button();

    TLOG("\n# bracino gateway (issue 015) — DN003 wire + DN004 shape\n");
    print_help();
    printf("> "); fflush(stdout);

    console_loop();
}
