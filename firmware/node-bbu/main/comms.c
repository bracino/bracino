/*
 * comms.c — DN003 ESP-NOW client for node-bbu.
 *
 * Contract highlights (docs/DESIGN_NOTE_003_espnow_node_schema.md):
 *  - unified FIFO; every sample enters at capture, transmit only from FIFO
 *  - stop-and-wait: one outstanding TELEMETRY_BATCH; BATCH_ACK(end_ms)
 *    watermark trims entries <= watermark; 2 s timeout retransmits
 *  - no telemetry TX before first non-zero TIME_SYNC this boot session
 *  - HELLO broadcast on channel scan (cached → 1/6/11 → 1..13), bounded:
 *    ~250 ms dwell/channel, ~5 s/attempt, >=10 s rest between attempts
 *  - 3 consecutive failed unicast exchanges => unreachable => scan
 *  - control loop only ever calls comms_offer_sample (enqueue, no I/O)
 *
 * Bench knobs (serial): comms on|off|st, ident T I, tel <s>, ring <n>.
 * TTL simplification (DN003 PARAM_SET): replay guarded by monotonic
 * admin_seq (EXPIRED otherwise); wall-clock TTL needs gateway-side
 * issuance timestamps (DN005) — noted in issue 011.
 */

#include "comms.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "espnow_schema.h"
#include "bracino_log.h"
#include "params.h"

/* ---- tuning (DN003 start values) ---- */

#ifndef COMMS_RING_SAMPLES
/* DN003 targets a 90 KB ring (7680 samples ~ 32 h); that is tight next to
 * WiFi + UI on a C3. 4096 samples (~17 h at 15 s) is the phase-1 default
 * pending a heap measurement. Override at build time or via serial
 * `ring <n>` (bench knob; reallocates the ring EMPTY). */
#define COMMS_RING_SAMPLES 4096
#endif

#define COMMS_TASK_STACK   8192
#define COMMS_TASK_PRIO    1    /* below monitor(2); never in the loop path */

#define HEARTBEAT_S        2    /* DN003 ALWAYS_ON target ~2 s */
#define TX_CB_WAIT_MS      500  /* MAC-level ack wait on unicast sends */
#define ACK_TIMEOUT_MS     2000 /* BATCH_ACK wait (DN003 start value) */
#define MAX_CONSEC_FAIL    3    /* failed unicast exchanges => unreachable */

#define SCAN_DWELL_MS      250
#define SCAN_BUDGET_MS     5000
#define SCAN_REST_MS       10000
#define SCAN_PRIOR_CH      { 1, 6, 11 }

#define RECV_QUEUE_LEN     8
#define EVENT_QUEUE_LEN    8
#define PARAM_DEFAULT_TTL_S 60

#define DEFAULT_SAMPLE_S   15

#define DESC_BUF_SIZE      512  /* CONFIG_DESC TLV stream before fragmentation */

/* ---- types ---- */

typedef struct {
    bbu_telemetry_v1_t s;
    uint32_t capture_ms;
} fifo_ent_t;

typedef enum { CS_SCANNING = 0, CS_ONLINE } comms_state_t;

typedef struct {
    uint8_t mac[6];
    int len;
    uint8_t ch; /* primary channel at rx time — bind decisions need it,
                 * because scan pops stale frames from later dwells */
    uint8_t data[ESPNOW_MAX_PAYLOAD];
} rx_msg_t;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t val[10];
} ev_msg_t;

/* ---- state ---- */

static QueueHandle_t s_rx_q;
static SemaphoreHandle_t s_tx_done; /* binary; send-cb signal */

static bool s_enabled;         /* NVS comms_enabled */
static bool s_radio_up;
static comms_state_t s_state = CS_SCANNING;

static uint8_t s_node_type = NODE_TYPE_BBU;
static uint8_t s_node_id = 1;
static uint8_t s_boot_session;
static uint16_t s_seq;

static uint8_t s_own_mac[6];
static uint8_t s_gw_mac[6];
static uint8_t s_channel;      /* bound channel */
static bool s_bound;

static uint32_t s_anchor_epoch_s;   /* 0 = never anchored */
static uint16_t s_anchor_epoch_ms;
static uint32_t s_anchor_clock_ms;

static bool s_batch_out;
static uint32_t s_batch_sent_ms;
static uint8_t s_batch_buf[ESPNOW_MAX_PAYLOAD];
static size_t s_batch_len;
static uint32_t s_batch_end_ms;

static uint32_t s_last_capture_ms;
static uint32_t s_sample_period_s = DEFAULT_SAMPLE_S;
static uint32_t s_next_heartbeat_ms;
static uint32_t s_next_scan_ms;
static uint8_t s_consec_fail;
static uint32_t s_admin_seq;   /* PARAM_SET replay guard */

static ev_msg_t s_ev_q[EVENT_QUEUE_LEN];
static uint8_t s_ev_head, s_ev_cnt;

/* counters */
static struct {
    uint32_t rx, rx_drop;
    uint32_t tx_ok, tx_fail;
    uint32_t acks, retrans;
    uint32_t decim_passes, ev_sent, ev_drop;
} s_ct;

static const char *const TAG_STATE[] = { "SCANNING", "ONLINE" };

static const uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* FIFO ring */
static fifo_ent_t *s_ring;
static uint16_t s_ring_cap;
static uint16_t s_ring_head;   /* oldest entry */
static uint16_t s_ring_cnt;
static portMUX_TYPE s_fifo_mu = portMUX_INITIALIZER_UNLOCKED;

