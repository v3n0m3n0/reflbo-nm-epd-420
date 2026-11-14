#include "meshcore_node.h"

#include "ed_25519.h"
#include "util_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool valid_bw_hz(uint32_t bw_hz)
{
    static const uint32_t k_valid_bws[] = {
        7800U, 10400U, 15600U, 20800U, 31250U, 41700U, 62500U, 125000U, 250000U, 500000U
    };
    for (size_t i = 0; i < sizeof(k_valid_bws) / sizeof(k_valid_bws[0]); i++) {
        if (bw_hz == k_valid_bws[i]) {
            return true;
        }
    }
    return false;
}

static bool parse_on_off(const char *s, bool default_val)
{
    if (!s) return default_val;
    while (*s == ' ' || *s == '\t') s++;
    if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 ||
        strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0) {
        return true;
    }
    if (strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0 ||
        strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0) {
        return false;
    }
    return default_val;
}

void meshcore_node_init(meshcore_node_state_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
}

void meshcore_node_reset(meshcore_node_state_t *st, lora_stats_t *stats)
{
    if (stats) {
        stats->packet_count = 0;
        stats->packets_sent = 0;
        stats->recv_flood = 0;
        stats->recv_direct = 0;
        stats->sent_flood = 0;
        stats->sent_direct = 0;
        stats->recv_errors = 0;
        stats->tx_air_ms = 0;
        stats->rx_air_ms = 0;
        stats->last_rssi_dbm = 0;
        stats->last_snr_db = 0;
        stats->last_snr_q4 = 0;
        stats->last_packet_time_s = 0;
        stats->advert_count = 0;
        stats->neighbor_count = 0;
        stats->msg_count = 0;
        memset(stats->last_sender, 0, sizeof(stats->last_sender));
    }
    if (st) {
        memset(st, 0, sizeof(*st));
    }
}

void meshcore_node_mark_seen(meshcore_node_state_t *st, const meshcore_packet_t *pkt, uint32_t now_ms)
{
    if (!st || !pkt) return;
    uint8_t h[8];
    meshcore_packet_hash(pkt, h);
    for (size_t i = 0; i < MESHCORE_SEEN_PKT_CACHE_SIZE; i++) {
        if (st->seen_pkts[i].valid && memcmp(st->seen_pkts[i].hash, h, 8) == 0) {
            st->seen_pkts[i].seen_ms = now_ms;
            return;
        }
    }
    st->seen_pkts[st->seen_pkt_next].valid = true;
    st->seen_pkts[st->seen_pkt_next].seen_ms = now_ms;
    memcpy(st->seen_pkts[st->seen_pkt_next].hash, h, 8);
    st->seen_pkt_next = (st->seen_pkt_next + 1) % MESHCORE_SEEN_PKT_CACHE_SIZE;
}

bool meshcore_node_is_seen_and_mark(meshcore_node_state_t *st, const meshcore_packet_t *pkt, uint32_t now_ms)
{
    if (!st || !pkt) return false;
    uint8_t h[8];
    meshcore_packet_hash(pkt, h);
    for (size_t i = 0; i < MESHCORE_SEEN_PKT_CACHE_SIZE; i++) {
        if (st->seen_pkts[i].valid && memcmp(st->seen_pkts[i].hash, h, 8) == 0) {
            if ((uint32_t)(now_ms - st->seen_pkts[i].seen_ms) < MESHCORE_SEEN_PKT_TTL_MS) {
                return true;
            }
            st->seen_pkts[i].seen_ms = now_ms;
            return false;
        }
    }
    st->seen_pkts[st->seen_pkt_next].valid = true;
    st->seen_pkts[st->seen_pkt_next].seen_ms = now_ms;
    memcpy(st->seen_pkts[st->seen_pkt_next].hash, h, 8);
    st->seen_pkt_next = (st->seen_pkt_next + 1) % MESHCORE_SEEN_PKT_CACHE_SIZE;
    return false;
}

void meshcore_node_update_advert(meshcore_node_state_t *st, lora_stats_t *stats, const meshcore_advert_t *adv)
{
    if (!st || !stats || !adv) return;

    for (uint8_t i = 0; i < stats->advert_count && i < MESHCORE_MAX_ADVERTS; i++) {
        if (memcmp(st->adverts[i].pub_key, adv->pub_key, 32) == 0) {
            st->adverts[i] = *adv;
            return;
        }
    }

    if (stats->advert_count < MESHCORE_MAX_ADVERTS) {
        st->adverts[stats->advert_count++] = *adv;
        return;
    }

    uint8_t oldest_idx = 0;
    uint32_t oldest_time = st->adverts[0].rx_time_s;
    for (uint8_t i = 1; i < MESHCORE_MAX_ADVERTS; i++) {
        if (st->adverts[i].rx_time_s < oldest_time) {
            oldest_time = st->adverts[i].rx_time_s;
            oldest_idx = i;
        }
    }
    st->adverts[oldest_idx] = *adv;
}

bool meshcore_node_remove_advert_by_pubkey(meshcore_node_state_t *st, lora_stats_t *stats, const uint8_t pub_key[32])
{
    if (!st || !stats || !pub_key) return false;
    for (uint8_t i = 0; i < stats->advert_count && i < MESHCORE_MAX_ADVERTS; i++) {
        if (memcmp(st->adverts[i].pub_key, pub_key, 32) == 0) {
            for (uint8_t j = i; j + 1 < stats->advert_count && j + 1 < MESHCORE_MAX_ADVERTS; j++) {
                st->adverts[j] = st->adverts[j + 1];
            }
            stats->advert_count--;
            memset(&st->adverts[stats->advert_count], 0, sizeof(meshcore_advert_t));
            return true;
        }
    }
    return false;
}

void meshcore_node_update_neighbor(meshcore_node_state_t *st, lora_stats_t *stats, const meshcore_neighbor_t *nb)
{
    if (!st || !stats || !nb) return;

    for (uint8_t i = 0; i < stats->neighbor_count && i < MESHCORE_MAX_NEIGHBORS; i++) {
        if (memcmp(st->neighbors[i].pub_key_prefix, nb->pub_key_prefix, 8) == 0) {
            st->neighbors[i] = *nb;
            return;
        }
    }

    if (stats->neighbor_count < MESHCORE_MAX_NEIGHBORS) {
        st->neighbors[stats->neighbor_count++] = *nb;
        return;
    }

    uint8_t oldest_idx = 0;
    uint32_t oldest_time = st->neighbors[0].rx_time_s;
    for (uint8_t i = 1; i < MESHCORE_MAX_NEIGHBORS; i++) {
        if (st->neighbors[i].rx_time_s < oldest_time) {
            oldest_time = st->neighbors[i].rx_time_s;
            oldest_idx = i;
        }
    }
    st->neighbors[oldest_idx] = *nb;
}

