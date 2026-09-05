/* gw.h — shared state between the gateway's three translation units.
 *
 *   main.c   — DN003 wire chassis (ESP-NOW, registry, LED, console)
 *   net.c    — WiFi STA + SNTP + MQTT + DN004 state machine + NVS + time
 *   softap.c — GPIO27 button maintenance AP (provisioning + status pages)
 *
 * Pass 1 logger-gateway (issue 015, design settled 2026-09-04):
 * single-node scope, RAM registry, no command pipeline, no role table.
 * BATCH_ACK is gated on the commit service's watermark (never on
 * "broker received it").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espnow_schema.h"

/* ---- gateway mode (DN004 state machine; owned by net.c) ---- */

typedef enum {
    GW_WAIT_BACKEND = 0,
    GW_ACTIVE = 1,
} gw_mode_t;

/* ---- node registry (pass 1: RAM only, MAC learned on first HELLO) ---- */

typedef struct {
    bool have;
    uint64_t epoch_total_ms;   /* gateway epoch (ms since Unix) at anchor */
    uint32_t node_clock_ms;    /* node_clock_ms in the HELLO that anchored */
    uint8_t boot_session;
} gw_anchor_t;

typedef struct {
    bool in_use;
    uint8_t mac[6];
    uint8_t type, id;
    uint16_t liveness_s;
    uint32_t last_seen_ms;     /* gateway uptime ms of last frame */
    uint8_t last_boot;
    uint16_t last_seq;
    uint8_t config_ver;
    bool config_fetched;
    uint16_t last_batch_seq;   /* dup-batch dedupe (retransmit re-ACK) */
    uint8_t last_batch_boot;
    bool flagged_unreach;
    gw_anchor_t anchor;

    /* commit-gated ack (issue 015): BATCH_ACK only after the commit
     * service's watermark covers the batch's end_ms in the node clock
     * domain. boot_session change resets this (node clock restarted). */
    bool have_commit;
    uint32_t commit_ms;        /* last watermark seen (node clock ms) */
    bool have_pending;
    uint32_t pending_ms;       /* end_ms of batch awaiting watermark */
    uint32_t pending_since_ms; /* gateway uptime ms */

    /* CONFIG_DESC reassembly */
    uint8_t frag_total, frag_next;
    uint32_t frag_last_ms;
    uint8_t desc_buf[600];
    size_t desc_len;
} gw_node_t;

#define GW_MAX_NODES 4

/* ---- exported state ---- */

extern volatile gw_mode_t gw_mode;
extern volatile uint32_t gw_frame_cnt;  /* frames processed (LED flicker) */
extern gw_node_t gw_nodes[GW_MAX_NODES];
extern int gw_node_cnt;

typedef struct {
    uint32_t rx[13];
    uint32_t tx_ok, tx_fail;
    uint32_t acks_sent, acks_held;   /* held = decoded but not acked */
    uint32_t samples_published;
} gw_ct_t;
extern gw_ct_t gw_ct;

/* ---- time (net.c) ---- */

bool gw_time_valid(void);
uint64_t gw_epoch_ms(void);                      /* 0 when invalid */
void gw_time_set_unix(uint64_t unix_s, uint16_t ms);
void gw_time_mark_serial(void);                  /* tag time source "serial" */
const char *gw_time_source(void);                /* sntp/mqtt/serial/nvs/none */

/* ---- NVS config (net.c; namespace "gw") ---- */

void gw_nvs_get_str(const char *key, char *out, size_t outlen, const char *def);
void gw_nvs_set_str(const char *key, const char *val);
uint32_t gw_nvs_get_u32(const char *key, uint32_t def);
void gw_nvs_set_u32(const char *key, uint32_t v);

/* ---- uplink health (net.c) ---- */

bool gw_wifi_up(void);
bool gw_broker_up(void);
uint32_t gw_health_age_ms(void);                 /* UINT32_MAX = never seen */
const char *gw_net_ssid(void);                   /* provisioned SSID or "" */
void gw_net_broker_str(char *out, size_t n);     /* "host:port" */
const char *gw_net_mqtt_user(void);              /* provisioned MQTT user or NULL */
void gw_net_mqtt_restart(void);                  /* after broker NVS change */
void gw_net_init(void);                          /* WiFi+MQTT+time bring-up */

/* ---- MQTT publish (net.c) — thread-safe, NULL-safe before broker ---- */

bool gw_mqtt_publish(const char *topic, const char *json, int qos, bool retain);

/* ---- radio (main.c) ---- */

uint32_t gw_now_ms(void);                        /* gateway uptime ms */
void gw_espnow_enable(void);                     /* ACTIVE enter */
void gw_espnow_disable(void);                    /* ACTIVE exit */
bool gw_send_wait(const uint8_t *mac, const uint8_t *buf, size_t len);
size_t gw_env_build(uint8_t *buf, uint8_t node_type, uint8_t node_id,
                    uint8_t msg_type, const void *payload, uint8_t plen);
void gw_time_sync_send(gw_node_t *n);            /* standalone TIME_SYNC */
gw_node_t *gw_node_by_mac(const uint8_t *mac);
gw_node_t *gw_node_by_role(uint8_t type, uint8_t id);
gw_node_t *gw_node_find_or_add(const uint8_t *mac, uint8_t type, uint8_t id);

/* ack bookkeeping shared by main.c (batch) and net.c (watermark): ack n
 * with watermark w if it has a pending batch the watermark covers. */
void gw_ack_if_covered(gw_node_t *n, uint32_t w);

/* ---- maintenance SoftAP (softap.c) ---- */

bool gw_softap_open(void);
void gw_softap_toggle(void);
void gw_softap_start_button(void);               /* button poll task */