/* ---- small helpers ---- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Build an envelope into buf; returns total frame length. */
static size_t env_build(uint8_t *buf, uint8_t msg_type, uint8_t flags,
                        const void *payload, uint8_t plen)
{
    espnow_envelope_t *e = (espnow_envelope_t *)buf;
    e->proto_ver = ESPNOW_PROTO_VER;
    e->node_type = s_node_type;
    e->node_id = s_node_id;
    e->msg_type = msg_type;
    e->seq = s_seq++;
    e->node_clock_ms = now_ms();
    e->flags = flags;
    e->boot_session = s_boot_session;
    e->payload_len = plen;
    if (plen) {
        memcpy(e->payload, payload, plen);
    }
    return ESPNOW_ENV_SIZE + plen;
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

/* Wire length of a param value for its descriptor type. */
static size_t param_wire_len(const bbu_param_desc_t *d)
{
    switch (d->type) {
    case PTYPE_U32:     return 4;
    case PTYPE_I16_X10: return 2;
    case PTYPE_ENUM:    return 1;
    default:            return 0;
    }
}

size_t comms_encode_param_value(uint8_t id, int32_t raw, uint8_t *out)
{
    const bbu_param_desc_t *d = params_desc_by_id(id);
    if (d == NULL || out == NULL) {
        return 0;
    }
    switch (d->type) {
    case PTYPE_U32:
        wr_u32(out, (uint32_t)raw);
        return 4;
    case PTYPE_I16_X10:
        wr_u16(out, (uint16_t)(int16_t)raw);
        return 2;
    case PTYPE_ENUM:
        out[0] = (uint8_t)raw;
        return 1;
    default:
        return 0;
    }
}

/* ---- NVS ---- */

#define COMMS_NS "comms"

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(COMMS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "en", s_enabled ? 1 : 0);
    nvs_set_u8(h, "ntype", s_node_type);
    nvs_set_u8(h, "nid", s_node_id);
    nvs_set_u8(h, "ch", s_channel);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(COMMS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v;
    if (nvs_get_u8(h, "en", &v) == ESP_OK) {
        s_enabled = v != 0;
    }
    if (nvs_get_u8(h, "ntype", &v) == ESP_OK && v != 0) {
        s_node_type = v;
    }
    if (nvs_get_u8(h, "nid", &v) == ESP_OK) {
        s_node_id = v;
    }
    if (nvs_get_u8(h, "ch", &v) == ESP_OK && v >= 1 && v <= 13) {
        s_channel = v;
    }
    nvs_close(h);
}

/* ---- radio ---- */

static volatile bool s_tx_delivered; /* radio-level ack of last unicast: a
                                      * MAC NAK means the gateway never got
                                      * the frame (e.g. it hopped channel) */

static void radio_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    bool ok = status == ESP_NOW_SEND_SUCCESS;
    if (ok) {
        s_ct.tx_ok++;
    } else {
        s_ct.tx_fail++;
    }
    s_tx_delivered = ok; /* MAC-level ack: a NAK here means the gateway
                          * never got the frame (e.g. it hopped channel) */
    xSemaphoreGive(s_tx_done);
}

static void radio_recv_cb(const esp_now_recv_info_t *info,
                          const uint8_t *data, int len)
{
    rx_msg_t m;
    if (data == NULL || len <= 0 || len > ESPNOW_MAX_PAYLOAD || info == NULL) {
        return;
    }
    /* queue broadcasts (HELLO from other nodes; ignored downstream) and
     * unicast addressed to us; drop anything else addressed */
    bool to_us = memcmp(info->des_addr, s_own_mac, 6) == 0;
    bool bcast = memcmp(info->des_addr, "\xff\xff\xff\xff\xff\xff", 6) == 0;
    if (!to_us && !bcast) {
        return;
    }
    memcpy(m.mac, info->src_addr, 6);
    m.len = len;
    uint8_t prim;
    wifi_second_chan_t sec;
    m.ch = (esp_wifi_get_channel(&prim, &sec) == ESP_OK) ? prim : 0;
    memcpy(m.data, data, (size_t)len);
    s_ct.rx++;
    if (xQueueSend(s_rx_q, &m, 0) != pdTRUE) {
        s_ct.rx_drop++; /* congested: drop, never block the WiFi task */
    }
}

static void set_channel(uint8_t ch)
{
    esp_wifi_set_promiscuous(true);
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (err != ESP_OK) {
        TLOG("comms: !! set_channel(%u) FAILED: %s — TX may stay on prior "
               "channel\n", ch, esp_err_to_name(err));
        return;
    }
    /* Keep the broadcast peer's channel in sync: if the driver latches the
     * peer's TX channel at add_peer time, every HELLO would leave on the
     * boot channel no matter what set_channel does. (This exact symptom —
     * heard on ch 1, silent on ch 6 — cost the first bench session.) */
    if (esp_now_is_peer_exist(BCAST_MAC)) {
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, BCAST_MAC, 6);
        peer.channel = ch;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        err = esp_now_mod_peer(&peer);
        if (err != ESP_OK) {
            TLOG("comms: !! peer channel sync to %u FAILED: %s\n",
                   ch, esp_err_to_name(err));
        }
    }
}