bool meshcore_node_is_dup_message(const meshcore_node_state_t *st, const lora_stats_t *stats,
                                 uint32_t ts, uint32_t rx_time_s, const char *text)
{
    if (!st || !stats || !text) return false;
    for (uint8_t i = 0; i < stats->msg_count && i < MESHCORE_MAX_MESSAGES; i++) {
        if (ts != 0 && st->messages[i].timestamp != 0) {
            if (st->messages[i].timestamp == ts && strcmp(st->messages[i].text, text) == 0) {
                return true;
            }
        } else {
            if (strcmp(st->messages[i].text, text) == 0 &&
                (rx_time_s >= st->messages[i].rx_time_s) &&
                (rx_time_s - st->messages[i].rx_time_s <= 15U)) {
                return true;
            }
        }
    }
    return false;
}

void meshcore_node_append_message(meshcore_node_state_t *st, lora_stats_t *stats, const meshcore_msg_t *msg)
{
    if (!st || !stats || !msg) return;
    stats->msg_seq++;
    if (stats->msg_count < MESHCORE_MAX_MESSAGES) {
        st->messages[stats->msg_count++] = *msg;
    } else {
        for (uint8_t i = 0; i < MESHCORE_MAX_MESSAGES - 1; i++) {
            st->messages[i] = st->messages[i + 1];
        }
        st->messages[MESHCORE_MAX_MESSAGES - 1] = *msg;
    }
}

size_t meshcore_node_build_discover_resp(const meshcore_packet_t *req_pkt, int8_t snr_q4,
                                         const uint8_t self_pub_key[32],
                                         uint8_t *out_raw, size_t max_raw)
{
    if (!req_pkt || !self_pub_key || !out_raw || max_raw == 0) {
        return 0;
    }
    if (req_pkt->payload_len < 6 || (req_pkt->payload[0] & 0xF0) != 0x80) {
        return 0;
    }

    uint8_t resp_payload[14];
    resp_payload[0] = 0x90 | MESHCORE_ADV_CHAT;
    resp_payload[1] = (uint8_t)snr_q4;
    memcpy(&resp_payload[2], &req_pkt->payload[2], 4); /* Match request tag */
    memcpy(&resp_payload[6], self_pub_key, 8);        /* Our pubkey prefix */

    meshcore_packet_t resp_pkt;
    memset(&resp_pkt, 0, sizeof(resp_pkt));
    resp_pkt.header = (uint8_t)((MESHCORE_ROUTE_DIRECT & 0x03) | ((MESHCORE_PAYLOAD_CONTROL & 0x0F) << 2));
    resp_pkt.path_len = 0;
    resp_pkt.payload_len = sizeof(resp_payload);
    memcpy(resp_pkt.payload, resp_payload, sizeof(resp_payload));
    return meshcore_packet_encode(&resp_pkt, out_raw, max_raw);
}

size_t meshcore_node_build_ack(const uint8_t *plain_after_hdr, size_t plain_len,
                               const uint8_t peer_pub_key[32],
                               int8_t peer_out_path_len, const uint8_t *peer_out_path,
                               uint8_t *out_raw, size_t max_raw)
{
    if (!plain_after_hdr || plain_len == 0 || !peer_pub_key || !out_raw || max_raw == 0) {
        return 0;
    }

    util_sha256_t sha;
    util_sha256_init(&sha);
    util_sha256_update(&sha, plain_after_hdr, plain_len);
    util_sha256_update(&sha, peer_pub_key, 32);
    uint8_t full_hash[32];
    util_sha256_final(&sha, full_hash);

    meshcore_packet_t ack_pkt;
    memset(&ack_pkt, 0, sizeof(ack_pkt));
    bool use_direct = false;
    if (peer_out_path && peer_out_path_len > 0) {
        uint8_t pl = (uint8_t)peer_out_path_len;
        uint8_t hash_size = (uint8_t)((pl >> 6) + 1U);
        uint8_t hop_cnt = (uint8_t)(pl & 0x3FU);
        size_t path_bytes = (size_t)hash_size * (size_t)hop_cnt;
        if (hash_size <= 3U && path_bytes > 0U && path_bytes <= sizeof(ack_pkt.path)) {
            ack_pkt.header = (uint8_t)((MESHCORE_ROUTE_DIRECT & 0x03) | ((MESHCORE_PAYLOAD_ACK & 0x0F) << 2));
            ack_pkt.path_len = pl;
            memcpy(ack_pkt.path, peer_out_path, path_bytes);
            use_direct = true;
        }
    }
    if (!use_direct) {
        ack_pkt.header = (uint8_t)((MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_ACK & 0x0F) << 2));
        ack_pkt.path_len = 0;
    }
    ack_pkt.payload_len = 4;
    memcpy(ack_pkt.payload, full_hash, 4);
    return meshcore_packet_encode(&ack_pkt, out_raw, max_raw);
}

uint32_t meshcore_node_tx_wait_ms(const meshcore_node_state_t *st, uint32_t now_ms)
{
    if (!st || !st->has_tx_budget) {
        return 0;
    }
    int32_t diff = (int32_t)(st->next_tx_allowed_ms - now_ms);
    return (diff > 0) ? (uint32_t)diff : 0U;
}

