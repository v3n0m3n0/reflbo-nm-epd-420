#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lora_widget.h"
#include "meshcore_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESHCORE_SEEN_PKT_CACHE_SIZE 64
#define MESHCORE_SEEN_PKT_TTL_MS     120000U /* 2 minutes */
#define MESHCORE_MAX_PENDING_ACKS    8

typedef struct {
    uint8_t hash[8];
    uint32_t seen_ms;
    bool valid;
} meshcore_seen_pkt_t;

typedef struct {
    bool     active;
    uint8_t  ack_tag[4];
    uint32_t sent_ms;
} meshcore_pending_ack_t;

typedef struct {
    meshcore_seen_pkt_t seen_pkts[MESHCORE_SEEN_PKT_CACHE_SIZE];
    size_t seen_pkt_next;
    meshcore_advert_t adverts[MESHCORE_MAX_ADVERTS];
    meshcore_neighbor_t neighbors[MESHCORE_MAX_NEIGHBORS];
    meshcore_msg_t messages[MESHCORE_MAX_MESSAGES];
    uint32_t next_tx_allowed_ms;
    bool has_tx_budget;
    meshcore_pending_ack_t pending_acks[MESHCORE_MAX_PENDING_ACKS];
    size_t pending_ack_next;
    uint32_t next_msg_id;
} meshcore_node_state_t;

typedef struct {
    void (*reset_stats)(void *ctx);
    bool (*send_advert)(void *ctx, bool flood);
    bool (*send_discover)(void *ctx);
    bool (*send_channel_msg)(void *ctx, uint8_t channel_idx, const char *text);
    void (*update_ble_name)(void *ctx, const char *node_name);
    void (*update_ble_pin)(void *ctx, uint32_t pin);
    bool (*reset_identity)(void *ctx);
    void (*list_contacts)(void *ctx, char *reply, size_t reply_sz);
    void (*list_channels)(void *ctx, char *reply, size_t reply_sz);
    const char (*channel_names)[LORA_CHANNEL_NAME_LEN];
    size_t channel_names_count;
    void *user_data;
} meshcore_cli_env_t;

void meshcore_node_init(meshcore_node_state_t *st);
void meshcore_node_reset(meshcore_node_state_t *st, lora_stats_t *stats);

void meshcore_node_mark_seen(meshcore_node_state_t *st, const meshcore_packet_t *pkt, uint32_t now_ms);
bool meshcore_node_is_seen_and_mark(meshcore_node_state_t *st, const meshcore_packet_t *pkt, uint32_t now_ms);

void meshcore_node_update_advert(meshcore_node_state_t *st, lora_stats_t *stats, const meshcore_advert_t *adv);
bool meshcore_node_remove_advert_by_pubkey(meshcore_node_state_t *st, lora_stats_t *stats, const uint8_t pub_key[32]);
void meshcore_node_update_neighbor(meshcore_node_state_t *st, lora_stats_t *stats, const meshcore_neighbor_t *nb);

bool meshcore_node_is_dup_message(const meshcore_node_state_t *st, const lora_stats_t *stats,
                                 uint32_t ts, uint32_t rx_time_s, const char *text);
void meshcore_node_append_message(meshcore_node_state_t *st, lora_stats_t *stats, const meshcore_msg_t *msg);
bool meshcore_node_confirm_msg_ack(meshcore_node_state_t *st, lora_stats_t *stats, const uint8_t ack_tag[4], uint16_t rtt_ms);
bool meshcore_node_timeout_msg_ack(meshcore_node_state_t *st, lora_stats_t *stats, const uint8_t ack_tag[4]);

size_t meshcore_node_build_discover_resp(const meshcore_packet_t *req_pkt, int8_t snr_q4,
                                         const uint8_t self_pub_key[32],
                                         uint8_t *out_raw, size_t max_raw);

size_t meshcore_node_build_ack(const uint8_t *plain_after_hdr, size_t plain_len,
                               const uint8_t peer_pub_key[32],
                               int8_t peer_out_path_len, const uint8_t *peer_out_path,
                               uint8_t *out_raw, size_t max_raw);

uint32_t meshcore_node_tx_wait_ms(const meshcore_node_state_t *st, uint32_t now_ms);
void meshcore_node_record_tx_airtime(meshcore_node_state_t *st, uint32_t now_ms,
                                     uint32_t air_ms, uint8_t airtime_factor_x10);

size_t meshcore_node_build_direct_txt(const uint8_t self_pub_key[32],
                                      const uint8_t self_prv_key[64],
                                      const uint8_t peer_pub_key[32],
                                      int8_t peer_out_path_len,
                                      const uint8_t *peer_out_path,
                                      uint8_t txt_type,
                                      uint8_t attempt,
                                      uint32_t msg_ts,
                                      const char *text,
                                      uint8_t out_ack_tag[4],
                                      uint8_t *out_raw,
                                      size_t max_raw);

void meshcore_node_expect_ack(meshcore_node_state_t *st, const uint8_t ack_tag[4], uint32_t sent_ms);
void meshcore_node_cancel_ack(meshcore_node_state_t *st, const uint8_t ack_tag[4]);
bool meshcore_node_check_ack(meshcore_node_state_t *st, const meshcore_packet_t *pkt,
                             uint32_t now_ms, uint8_t out_ack_tag[4], uint32_t *out_rtt_ms);
size_t meshcore_node_expire_acks(meshcore_node_state_t *st, lora_stats_t *stats,
                                 uint32_t now_ms, uint32_t timeout_ms);

bool meshcore_node_prepare_client_repeat(const meshcore_packet_t *rx_pkt,
                                         const uint8_t self_pub_key[32],
                                         uint8_t path_hash_mode,
                                         uint8_t *out_raw,
                                         size_t max_raw,
                                         size_t *out_len);

void meshcore_node_exec_cli(meshcore_node_state_t *st, lora_config_t *current_cfg, lora_stats_t *stats,
                            const meshcore_cli_env_t *env, const char *cmd,
                            lora_config_t *cfg_inout, bool *cfg_changed,
                            char *reply, size_t reply_sz);

#ifdef __cplusplus
}
#endif