static esp_err_t radio_start(void)
{
    if (s_radio_up) {
        return ESP_OK;
    }
    /* netif/event loop may already exist (ERROR_CHECK nodes share init) */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); /* ESP-NOW rx latency */
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_own_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(radio_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(radio_recv_cb));

    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, bcast, 6);
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    s_radio_up = true;
    TLOG("comms: radio up, node(%u,%u) mac " MACSTR "\n",
           s_node_type, s_node_id, MAC2STR(s_own_mac));
    return ESP_OK;
}

static void add_gw_peer(const uint8_t *mac)
{
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
    esp_now_add_peer(&peer);
}

/* Send and wait for the MAC-level callback. Returns radio-level success. */
/* Radio-level delivery: a callback that reports a MAC NAK (gateway gone,
 * channel mismatch) is a FAILURE — ignoring it here meant heartbeats to a
 * moved gateway "succeeded" forever and the node never rescanned. */
static volatile bool s_tx_delivered;

static bool send_wait(const uint8_t *mac, const uint8_t *buf, size_t len)
{
    xSemaphoreTake(s_tx_done, 0); /* drain stale signal */
    s_tx_delivered = false;
    if (esp_now_send(mac, buf, len) != ESP_OK) {
        s_ct.tx_fail++;
        return false;
    }
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(TX_CB_WAIT_MS)) != pdTRUE) {
        return false;
    }
    return s_tx_delivered;
}

/* ---- FIFO (spinlock: capture runs in the 1 Hz monitor task) ---- */

static uint16_t fifo_count(void)
{
    uint16_t n;
    portENTER_CRITICAL(&s_fifo_mu);
    n = s_ring_cnt;
    portEXIT_CRITICAL(&s_fifo_mu);
    return n;
}

/* Drop every other entry in the oldest half (DN003 decimation policy).
 * Decimation only ever touches unacked entries — during an outage all
 * entries are unacked; while draining, acks outpace capture. */
static void fifo_decimate_locked(void)
{
    uint16_t half = s_ring_cnt / 2;
    uint16_t keep = 0;
    for (uint16_t i = 0; i < s_ring_cnt; i++) {
        if (i < half && (i & 1) == 0) {
            continue; /* drop 0,2,4,... of the oldest half */
        }
        if (keep != i) {
            s_ring[(s_ring_head + keep) % s_ring_cap] =
                s_ring[(s_ring_head + i) % s_ring_cap];
        }
        keep++;
    }
    s_ring_cnt = keep;
    s_ct.decim_passes++;
}

static void fifo_push(const bbu_telemetry_v1_t *s, uint32_t capture_ms)
{
    portENTER_CRITICAL(&s_fifo_mu);
    if (s_ring_cnt >= s_ring_cap) {
        fifo_decimate_locked();
        if (s_ring_cnt >= s_ring_cap) {
            /* pathological tiny ring: overwrite oldest */
            s_ring_head = (s_ring_head + 1) % s_ring_cap;
            s_ring_cnt--;
        }
    }
    uint16_t idx = (s_ring_head + s_ring_cnt) % s_ring_cap;
    s_ring[idx].s = *s;
    s_ring[idx].capture_ms = capture_ms;
    s_ring_cnt++;
    portEXIT_CRITICAL(&s_fifo_mu);
}

static void fifo_trim(uint32_t watermark)
{
    portENTER_CRITICAL(&s_fifo_mu);
    while (s_ring_cnt > 0 && s_ring[s_ring_head].capture_ms <= watermark) {
        s_ring_head = (s_ring_head + 1) % s_ring_cap;
        s_ring_cnt--;
    }
    portEXIT_CRITICAL(&s_fifo_mu);
}

/* ---- events (best-effort; dropped + counted during outage) ---- */

void comms_offer_event(uint8_t event_id, const uint8_t *value, uint8_t len)
{
    if (len > sizeof(s_ev_q[0].val)) {
        return;
    }
    portENTER_CRITICAL(&s_fifo_mu);
    if (s_ev_cnt >= EVENT_QUEUE_LEN) {
        portEXIT_CRITICAL(&s_fifo_mu);
        s_ct.ev_drop++;
        return;
    }
    ev_msg_t *e = &s_ev_q[(s_ev_head + s_ev_cnt) % EVENT_QUEUE_LEN];
    e->id = event_id;
    e->len = len;
    memcpy(e->val, value, len);
    s_ev_cnt++;
    portEXIT_CRITICAL(&s_fifo_mu);
}

static bool ev_pop(ev_msg_t *out)
{
    bool got = false;
    portENTER_CRITICAL(&s_fifo_mu);
    if (s_ev_cnt > 0) {
        *out = s_ev_q[s_ev_head];
        s_ev_head = (s_ev_head + 1) % EVENT_QUEUE_LEN;
        s_ev_cnt--;
        got = true;
    }
    portEXIT_CRITICAL(&s_fifo_mu);
    return got;
}

/* ---- HELLO / discovery ---- */

static size_t hello_build(uint8_t *buf)
{
    uint8_t tlv[32];
    size_t n = 0;
    tlv[n++] = TLV_NODE_TYPE;      tlv[n++] = 1; tlv[n++] = s_node_type;
    tlv[n++] = TLV_NODE_ID;        tlv[n++] = 1; tlv[n++] = s_node_id;
    tlv[n++] = TLV_LIVENESS_MODE;  tlv[n++] = 1; tlv[n++] = LIVENESS_ALWAYS_ON;
    tlv[n++] = TLV_LIVENESS_PARAM; tlv[n++] = 2; wr_u16(tlv + n, HEARTBEAT_S); n += 2;
    tlv[n++] = TLV_SCHEMA_VER;     tlv[n++] = 1; tlv[n++] = BBU_SCHEMA_VER;
    tlv[n++] = TLV_CONFIG_VER;     tlv[n++] = 1; tlv[n++] = BBU_CONFIG_VER;
    tlv[n++] = TLV_WAKE_REASON;    tlv[n++] = 1; tlv[n++] = WAKE_POWER_ON;
    return env_build(buf, MSG_HELLO, 0, tlv, (uint8_t)n);
}