void meshcore_node_record_tx_airtime(meshcore_node_state_t *st, uint32_t now_ms,
                                     uint32_t air_ms, uint8_t airtime_factor_x10)
{
    if (!st) return;
    uint32_t silence_ms = (uint32_t)(((uint64_t)air_ms * (uint64_t)airtime_factor_x10) / 10U);
    st->next_tx_allowed_ms = now_ms + silence_ms;
    st->has_tx_budget = true;
}

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
                                      size_t max_raw)
{
    if (!self_pub_key || !self_prv_key || !peer_pub_key || !text || text[0] == '\0' || !out_raw || max_raw == 0U) {
        return 0;
    }

    uint8_t plain[165] = {0};
    plain[0] = (uint8_t)(msg_ts & 0xFFU);
    plain[1] = (uint8_t)((msg_ts >> 8) & 0xFFU);
    plain[2] = (uint8_t)((msg_ts >> 16) & 0xFFU);
    plain[3] = (uint8_t)((msg_ts >> 24) & 0xFFU);
    plain[4] = (uint8_t)(((txt_type & 0x3FU) << 2) | (attempt & 0x03U));

    size_t tlen = strlen(text);
    if (tlen > sizeof(plain) - 5U) {
        tlen = sizeof(plain) - 5U;
    }
    memcpy(&plain[5], text, tlen);
    size_t plain_len = 5U + tlen;

    util_sha256_t sha;
    util_sha256_init(&sha);
    util_sha256_update(&sha, plain, plain_len);
    util_sha256_update(&sha, self_pub_key, 32);
    uint8_t full_hash[32];
    util_sha256_final(&sha, full_hash);
    if (out_ack_tag) {
        memcpy(out_ack_tag, full_hash, 4);
    }

    uint8_t shared_secret[32];
    ed25519_key_exchange(shared_secret, peer_pub_key, self_prv_key);

    meshcore_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload[0] = peer_pub_key[0];
    pkt.payload[1] = self_pub_key[0];
    int enc_len = meshcore_encrypt_then_mac(shared_secret, &pkt.payload[2],
                                            sizeof(pkt.payload) - 2U, plain, plain_len);
    if (enc_len <= 0) {
        return 0;
    }
    pkt.payload_len = 2U + (size_t)enc_len;

    bool use_direct = false;
    if (peer_out_path && peer_out_path_len > 0) {
        uint8_t pl = (uint8_t)peer_out_path_len;
        uint8_t hash_size = (uint8_t)((pl >> 6) + 1U);
        uint8_t hop_cnt = (uint8_t)(pl & 0x3FU);
        size_t path_bytes = (size_t)hash_size * (size_t)hop_cnt;
        if (hash_size <= 3U && path_bytes > 0U && path_bytes <= sizeof(pkt.path)) {
            pkt.header = (uint8_t)((MESHCORE_ROUTE_DIRECT & 0x03) | ((MESHCORE_PAYLOAD_TXT_MSG & 0x0F) << 2));
            pkt.path_len = pl;
            memcpy(pkt.path, peer_out_path, path_bytes);
            use_direct = true;
        }
    }
    if (!use_direct) {
        pkt.header = (uint8_t)((MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_TXT_MSG & 0x0F) << 2));
        pkt.path_len = 0;
    }

    return meshcore_packet_encode(&pkt, out_raw, max_raw);
}

void meshcore_node_expect_ack(meshcore_node_state_t *st, const uint8_t ack_tag[4], uint32_t sent_ms)
{
    if (!st || !ack_tag) return;
    st->pending_ack.active = true;
    memcpy(st->pending_ack.ack_tag, ack_tag, 4);
    st->pending_ack.sent_ms = sent_ms;
}

bool meshcore_node_check_ack(meshcore_node_state_t *st, const meshcore_packet_t *pkt,
                             uint32_t now_ms, uint8_t out_ack_tag[4], uint32_t *out_rtt_ms)
{
    if (!st || !pkt || !st->pending_ack.active) {
        return false;
    }
    if (meshcore_packet_get_payload_type(pkt) != MESHCORE_PAYLOAD_ACK || pkt->payload_len < 4U) {
        return false;
    }
    if (memcmp(pkt->payload, st->pending_ack.ack_tag, 4) != 0) {
        return false;
    }
    uint32_t rtt = now_ms - st->pending_ack.sent_ms;
    if (rtt == 0U) {
        rtt = 1U;
    }
    if (out_ack_tag) {
        memcpy(out_ack_tag, st->pending_ack.ack_tag, 4);
    }
    if (out_rtt_ms) {
        *out_rtt_ms = rtt;
    }
    st->pending_ack.active = false;
    return true;
}

bool meshcore_node_prepare_client_repeat(const meshcore_packet_t *rx_pkt,
                                         const uint8_t self_pub_key[32],
                                         uint8_t path_hash_mode,
                                         uint8_t *out_raw,
                                         size_t max_raw,
                                         size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!rx_pkt || !self_pub_key || !out_raw || max_raw == 0U || !out_len) {
        return false;
    }
    uint8_t route = meshcore_packet_get_route_type(rx_pkt);
    if (route != MESHCORE_ROUTE_FLOOD && route != MESHCORE_ROUTE_TRANSPORT_FLOOD) {
        return false;
    }
    uint8_t hops = meshcore_packet_get_hop_count(rx_pkt);
    if (hops >= 7U) {
        return false;
    }
    uint8_t mode = (rx_pkt->path_len >> 6) & 0x03U;
    if (mode == 3U) {
        return false;
    }
    if (hops == 0U && mode == 0U) {
        mode = (path_hash_mode <= 2U) ? path_hash_mode : 0U;
    }
    uint8_t hash_size = (uint8_t)(mode + 1U);
    size_t cur_bytes = (size_t)hops * (size_t)hash_size;
    if (cur_bytes + (size_t)hash_size > MESHCORE_MAX_PATH_SIZE) {
        return false;
    }
    for (uint8_t i = 0; i < hops; i++) {
        if (memcmp(&rx_pkt->path[(size_t)i * (size_t)hash_size], self_pub_key, hash_size) == 0) {
            return false;
        }
    }

    meshcore_packet_t fwd = *rx_pkt;
    memcpy(&fwd.path[cur_bytes], self_pub_key, hash_size);
    fwd.path_len = (uint8_t)((mode << 6) | ((hops + 1U) & 0x3FU));

    size_t enc = meshcore_packet_encode(&fwd, out_raw, max_raw);
    if (enc == 0U) {
        return false;
    }
    *out_len = enc;
    return true;
}

static uint32_t cfg_effective_bw_hz(const lora_config_t *cfg)
{
    return cfg->bw_hz ? cfg->bw_hz : (uint32_t)(cfg->bw_khz ? cfg->bw_khz : 250) * 1000U;
}