static void apply_time_sync(uint32_t epoch_s, uint16_t epoch_ms)
{
    if (epoch_s == 0) {
        TLOG("comms: TIME_SYNC epoch=0 — gateway has no time yet\n");
        return;
    }
    s_anchor_epoch_s = epoch_s;
    s_anchor_epoch_ms = epoch_ms;
    s_anchor_clock_ms = now_ms();
    TLOG("comms: anchored epoch=%lu.%03u at clock %lu ms\n",
           (unsigned long)s_anchor_epoch_s, s_anchor_epoch_ms,
           (unsigned long)s_anchor_clock_ms);
}

/* HELLO_ACK: always carries TIME_SYNC; may carry REJECT_REASON. */
static bool parse_hello_ack(const rx_msg_t *m, uint8_t expect_ch)
{
    const espnow_envelope_t *e = (const espnow_envelope_t *)m->data;
    if (m->len < ESPNOW_ENV_SIZE + 2 || e->msg_type != MSG_HELLO_ACK) {
        return false;
    }
    tlv_cur_t c = { m->data + ESPNOW_ENV_SIZE, m->len - ESPNOW_ENV_SIZE };
    uint8_t tag, vlen;
    const uint8_t *val;
    bool got_sync = false;
    while (tlv_next(&c, &tag, &val, &vlen)) {
        if (tag == TLV_TIME_SYNC && vlen == 6) {
            apply_time_sync(rd_u32(val), rd_u16(val + 4));
            got_sync = true;
        } else if (tag == TLV_REJECT_REASON && vlen == 1) {
            TLOG("comms: HELLO rejected, reason=%u\n", val[0]);
        }
    }
    if (got_sync && m->ch != expect_ch) {
        /* Late ACK from an earlier dwell: the gateway was heard on
         * expect_ch-adjacent traffic, but this frame came in on a
         * different channel than the one we're dwelling on now. Anchor
         * refresh is fine, BINDING here is not — the gateway isn't on
         * m->ch (that's why the batch afterwards always failed). */
        TLOG("comms: HELLO_ACK heard on ch %u while dwelling on %u — "
               "not binding\n", m->ch, expect_ch);
        return false;
    }
    return got_sync;
}

/* One scan attempt over cached → {1,6,11} → all 13, bounded per DN003. */
static void scan_attempt(void)
{
    static const uint8_t prior[] = SCAN_PRIOR_CH;
    uint8_t chans[13];
    int nch = 0;

    if (s_channel >= 1 && s_channel <= 13) {
        chans[nch++] = s_channel; /* last-known-good first */
    }
    for (int i = 0; i < 3; i++) {
        if (prior[i] != s_channel) {
            chans[nch++] = prior[i];
        }
    }
    for (uint8_t ch = 1; ch <= 13; ch++) {
        bool dup = false;
        for (int i = 0; i < nch; i++) {
            if (chans[i] == ch) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            chans[nch++] = ch;
        }
    }

    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = hello_build(buf);
    uint32_t t0 = now_ms();

    for (int i = 0; i < nch && (now_ms() - t0) < SCAN_BUDGET_MS; i++) {
        set_channel(chans[i]);
        esp_err_t send_err = esp_now_send(BCAST_MAC, buf, len);
        if (send_err != ESP_OK) {
            TLOG("comms: !! HELLO send on ch %u FAILED: %s\n",
                   chans[i], esp_err_to_name(send_err));
        }
        rx_msg_t m;
        while (xQueueReceive(s_rx_q, &m, pdMS_TO_TICKS(SCAN_DWELL_MS)) == pdTRUE) {
            if (parse_hello_ack(&m, chans[i])) {
                s_channel = chans[i];
                memcpy(s_gw_mac, m.mac, 6);
                add_gw_peer(s_gw_mac);
                s_bound = true;
                s_state = CS_ONLINE;
                s_consec_fail = 0;
                s_batch_out = false;
                s_next_heartbeat_ms = now_ms() + HEARTBEAT_S * 1000;
                nvs_save(); /* remember last-known-good channel */
                TLOG("comms: bound gw " MACSTR " ch=%u\n",
                       MAC2STR(s_gw_mac), s_channel);
                return;
            }
        }
    }
    s_next_scan_ms = now_ms() + SCAN_REST_MS; /* rest between attempts */
}

static void go_unreachable(void)
{
    TLOG("comms: gateway unreachable after %u failed exchanges — "
           "buffering, rescanning\n", s_consec_fail);
    s_bound = false;
    s_batch_out = false; /* entries stay in the FIFO and drain on rebind */
    s_state = CS_SCANNING;
    s_next_scan_ms = now_ms() + SCAN_REST_MS;
    rx_msg_t stale;
    while (xQueueReceive(s_rx_q, &stale, 0) == pdTRUE) {
        /* drop frames that pre-date the outage: a stale HELLO_ACK sitting
         * here would re-bind instantly on a dead channel */
    }
}

/* ---- telemetry batch (stop-and-wait) ---- */

static bool anchored(void)
{
    /* DN003 epoch-less invariant: TELEMETRY_BATCH only after a non-zero
     * TIME_SYNC this boot session. HELLO itself is exempt (it obtains sync). */
    return s_anchor_epoch_s != 0;
}

static void send_batch(void)
{
    uint8_t payload[ESPNOW_MAX_PAYLOAD];
    telemetry_batch_hdr_t *h = (telemetry_batch_hdr_t *)payload;
    uint16_t n;

    portENTER_CRITICAL(&s_fifo_mu);
    n = s_ring_cnt < BBU_FRAME_SAMPLES ? s_ring_cnt : BBU_FRAME_SAMPLES;
    if (n == 0) {
        portEXIT_CRITICAL(&s_fifo_mu);
        return;
    }
    h->start_ms = s_ring[s_ring_head].capture_ms;
    h->end_ms = s_ring[(s_ring_head + n - 1) % s_ring_cap].capture_ms;
    h->count = n;
    h->schema_ver = BBU_SCHEMA_VER;
    for (uint16_t i = 0; i < n; i++) {
        memcpy(payload + 11 + i * sizeof(bbu_telemetry_v1_t),
               &s_ring[(s_ring_head + i) % s_ring_cap].s,
               sizeof(bbu_telemetry_v1_t));
    }
    portEXIT_CRITICAL(&s_fifo_mu);

    s_batch_len = env_build(s_batch_buf, MSG_TELEMETRY_BATCH, 0,
                            payload, (uint8_t)(11 + n * sizeof(bbu_telemetry_v1_t)));
    s_batch_end_ms = h->end_ms;
    s_batch_out = true;
    s_batch_sent_ms = now_ms();

    bool ok = send_wait(s_gw_mac, s_batch_buf, s_batch_len);
    if (ok) {
        s_consec_fail = 0; /* radio link alive; ack decides the rest */
    } else {
        s_consec_fail++;
        TLOG("comms: batch send failed (fail %u)\n", s_consec_fail);
    }
}

/* ---- parameters over the wire ---- */

/* DN003: PARAM_ACK {param_id, result, prev_value, new_value} — values in
 * the param's wire type (see descriptor). */
static void param_ack_build_send(uint8_t id, uint8_t result,
                                 int32_t prev_raw, int32_t new_raw)
{
    uint8_t tlv[24];
    size_t n = 0;
    tlv[n++] = TLV_PARAM_ID;     tlv[n++] = 1; tlv[n++] = id;
    tlv[n++] = TLV_PARAM_RESULT; tlv[n++] = 1; tlv[n++] = result;

    tlv[n++] = TLV_PREV_VALUE;
    size_t vlen = comms_encode_param_value(id, prev_raw, tlv + n + 1);
    tlv[n++] = (uint8_t)vlen;
    n += vlen;
    tlv[n++] = TLV_NEW_VALUE;
    vlen = comms_encode_param_value(id, new_raw, tlv + n + 1);
    tlv[n++] = (uint8_t)vlen;
    n += vlen;

    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = env_build(buf, MSG_PARAM_ACK, 0, tlv, (uint8_t)n);
    send_wait(s_gw_mac, buf, len);
}

/* DN003: PARAM_SET {param_id, value, admin_seq, ttl_s}. Replay guard via
 * monotonic admin_seq; EXPIRED otherwise. */
static void handle_param_set(const rx_msg_t *m)
{
    tlv_cur_t c = { m->data + ESPNOW_ENV_SIZE, m->len - ESPNOW_ENV_SIZE };
    uint8_t tag, vlen;
    const uint8_t *val;
    uint8_t id = 0, result = PARAM_RESULT_REJECTED_TYPE, wire[4] = { 0 };
    size_t wire_len = 0;
    uint32_t admin_seq = 0;
    bool have_val = false, have_seq = false;

    while (tlv_next(&c, &tag, &val, &vlen)) {
        switch (tag) {
        case TLV_PARAM_ID:
            if (vlen == 1) {
                id = val[0];
            }
            break;
        case TLV_PARAM_VALUE:
            have_val = true;
            wire_len = vlen < sizeof(wire) ? vlen : sizeof(wire);
            memcpy(wire, val, wire_len);
            break;
        case TLV_ADMIN_SEQ:
            if (vlen == 4) {
                admin_seq = rd_u32(val);
                have_seq = true;
            }
            break;
        case TLV_TTL_S:
            /* parsed for the log; node-side TTL enforcement deferred
             * to DN005 issuance timestamps (see header comment) */
            break;
        default:
            break; /* unknown tags skipped: forward compatibility */
        }
    }

    const bbu_param_desc_t *d = params_desc_by_id(id);
    int32_t value = 0;
    bool type_ok = d != NULL && have_val && have_seq &&
                   wire_len == param_wire_len(d);
    if (type_ok) {
        switch (d->type) {
        case PTYPE_U32:
            value = (int32_t)rd_u32(wire);
            break;
        case PTYPE_I16_X10:
            value = (int16_t)rd_u16(wire);
            break;
        default:
            value = wire[0];
            break;
        }
    }

    int32_t prev_raw = 0;
    (void)params_get_raw_by_id(id, &prev_raw);

    if (!type_ok) {
        result = PARAM_RESULT_REJECTED_TYPE;
    } else if (admin_seq <= s_admin_seq) {
        result = PARAM_RESULT_EXPIRED; /* replayed/stale command */
    } else if (!params_set_by_id(id, value)) {
        result = PARAM_RESULT_REJECTED_RANGE;
    } else {
        s_admin_seq = admin_seq;
        result = PARAM_RESULT_OK;
        TLOG("comms: PARAM_SET %s=%ld (admin_seq %lu, ttl %us)\n",
               d->name, (long)value, (unsigned long)admin_seq,
               PARAM_DEFAULT_TTL_S);
    }

    int32_t new_raw = prev_raw;
    if (result == PARAM_RESULT_OK) {
        (void)params_get_raw_by_id(id, &new_raw);
    }
    param_ack_build_send(id, result, prev_raw, new_raw);
}