void meshcore_node_exec_cli(meshcore_node_state_t *st, lora_config_t *current_cfg, lora_stats_t *stats,
                            const meshcore_cli_env_t *env, const char *cmd,
                            lora_config_t *cfg_inout, bool *cfg_changed,
                            char *reply, size_t reply_sz)
{
    if (!reply || reply_sz == 0) {
        return;
    }
    reply[0] = '\0';
    if (cfg_changed) *cfg_changed = false;

    if (!cmd || cmd[0] == '\0') {
        snprintf(reply, reply_sz, "Empty command. Try 'help'.\n");
        return;
    }

    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '\0') {
        snprintf(reply, reply_sz, "Empty command. Try 'help'.\n");
        return;
    }

    if (!current_cfg || !stats) {
        snprintf(reply, reply_sz, "Internal state unavailable.\n");
        return;
    }

    /* Exact single/multi-word commands */
    if (strcasecmp(cmd, "ver") == 0) {
        snprintf(reply, reply_sz, "reflbo-nm-epd-420 v1.0.0 (MeshCore v1.18.0)\n");
        return;
    }
    if (strcasecmp(cmd, "board") == 0) {
        snprintf(reply, reply_sz, "RockBase NM-EPD-420 (ESP32-S3R8 / SX1262)\n");
        return;
    }
    if (strcasecmp(cmd, "clear stats") == 0) {
        if (env && env->reset_stats) {
            env->reset_stats(env->user_data);
        } else {
            meshcore_node_reset(st, stats);
        }
        snprintf(reply, reply_sz, "Stats cleared.\n");
        return;
    }
    if (strcasecmp(cmd, "reset-identity") == 0) {
        bool ok = (env && env->reset_identity) ? env->reset_identity(env->user_data) : false;
        snprintf(reply, reply_sz, "Identity reset: %s (PubKey: %s, PIN: %06lu)\n",
                 ok ? "OK" : "FAIL", stats->pub_key_hex, (unsigned long)stats->ble_pin);
        return;
    }
    if (strcasecmp(cmd, "advert.zerohop") == 0) {
        bool ok = (env && env->send_advert) ? env->send_advert(env->user_data, false) : false;
        snprintf(reply, reply_sz, "Advert zerohop broadcast: %s\n", ok ? "OK" : "FAIL");
        return;
    }
    if (strcasecmp(cmd, "discover.neighbors") == 0) {
        bool ok = (env && env->send_discover) ? env->send_discover(env->user_data) : false;
        snprintf(reply, reply_sz, "Discover request: %s\n", ok ? "OK" : "FAIL");
        return;
    }
    if (strcasecmp(cmd, "stats-core") == 0) {
        snprintf(reply, reply_sz,
                 "Core Stats:\n"
                 "  Board: RockBase NM-EPD-420 (ESP32-S3R8 / SX1262)\n"
                 "  Node: %s\n"
                 "  PubKey: %s\n"
                 "  Enabled: %s\n"
                 "  Initialized: %s\n",
                 current_cfg->node_name, stats->pub_key_hex,
                 stats->enabled ? "true" : "false", stats->initialized ? "true" : "false");
        return;
    }
    if (strcasecmp(cmd, "stats-radio") == 0) {
        snprintf(reply, reply_sz,
                 "Radio Stats:\n"
                 "  Noise floor: %d dBm\n"
                 "  Last RSSI: %d dBm\n"
                 "  Last SNR: %.2f dB\n"
                 "  Airtime TX: %lu ms\n"
                 "  Airtime RX: %lu ms\n"
                 "  Receive errors: %lu\n",
                 stats->noise_floor_dbm, stats->last_rssi_dbm, (double)stats->last_snr_q4 / 4.0,
                 (unsigned long)stats->tx_air_ms, (unsigned long)stats->rx_air_ms,
                 (unsigned long)stats->recv_errors);
        return;
    }
    if (strcasecmp(cmd, "stats-packets") == 0) {
        snprintf(reply, reply_sz,
                 "Packet Stats:\n"
                 "  Packets RX: %lu (Flood: %lu, Direct: %lu)\n"
                 "  Packets TX: %lu (Flood: %lu, Direct: %lu)\n"
                 "  Errors: %lu\n",
                 (unsigned long)stats->packet_count, (unsigned long)stats->recv_flood, (unsigned long)stats->recv_direct,
                 (unsigned long)stats->packets_sent, (unsigned long)stats->sent_flood, (unsigned long)stats->sent_direct,
                 (unsigned long)stats->recv_errors);
        return;
    }

    /* Handle 'get <param>' */
    if (strncasecmp(cmd, "get ", 4) == 0) {
        const char *p = cmd + 4;
        while (*p == ' ' || *p == '\t') p++;
        uint32_t bw_hz = cfg_effective_bw_hz(current_cfg);
        if (strcasecmp(p, "radio") == 0) {
            double f_mhz = (double)current_cfg->freq_khz / 1000.0;
            double bw_khz = (double)bw_hz / 1000.0;
            snprintf(reply, reply_sz, "%.3f,%.1f,%u,%u\n", f_mhz, bw_khz, current_cfg->sf, current_cfg->cr);
        } else if (strcasecmp(p, "tx") == 0) {
            snprintf(reply, reply_sz, "%d\n", current_cfg->tx_power_dbm);
        } else if (strcasecmp(p, "name") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->node_name);
        } else if (strcasecmp(p, "radio.rxgain") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->rx_boosted ? "on" : "off");
        } else if (strcasecmp(p, "cad") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->cad_enabled ? "on" : "off");
        } else if (strcasecmp(p, "repeat") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->client_repeat ? "on" : "off");
        } else if (strcasecmp(p, "af") == 0) {
            snprintf(reply, reply_sz, "%.1f\n", (double)current_cfg->airtime_factor_x10 / 10.0);
        } else if (strcasecmp(p, "dutycycle") == 0) {
            unsigned int pct = (unsigned int)(1000 / (10 + current_cfg->airtime_factor_x10));
            snprintf(reply, reply_sz, "%u%%\n", pct);
        } else if (strcasecmp(p, "path.hash.mode") == 0) {
            snprintf(reply, reply_sz, "%u\n", current_cfg->path_hash_mode);
        } else if (strcasecmp(p, "freq") == 0) {
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)current_cfg->freq_khz);
        } else if (strcasecmp(p, "bw") == 0) {
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)bw_hz);
        } else if (strcasecmp(p, "sf") == 0) {
            snprintf(reply, reply_sz, "%u\n", current_cfg->sf);
        } else if (strcasecmp(p, "cr") == 0) {
            snprintf(reply, reply_sz, "%u\n", current_cfg->cr);
        } else if (strcasecmp(p, "channel") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->channel_name);
        } else if (strcasecmp(p, "psk") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->channel_psk);
        } else if (strcasecmp(p, "loc") == 0) {
            snprintf(reply, reply_sz, "%s\n", current_cfg->share_loc ? "on" : "off");
        } else if (strcasecmp(p, "public.key") == 0) {
            snprintf(reply, reply_sz, "%s\n", stats->pub_key_hex);
        } else if (strcasecmp(p, "pin") == 0) {
            snprintf(reply, reply_sz, "%06lu\n", (unsigned long)stats->ble_pin);
        } else {
            snprintf(reply, reply_sz, "Unknown parameter 'get %s'\n", p);
        }
        return;
    }

    /* Handle 'set <param> <val>' */
    if (strncasecmp(cmd, "set ", 4) == 0) {
        const char *p = cmd + 4;
        while (*p == ' ' || *p == '\t') p++;
        char sparam[32] = {0};
        char sval[128] = {0};
        int parsed = sscanf(p, "%31s %127[^\r\n]", sparam, sval);
        if (parsed <= 0) {
            snprintf(reply, reply_sz, "Usage: set <param> <value>\n");
            return;
        }

        if (strcasecmp(sparam, "radio") == 0) {
            float f_val = 0.0f, bw_val = 0.0f;
            unsigned int sf_val = 0, cr_val = 0;
            if (sscanf(sval, "%f,%f,%u,%u", &f_val, &bw_val, &sf_val, &cr_val) >= 4) {
                uint32_t raw_freq = (f_val < 3000.0f) ? (uint32_t)(f_val * 1000.0f + 0.5f) : (uint32_t)(f_val + 0.5f);
                uint32_t hz = (bw_val < 1000.0f) ? (uint32_t)(bw_val * 1000.0f + 0.5f) : (uint32_t)(bw_val + 0.5f);
                if (!valid_bw_hz(hz)) {
                    snprintf(reply, reply_sz, "Error: invalid bandwidth %lu Hz\n", (unsigned long)hz);
                    return;
                }
                if (cfg_inout) {
                    cfg_inout->freq_khz = clamp_u32(raw_freq, 430000U, 928000U);
                    cfg_inout->bw_hz = hz;
                    cfg_inout->bw_khz = (uint16_t)(hz / 1000U);
                    cfg_inout->sf = (uint8_t)clamp_u32(sf_val, 5U, 12U);
                    cfg_inout->cr = (uint8_t)clamp_u32(cr_val, 5U, 8U);
                    current_cfg->freq_khz = cfg_inout->freq_khz;
                    current_cfg->bw_hz = cfg_inout->bw_hz;
                    current_cfg->bw_khz = cfg_inout->bw_khz;
                    current_cfg->sf = cfg_inout->sf;
                    current_cfg->cr = cfg_inout->cr;
                    stats->freq_khz = cfg_inout->freq_khz;
                    stats->bw_hz = cfg_inout->bw_hz;
                    stats->bw_khz = cfg_inout->bw_khz;
                    stats->sf = cfg_inout->sf;
                    stats->cr = cfg_inout->cr;
                    if (cfg_changed) *cfg_changed = true;
                }
                snprintf(reply, reply_sz, "Radio set: %lu kHz, %lu Hz, SF%u, CR4/%u\n",
                         (unsigned long)current_cfg->freq_khz,
                         (unsigned long)current_cfg->bw_hz,
                         current_cfg->sf, current_cfg->cr);
            } else {
                snprintf(reply, reply_sz, "Usage: set radio <freq>,<bw>,<sf>,<cr>\n");
            }
        } else if (strcasecmp(sparam, "tx") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                int32_t pwr = clamp_i32((int32_t)atoi(sval), -9, 22);
                cfg_inout->tx_power_dbm = (int8_t)pwr;
                current_cfg->tx_power_dbm = cfg_inout->tx_power_dbm;
                stats->tx_power_dbm = cfg_inout->tx_power_dbm;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%d\n", current_cfg->tx_power_dbm);
        } else if (strcasecmp(sparam, "name") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                strncpy(cfg_inout->node_name, sval, sizeof(cfg_inout->node_name) - 1);
                cfg_inout->node_name[sizeof(cfg_inout->node_name) - 1] = '\0';
                strncpy(current_cfg->node_name, cfg_inout->node_name, sizeof(current_cfg->node_name) - 1);
                current_cfg->node_name[sizeof(current_cfg->node_name) - 1] = '\0';
                strncpy(stats->node_name, cfg_inout->node_name, sizeof(stats->node_name) - 1);
                stats->node_name[sizeof(stats->node_name) - 1] = '\0';
                if (cfg_changed) *cfg_changed = true;
                if (env && env->update_ble_name) {
                    env->update_ble_name(env->user_data, cfg_inout->node_name);
                }
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->node_name);
        } else if (strcasecmp(sparam, "radio.rxgain") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->rx_boosted = parse_on_off(sval, cfg_inout->rx_boosted);
                current_cfg->rx_boosted = cfg_inout->rx_boosted;
                stats->rx_boosted = cfg_inout->rx_boosted;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->rx_boosted ? "on" : "off");
        } else if (strcasecmp(sparam, "cad") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->cad_enabled = parse_on_off(sval, cfg_inout->cad_enabled);
                current_cfg->cad_enabled = cfg_inout->cad_enabled;
                stats->cad_enabled = cfg_inout->cad_enabled;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->cad_enabled ? "on" : "off");
        } else if (strcasecmp(sparam, "repeat") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->client_repeat = parse_on_off(sval, cfg_inout->client_repeat);
                current_cfg->client_repeat = cfg_inout->client_repeat;
                stats->client_repeat = cfg_inout->client_repeat;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->client_repeat ? "on" : "off");
        } else if (strcasecmp(sparam, "af") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                float af = (float)atof(sval);
                if (af < 0.0f) af = 0.0f;
                if (af > 9.0f) af = 9.0f;
                cfg_inout->airtime_factor_x10 = (uint8_t)clamp_u32((uint32_t)(af * 10.0f + 0.5f), 0U, 90U);
                current_cfg->airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                stats->airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%.1f\n", (double)current_cfg->airtime_factor_x10 / 10.0);
        } else if (strcasecmp(sparam, "dutycycle") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                int32_t pct = clamp_i32((int32_t)atoi(sval), 1, 100);
                cfg_inout->airtime_factor_x10 = (uint8_t)clamp_u32((uint32_t)((100 - pct) * 10 / pct), 0U, 90U);
                current_cfg->airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                stats->airtime_factor_x10 = cfg_inout->airtime_factor_x10;
                if (cfg_changed) *cfg_changed = true;
            }
            unsigned int pct = (unsigned int)(1000 / (10 + current_cfg->airtime_factor_x10));
            snprintf(reply, reply_sz, "%u%%\n", pct);
        } else if (strcasecmp(sparam, "path.hash.mode") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                int32_t m = clamp_i32((int32_t)atoi(sval), 0, 2);
                cfg_inout->path_hash_mode = (uint8_t)m;
                current_cfg->path_hash_mode = cfg_inout->path_hash_mode;
                stats->path_hash_mode = cfg_inout->path_hash_mode;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%u\n", current_cfg->path_hash_mode);
        } else if (strcasecmp(sparam, "freq") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                float f = (float)atof(sval);
                uint32_t raw = (f < 3000.0f) ? (uint32_t)(f * 1000.0f + 0.5f) : (uint32_t)(f + 0.5f);
                cfg_inout->freq_khz = clamp_u32(raw, 430000U, 928000U);
                current_cfg->freq_khz = cfg_inout->freq_khz;
                stats->freq_khz = cfg_inout->freq_khz;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)current_cfg->freq_khz);
        } else if (strcasecmp(sparam, "bw") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                float bw = (float)atof(sval);
                uint32_t hz = (bw < 1000.0f) ? (uint32_t)(bw * 1000.0f + 0.5f) : (uint32_t)(bw + 0.5f);
                if (!valid_bw_hz(hz)) {
                    snprintf(reply, reply_sz, "Error: invalid bandwidth %lu Hz\n", (unsigned long)hz);
                    return;
                }
                cfg_inout->bw_hz = hz;
                cfg_inout->bw_khz = (uint16_t)(hz / 1000U);
                current_cfg->bw_hz = hz;
                current_cfg->bw_khz = cfg_inout->bw_khz;
                stats->bw_hz = hz;
                stats->bw_khz = cfg_inout->bw_khz;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%lu\n", (unsigned long)cfg_effective_bw_hz(current_cfg));
        } else if (strcasecmp(sparam, "sf") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->sf = (uint8_t)clamp_i32((int32_t)atoi(sval), 5, 12);
                current_cfg->sf = cfg_inout->sf;
                stats->sf = cfg_inout->sf;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%u\n", current_cfg->sf);
        } else if (strcasecmp(sparam, "cr") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->cr = (uint8_t)clamp_i32((int32_t)atoi(sval), 5, 8);
                current_cfg->cr = cfg_inout->cr;
                stats->cr = cfg_inout->cr;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%u\n", current_cfg->cr);
        } else if (strcasecmp(sparam, "channel") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                strncpy(cfg_inout->channel_name, sval, sizeof(cfg_inout->channel_name) - 1);
                cfg_inout->channel_name[sizeof(cfg_inout->channel_name) - 1] = '\0';
                strncpy(current_cfg->channel_name, cfg_inout->channel_name, sizeof(current_cfg->channel_name) - 1);
                current_cfg->channel_name[sizeof(current_cfg->channel_name) - 1] = '\0';
                strncpy(stats->channel_name, cfg_inout->channel_name, sizeof(stats->channel_name) - 1);
                stats->channel_name[sizeof(stats->channel_name) - 1] = '\0';
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->channel_name);
        } else if (strcasecmp(sparam, "psk") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                strncpy(cfg_inout->channel_psk, sval, sizeof(cfg_inout->channel_psk) - 1);
                cfg_inout->channel_psk[sizeof(cfg_inout->channel_psk) - 1] = '\0';
                strncpy(current_cfg->channel_psk, cfg_inout->channel_psk, sizeof(current_cfg->channel_psk) - 1);
                current_cfg->channel_psk[sizeof(current_cfg->channel_psk) - 1] = '\0';
                uint8_t secret32[32];
                uint8_t ch_hash = 0;
                if (meshcore_channel_key_from_psk(current_cfg->channel_psk, secret32, &ch_hash)) {
                    stats->channel_hash = ch_hash;
                }
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->channel_psk);
        } else if (strcasecmp(sparam, "loc") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                cfg_inout->share_loc = parse_on_off(sval, cfg_inout->share_loc);
                current_cfg->share_loc = cfg_inout->share_loc;
                stats->share_loc = cfg_inout->share_loc;
                if (cfg_changed) *cfg_changed = true;
            }
            snprintf(reply, reply_sz, "%s\n", current_cfg->share_loc ? "on" : "off");
        } else if (strcasecmp(sparam, "pin") == 0) {
            if (sval[0] != '\0' && cfg_inout) {
                uint32_t pin = (uint32_t)strtoul(sval, NULL, 10);
                if (pin >= 100000U && pin <= 999999U) {
                    cfg_inout->ble_pin = pin;
                    current_cfg->ble_pin = pin;
                    stats->ble_pin = pin;
                    if (cfg_changed) *cfg_changed = true;
                    if (env && env->update_ble_pin) {
                        env->update_ble_pin(env->user_data, pin);
                    }
                    snprintf(reply, reply_sz, "%06lu\n", (unsigned long)pin);
                } else {
                    snprintf(reply, reply_sz, "Error: PIN must be 100000..999999\n");
                }
            } else {
                snprintf(reply, reply_sz, "Usage: set pin <100000..999999>\n");
            }
        } else {
            snprintf(reply, reply_sz, "Unknown parameter 'set %s'\n", sparam);
        }
        return;
    }

    char op[32] = {0};
    char arg[128] = {0};
    int matched = sscanf(cmd, "%31s %127[^\r\n]", op, arg);

    if (matched <= 0) {
        snprintf(reply, reply_sz, "Invalid command.\n");
        return;
    }

    if (strcasecmp(op, "help") == 0) {
        snprintf(reply, reply_sz,
                 "MeshCore CLI Commands:\n"
                 "  ver                  - Get firmware version\n"
                 "  board                - Get hardware board name\n"
                 "  get/set radio        - Get or set freq,bw,sf,cr\n"
                 "  get/set tx           - Get or set TX power (dBm)\n"
                 "  get/set name         - Get or set node name\n"
                 "  get/set radio.rxgain - Get or set RX boosted gain (on/off)\n"
                 "  get/set cad          - Get or set CAD channel activity detection (on/off)\n"
                 "  get/set repeat       - Get or set client repeat mode (on/off)\n"
                 "  get/set af           - Get or set airtime factor\n"
                 "  get/set dutycycle    - Get or set duty cycle limit (%%)\n"
                 "  get/set path.hash.mode - Get or set path hash mode (0|1|2)\n"
                 "  get/set pin          - Get or set BLE companion 6-digit PIN\n"
                 "  stats-core           - Core node metrics\n"
                 "  stats-radio          - Radio layer metrics\n"
                 "  stats-packets        - Packet counters\n"
                 "  clear stats          - Reset statistics counters\n"
                 "  advert / adv         - Broadcast node advert (flood)\n"
                 "  advert.zerohop       - Broadcast zero-hop advert\n"
                 "  discover.neighbors   - Broadcast zero-hop discover request\n"
                 "  neighbors            - List direct zero-hop neighbors\n"
                 "  nodes / peers        - List heard node adverts\n"
                 "  msg / send <text>    - Send group channel text message\n"
                 "  messages             - Show recent chat messages\n"
                 "  reset-identity       - Generate new random Ed25519 keypair and default PIN\n");
    } else if (strcasecmp(op, "adv") == 0 || strcasecmp(op, "advert") == 0) {
        bool ok = (env && env->send_advert) ? env->send_advert(env->user_data, true) : false;
        snprintf(reply, reply_sz, "Advert broadcast: %s\n", ok ? "OK" : "FAIL");
    } else if (strcasecmp(op, "disc") == 0 || strcasecmp(op, "discover") == 0) {
        bool ok = (env && env->send_discover) ? env->send_discover(env->user_data) : false;
        snprintf(reply, reply_sz, "Discover request: %s\n", ok ? "OK" : "FAIL");
    } else if (strcasecmp(op, "msg") == 0 || strcasecmp(op, "send") == 0) {
        if (arg[0] == '\0') {
            snprintf(reply, reply_sz, "Usage: msg <text>\n");
        } else {
            bool ok = (env && env->send_channel_msg) ? env->send_channel_msg(env->user_data, 0, arg) : false;
            snprintf(reply, reply_sz, "Message send: %s\n", ok ? "OK" : "FAIL");
        }
    } else if (strcasecmp(op, "node") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            strncpy(cfg_inout->node_name, arg, sizeof(cfg_inout->node_name) - 1);
            cfg_inout->node_name[sizeof(cfg_inout->node_name) - 1] = '\0';
            strncpy(current_cfg->node_name, cfg_inout->node_name, sizeof(current_cfg->node_name) - 1);
            current_cfg->node_name[sizeof(current_cfg->node_name) - 1] = '\0';
            strncpy(stats->node_name, cfg_inout->node_name, sizeof(stats->node_name) - 1);
            stats->node_name[sizeof(stats->node_name) - 1] = '\0';
            if (cfg_changed) *cfg_changed = true;
            if (env && env->update_ble_name) {
                env->update_ble_name(env->user_data, cfg_inout->node_name);
            }
        }
        snprintf(reply, reply_sz, "Node name: %s\nPubKey: %.16s...\n",
                 current_cfg->node_name, stats->pub_key_hex);
    } else if (strcasecmp(op, "channel") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            strncpy(cfg_inout->channel_name, arg, sizeof(cfg_inout->channel_name) - 1);
            cfg_inout->channel_name[sizeof(cfg_inout->channel_name) - 1] = '\0';
            strncpy(current_cfg->channel_name, cfg_inout->channel_name, sizeof(current_cfg->channel_name) - 1);
            current_cfg->channel_name[sizeof(current_cfg->channel_name) - 1] = '\0';
            strncpy(stats->channel_name, cfg_inout->channel_name, sizeof(stats->channel_name) - 1);
            stats->channel_name[sizeof(stats->channel_name) - 1] = '\0';
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Channel: %s (hash 0x%02X)\n",
                 current_cfg->channel_name, stats->channel_hash);
    } else if (strcasecmp(op, "psk") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            strncpy(cfg_inout->channel_psk, arg, sizeof(cfg_inout->channel_psk) - 1);
            cfg_inout->channel_psk[sizeof(cfg_inout->channel_psk) - 1] = '\0';
            strncpy(current_cfg->channel_psk, cfg_inout->channel_psk, sizeof(current_cfg->channel_psk) - 1);
            current_cfg->channel_psk[sizeof(current_cfg->channel_psk) - 1] = '\0';
            uint8_t secret32[32];
            uint8_t ch_hash = 0;
            if (meshcore_channel_key_from_psk(current_cfg->channel_psk, secret32, &ch_hash)) {
                stats->channel_hash = ch_hash;
            }
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Channel PSK: %s (hash 0x%02X)\n",
                 current_cfg->channel_psk, stats->channel_hash);
    } else if (strcasecmp(op, "repeat") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->client_repeat = parse_on_off(arg, cfg_inout->client_repeat);
            current_cfg->client_repeat = cfg_inout->client_repeat;
            stats->client_repeat = cfg_inout->client_repeat;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Client repeat: %s\n", current_cfg->client_repeat ? "ON" : "OFF");
    } else if (strcasecmp(op, "loc") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->share_loc = parse_on_off(arg, cfg_inout->share_loc);
            current_cfg->share_loc = cfg_inout->share_loc;
            stats->share_loc = cfg_inout->share_loc;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Share location: %s\n", current_cfg->share_loc ? "ON" : "OFF");
    } else if (strcasecmp(op, "boost") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->rx_boosted = parse_on_off(arg, cfg_inout->rx_boosted);
            current_cfg->rx_boosted = cfg_inout->rx_boosted;
            stats->rx_boosted = cfg_inout->rx_boosted;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "RX Boosted: %s\n", current_cfg->rx_boosted ? "ON" : "OFF");
    } else if (strcasecmp(op, "power") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->tx_power_dbm = (int8_t)clamp_i32((int32_t)atoi(arg), -9, 22);
            current_cfg->tx_power_dbm = cfg_inout->tx_power_dbm;
            stats->tx_power_dbm = cfg_inout->tx_power_dbm;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "TX Power: %d dBm\n", current_cfg->tx_power_dbm);
    } else if (strcasecmp(op, "sf") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->sf = (uint8_t)clamp_i32((int32_t)atoi(arg), 5, 12);
            current_cfg->sf = cfg_inout->sf;
            stats->sf = cfg_inout->sf;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "SF: %u\n", current_cfg->sf);
    } else if (strcasecmp(op, "cr") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            cfg_inout->cr = (uint8_t)clamp_i32((int32_t)atoi(arg), 5, 8);
            current_cfg->cr = cfg_inout->cr;
            stats->cr = cfg_inout->cr;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "CR: 4/%u\n", current_cfg->cr);
    } else if (strcasecmp(op, "freq") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            float f = (float)atof(arg);
            uint32_t raw = (f < 3000.0f) ? (uint32_t)(f * 1000.0f + 0.5f) : (uint32_t)(f + 0.5f);
            cfg_inout->freq_khz = clamp_u32(raw, 430000U, 928000U);
            current_cfg->freq_khz = cfg_inout->freq_khz;
            stats->freq_khz = cfg_inout->freq_khz;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Frequency: %lu kHz\n", (unsigned long)current_cfg->freq_khz);
    } else if (strcasecmp(op, "bw") == 0) {
        if (arg[0] != '\0' && cfg_inout) {
            float bw = (float)atof(arg);
            uint32_t hz = (bw < 1000.0f) ? (uint32_t)(bw * 1000.0f + 0.5f) : (uint32_t)(bw + 0.5f);
            if (!valid_bw_hz(hz)) {
                snprintf(reply, reply_sz, "Error: invalid bandwidth %lu Hz\n", (unsigned long)hz);
                return;
            }
            cfg_inout->bw_hz = hz;
            cfg_inout->bw_khz = (uint16_t)(hz / 1000U);
            current_cfg->bw_hz = hz;
            current_cfg->bw_khz = cfg_inout->bw_khz;
            stats->bw_hz = hz;
            stats->bw_khz = cfg_inout->bw_khz;
            if (cfg_changed) *cfg_changed = true;
        }
        snprintf(reply, reply_sz, "Bandwidth: %lu Hz (%u kHz)\n",
                 (unsigned long)cfg_effective_bw_hz(current_cfg), current_cfg->bw_khz);
    } else if (strcasecmp(op, "stats") == 0 || strcasecmp(op, "status") == 0) {
        snprintf(reply, reply_sz,
                 "LoRa Statistics:\n"
                 "  Enabled: %s, Init: %s\n"
                 "  RX Packets: %lu (Flood: %lu, Direct: %lu, Errors: %lu)\n"
                 "  TX Packets: %lu (Flood: %lu, Direct: %lu)\n"
                 "  Signal: Last RSSI %d dBm, SNR %.2f dB, Noise %d dBm\n"
                 "  Airtime: TX %lu ms, RX %lu ms\n",
                 stats->enabled ? "YES" : "NO", stats->initialized ? "YES" : "NO",
                 (unsigned long)stats->packet_count, (unsigned long)stats->recv_flood,
                 (unsigned long)stats->recv_direct, (unsigned long)stats->recv_errors,
                 (unsigned long)stats->packets_sent, (unsigned long)stats->sent_flood,
                 (unsigned long)stats->sent_direct,
                 stats->last_rssi_dbm, (double)stats->last_snr_q4 / 4.0, stats->noise_floor_dbm,
                 (unsigned long)stats->tx_air_ms, (unsigned long)stats->rx_air_ms);
    } else if (strcasecmp(op, "nodes") == 0 || strcasecmp(op, "adverts") == 0 || strcasecmp(op, "peers") == 0) {
        size_t off = 0;
        int hdr = snprintf(reply, reply_sz, "Discovered Nodes (%u):\n", stats->advert_count);
        if (hdr > 0 && (size_t)hdr < reply_sz) off = (size_t)hdr;
        if (st) {
            for (uint8_t i = 0; i < stats->advert_count && i < MESHCORE_MAX_ADVERTS && off + 64 < reply_sz; i++) {
                int n = snprintf(reply + off, reply_sz - off,
                                 "  [%u] %-16s type %u, hops %u, RSSI %d dBm\n",
                                 (unsigned)(i + 1), st->adverts[i].name, st->adverts[i].node_type,
                                 st->adverts[i].hops, st->adverts[i].rssi_dbm);
                if (n <= 0 || (size_t)n >= reply_sz - off) break;
                off += (size_t)n;
            }
        }
    } else if (strcasecmp(op, "neighbors") == 0) {
        size_t off = 0;
        int hdr = snprintf(reply, reply_sz, "Direct Neighbors (%u):\n", stats->neighbor_count);
        if (hdr > 0 && (size_t)hdr < reply_sz) off = (size_t)hdr;
        if (st) {
            for (uint8_t i = 0; i < stats->neighbor_count && i < MESHCORE_MAX_NEIGHBORS && off + 64 < reply_sz; i++) {
                int n = snprintf(reply + off, reply_sz - off,
                                 "  [%u] prefix %02x%02x%02x%02x, RSSI %d dBm, SNR in %.2f dB\n",
                                 (unsigned)(i + 1),
                                 st->neighbors[i].pub_key_prefix[0], st->neighbors[i].pub_key_prefix[1],
                                 st->neighbors[i].pub_key_prefix[2], st->neighbors[i].pub_key_prefix[3],
                                 st->neighbors[i].rssi_dbm, (double)st->neighbors[i].snr_in_q4 / 4.0);
                if (n <= 0 || (size_t)n >= reply_sz - off) break;
                off += (size_t)n;
            }
        }
    } else if (strcasecmp(op, "messages") == 0) {
        size_t off = 0;
        int hdr = snprintf(reply, reply_sz, "Recent Messages (%u):\n", stats->msg_count);
        if (hdr > 0 && (size_t)hdr < reply_sz) off = (size_t)hdr;
        if (st) {
            for (uint8_t i = 0; i < stats->msg_count && i < MESHCORE_MAX_MESSAGES && off + 128 < reply_sz; i++) {
                int n = snprintf(reply + off, reply_sz - off,
                                 "  %s [%s]\n",
                                 st->messages[i].text,
                                 st->messages[i].outgoing ? "TX" : "RX");
                if (n <= 0 || (size_t)n >= reply_sz - off) break;
                off += (size_t)n;
            }
        }
    } else {
        snprintf(reply, reply_sz, "Unknown command '%s'. Try 'help'.\n", op);
    }
}