static void handle_param_get(const rx_msg_t *m)
{
    tlv_cur_t c = { m->data + ESPNOW_ENV_SIZE, m->len - ESPNOW_ENV_SIZE };
    uint8_t tag, vlen;
    const uint8_t *val;
    uint8_t id = 0;
    while (tlv_next(&c, &tag, &val, &vlen)) {
        if (tag == TLV_PARAM_ID && vlen == 1) {
            id = val[0];
        }
    }
    int32_t raw = 0;
    uint8_t result = params_get_raw_by_id(id, &raw)
                         ? PARAM_RESULT_OK
                         : PARAM_RESULT_REJECTED_TYPE;
    param_ack_build_send(id, result, raw, raw);
}

/* CONFIG_DESC: TLV stream of PARAM_DESCRIPTORs, fragmented when needed.
 * Fragmentation (DN003 open item, implementation choice): payload =
 * {frag_idx u8, frag_total u8, bytes...}; MORE_FRAGMENTS on all but last;
 * receiver reassembles in idx order, 2 s inter-fragment timeout. */
static void handle_config_get(void)
{
    static uint8_t desc[DESC_BUF_SIZE];
    size_t n = 0;
    int count = 0;
    const bbu_param_desc_t *table = params_table(&count);

    for (int i = 0; i < count; i++) {
        const bbu_param_desc_t *d = &table[i];
        size_t name_len = strlen(d->name);
        size_t vlen = 3 + 12 + name_len + 1;
        if (n + 2 + vlen > sizeof(desc)) {
            break; /* table too big for buffer: truncate (count < table) */
        }
        desc[n++] = TLV_PARAM_DESCRIPTOR;
        desc[n++] = (uint8_t)vlen;
        desc[n++] = d->id;
        desc[n++] = d->type;
        desc[n++] = d->flags;
        wr_u32(desc + n, (uint32_t)d->min); n += 4;
        wr_u32(desc + n, (uint32_t)d->max); n += 4;
        wr_u32(desc + n, (uint32_t)d->step); n += 4;
        memcpy(desc + n, d->name, name_len + 1);
        n += name_len + 1;
    }

    size_t frag_room = ESPNOW_MAX_PAYLOAD - ESPNOW_ENV_SIZE - CONFIG_DESC_FRAG_OVH;
    size_t frag_total = (n + frag_room - 1) / frag_room;
    if (frag_total < 1) {
        frag_total = 1;
    }
    TLOG("comms: CONFIG_DESC %u bytes in %u fragment(s)\n",
           (unsigned)n, (unsigned)frag_total);

    for (size_t f = 0; f < frag_total; f++) {
        size_t off = f * frag_room;
        size_t chunk = n - off < frag_room ? n - off : frag_room;
        uint8_t payload[ESPNOW_MAX_PAYLOAD];
        payload[0] = (uint8_t)f;
        payload[1] = (uint8_t)frag_total;
        memcpy(payload + CONFIG_DESC_FRAG_OVH, desc + off, chunk);
        uint8_t flags = (f + 1 < frag_total) ? ESPNOW_FLAG_MORE_FRAGMENTS : 0;
        uint8_t buf[ESPNOW_MAX_PAYLOAD];
        size_t len = env_build(buf, MSG_CONFIG_DESC, flags, payload,
                               (uint8_t)(CONFIG_DESC_FRAG_OVH + chunk));
        if (!send_wait(s_gw_mac, buf, len)) {
            TLOG("comms: CONFIG_DESC frag %u send failed\n", (unsigned)f);
            return;
        }
    }
}

/* ---- online service ---- */

static void handle_rx(const rx_msg_t *m)
{
    const espnow_envelope_t *e = (const espnow_envelope_t *)m->data;
    if (m->len < ESPNOW_ENV_SIZE || e->proto_ver != ESPNOW_PROTO_VER ||
        e->msg_type == MSG_INVALID) {
        return; /* malformed: drop + count via rx counter only */
    }
    /* HELLO from other nodes arrives here too (broadcast); ignore. */
    if (e->node_type != s_node_type || e->node_id != s_node_id) {
        return;
    }
    switch (e->msg_type) {
    case MSG_TIME_SYNC:
        if (m->len == ESPNOW_ENV_SIZE + 6) {
            apply_time_sync(rd_u32(m->data + ESPNOW_ENV_SIZE),
                            rd_u16(m->data + ESPNOW_ENV_SIZE + 4));
        }
        break;
    case MSG_HELLO_ACK:
        /* late duplicate HELLO_ACKs refresh the anchor; no channel check
         * needed here — the node is not re-binding from ONLINE */
        parse_hello_ack(m, m->ch);
        break;
    case MSG_BATCH_ACK:
        if (m->len == ESPNOW_ENV_SIZE + 4 && s_batch_out) {
            uint32_t w = rd_u32(m->data + ESPNOW_ENV_SIZE);
            fifo_trim(w);
            s_batch_out = false;
            s_consec_fail = 0;
            s_ct.acks++;
            TLOG("comms: ACK w=%lu fifo=%u\n",
                   (unsigned long)w, fifo_count());
        }
        break;
    case MSG_PARAM_SET:
        handle_param_set(m);
        break;
    case MSG_PARAM_GET:
        handle_param_get(m);
        break;
    case MSG_CONFIG_GET:
        handle_config_get();
        break;
    default:
        break; /* unknown/other types: ignore (append-only registry) */
    }
}

static void online_step(void)
{
    uint32_t now = now_ms();

    rx_msg_t m;
    while (xQueueReceive(s_rx_q, &m, 0) == pdTRUE) {
        handle_rx(&m);
    }

    if (s_batch_out && (int32_t)(now - s_batch_sent_ms) >= ACK_TIMEOUT_MS) {
        /* retransmit the SAME entries (stop-and-wait, DN003) */
        s_ct.retrans++;
        s_consec_fail++;
        TLOG("comms: ack timeout — retransmit (fail %u)\n", s_consec_fail);
        if (s_consec_fail >= MAX_CONSEC_FAIL) {
            go_unreachable();
            return;
        }
        if (!send_wait(s_gw_mac, s_batch_buf, s_batch_len)) {
            s_consec_fail++;
        }
        s_batch_sent_ms = now;
    }

    if (!s_batch_out && (int32_t)(now - s_next_heartbeat_ms) >= 0) {
        uint8_t buf[ESPNOW_ENV_SIZE];
        bool ok = send_wait(s_gw_mac, buf,
                            env_build(buf, MSG_HEARTBEAT, 0, NULL, 0));
        s_next_heartbeat_ms = now + HEARTBEAT_S * 1000;
        if (ok) {
            s_consec_fail = 0;
        } else {
            s_consec_fail++;
        }
    }

    if (!s_batch_out) {
        ev_msg_t ev;
        if (ev_pop(&ev)) {
            /* EVENT payload is ONE TLV: tag = event id */
            uint8_t tlv[2 + sizeof(ev.val)];
            tlv[0] = ev.id;
            tlv[1] = ev.len;
            memcpy(tlv + 2, ev.val, ev.len);
            uint8_t buf[ESPNOW_MAX_PAYLOAD];
            size_t len = env_build(buf, MSG_EVENT, 0, tlv,
                                   (uint8_t)(2 + ev.len));
            if (send_wait(s_gw_mac, buf, len)) {
                s_ct.ev_sent++;
            } else {
                s_ct.ev_drop++;
                s_consec_fail++;
            }
        }
    }

    if (!s_batch_out && anchored() && fifo_count() > 0) {
        send_batch();
    }

    if (s_consec_fail >= MAX_CONSEC_FAIL) {
        go_unreachable();
    }
}

static void comms_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (radio_start() != ESP_OK) {
            TLOG("comms: radio init failed\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        switch (s_state) {
        case CS_SCANNING:
            if ((int32_t)(now_ms() - s_next_scan_ms) >= 0) {
                scan_attempt();
            }
            break;
        case CS_ONLINE:
            online_step();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- public API ---- */

void comms_init(void)
{
    nvs_load();

    s_boot_session = (uint8_t)(esp_random() & 0xFF);
    if (s_boot_session == 0) {
        s_boot_session = 1;
    }

    /* Ring: try the full default, degrade in halves on OOM rather than
     * losing comms entirely (field reality: heap measured on the bench). */
    s_ring_cap = COMMS_RING_SAMPLES;
    while (s_ring != NULL || s_ring_cap >= 32) {
        s_ring = malloc(sizeof(fifo_ent_t) * s_ring_cap);
        if (s_ring != NULL) {
            break;
        }
        s_ring_cap /= 2;
    }
    if (s_ring == NULL) {
        TLOG("comms: ring alloc failed even at 32 entries — comms disabled\n");
        s_enabled = false;
        s_ring_cap = 0;
    }

    s_rx_q = xQueueCreate(RECV_QUEUE_LEN, sizeof(rx_msg_t));
    configASSERT(s_rx_q);
    s_tx_done = xSemaphoreCreateBinary();
    configASSERT(s_tx_done);

    s_next_scan_ms = 0; /* scan immediately once enabled */

    if (xTaskCreate(comms_task, "comms", COMMS_TASK_STACK, NULL,
                    COMMS_TASK_PRIO, NULL) != pdPASS) {
        TLOG("comms: task create failed\n");
        return;
    }
    TLOG("comms: init node(%u,%u) boot_session=%u enabled=%d ring=%u\n",
           s_node_type, s_node_id, s_boot_session, s_enabled, s_ring_cap);
}

bool comms_enabled(void)
{
    return s_enabled;
}

void comms_enable(bool on)
{
    if (s_enabled == on) {
        return;
    }
    s_enabled = on;
    if (on) {
        /* Force re-discovery: the gateway (re)learns us only via HELLO.
         * A stale ONLINE state from before the disable would leave us
         * unregistered on a restarted gateway (DN003 registration model). */
        s_state = CS_SCANNING;
        s_bound = false;
        s_batch_out = false;
        s_next_scan_ms = 0;
        /* Flush stale queued frames: a HELLO_ACK captured minutes ago (e.g.
         * replies to a hel burst while disabled) would otherwise bind us to
         * a dead conversation on the wrong channel with a stale anchor. */
        rx_msg_t stale;
        while (xQueueReceive(s_rx_q, &stale, 0) == pdTRUE) {
        }
        TLOG("comms: enabled (re-discovery forced, rx queue flushed)\n");
    } else {
        TLOG("comms: disabled (FIFO held, no radio work)\n");
    }
    nvs_save();
}

void comms_offer_sample(const comms_sample_t *s)
{
    if (!s_enabled || s == NULL) {
        return;
    }
    uint32_t now = now_ms();
    /* cadence gate: monitor calls at 1 Hz; capture every period */
    if (s_last_capture_ms != 0 &&
        (int32_t)(now - s_last_capture_ms) < (int32_t)(s_sample_period_s * 1000u)) {
        return;
    }
    s_last_capture_ms = now;

    bbu_telemetry_v1_t t = {
        .mode = s->mode_w,
        .relay_state = s->relay_state,
        .ct_state = s->ct_state,
        .t_tpo_x10 = s->t_tpo_x10,
        .t_tpu_x10 = s->t_tpu_x10,
        .t_amb_x10 = s->t_amb_x10,
        .fault_flags = s->fault_flags,
        .schema_ver = BBU_SCHEMA_VER,
    };
    fifo_push(&t, now);
}

/* Bench: manual HELLO burst on a fixed channel. Run with `comms off` so
 * the comms task doesn't fight over the channel (radio_start is
 * independent of the enable flag). Proves TX radiates + master ESP-NOW RX
 * independent of the scan state machine. */
void comms_bench_hello_burst(uint8_t ch, int count)
{
    if (radio_start() != ESP_OK) {
        TLOG("comms: radio init failed\n");
        return;
    }
    if (s_enabled) {
        TLOG("comms: WARNING comms task is live and may fight over the "
               "channel — run `comms off` first\n");
    }
    set_channel(ch);
    uint8_t buf[ESPNOW_MAX_PAYLOAD];
    size_t len = hello_build(buf);
    TLOG("comms: HELLO burst: %d frames on ch %u (frame %u B)\n",
           count, ch, (unsigned)len);
    for (int i = 0; i < count; i++) {
        esp_err_t err = esp_now_send(BCAST_MAC, buf, len);
        if (err != ESP_OK) {
            TLOG("  send %u FAILED: %s\n", i, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    TLOG("comms: burst done\n");
}

void comms_status_print(void)
{
    TLOG("comms: enabled=%d state=%s node(%u,%u) boot=%u seq=%u\n",
           s_enabled, TAG_STATE[s_state], s_node_type, s_node_id,
           s_boot_session, s_seq);
    TLOG("  channel=%u bound=%d gw=" MACSTR "\n",
           s_channel, s_bound, MAC2STR(s_gw_mac));
    TLOG("  anchored=%d epoch=%lu.%03u@%lums sample_period=%lus\n",
           anchored(), (unsigned long)s_anchor_epoch_s, s_anchor_epoch_ms,
           (unsigned long)s_anchor_clock_ms,
           (unsigned long)s_sample_period_s);
    TLOG("  fifo=%u/%u outstanding=%d fails=%u admin_seq=%lu\n",
           fifo_count(), s_ring_cap, s_batch_out, s_consec_fail,
           (unsigned long)s_admin_seq);
    TLOG("  rx=%lu drop=%lu tx_ok=%lu fail=%lu acks=%lu retrans=%lu\n",
           (unsigned long)s_ct.rx, (unsigned long)s_ct.rx_drop,
           (unsigned long)s_ct.tx_ok, (unsigned long)s_ct.tx_fail,
           (unsigned long)s_ct.acks, (unsigned long)s_ct.retrans);
    TLOG("  decim=%lu ev_sent=%lu ev_drop=%lu\n",
           (unsigned long)s_ct.decim_passes, (unsigned long)s_ct.ev_sent,
           (unsigned long)s_ct.ev_drop);
}

bool comms_set_ident(uint8_t node_type, uint8_t node_id)
{
    if (node_type == 0 || node_id == 0) {
        return false;
    }
    s_node_type = node_type;
    s_node_id = node_id;
    nvs_save();
    TLOG("comms: identity node(%u,%u) saved\n", s_node_type, s_node_id);
    return true;
}

void comms_set_sample_period_s(uint32_t s)
{
    if (s == 0) {
        s = 1;
    }
    s_sample_period_s = s;
    TLOG("comms: sample period %lus\n", (unsigned long)s_sample_period_s);
}

bool comms_ring_resize(uint16_t samples)
{
    if (samples == 0) {
        return false;
    }
    fifo_ent_t *ring = malloc(sizeof(fifo_ent_t) * samples);
    if (ring == NULL) {
        TLOG("comms: ring resize to %u failed (kept %u)\n",
               samples, s_ring_cap);
        return false;
    }
    portENTER_CRITICAL(&s_fifo_mu);
    free(s_ring);
    s_ring = ring;
    s_ring_cap = samples;
    s_ring_head = 0;
    s_ring_cnt = 0;
    portEXIT_CRITICAL(&s_fifo_mu);
    TLOG("comms: ring resized to %u samples (EMPTY — bench knob)\n",
           samples);
    return true;
}

uint8_t comms_node_type(void)
{
    return s_node_type;
}

uint8_t comms_node_id(void)
{
    return s_node_id;
}
