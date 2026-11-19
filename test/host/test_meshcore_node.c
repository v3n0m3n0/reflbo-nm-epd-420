#include "meshcore_node.h"
#include "ed_25519.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    int reset_count;
    int advert_count;
    bool last_advert_flood;
    int discover_count;
    int msg_count;
    uint8_t last_ch_idx;
    char last_msg_text[128];
    int ble_name_updates;
    char last_ble_name[32];
    int ble_pin_updates;
    uint32_t last_ble_pin;
    int rekey_count;
} mock_cli_ctx_t;

static void mock_reset_stats(void *ctx)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->reset_count++;
}

static bool mock_send_advert(void *ctx, bool flood)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->advert_count++;
    m->last_advert_flood = flood;
    return true;
}

static bool mock_send_discover(void *ctx)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->discover_count++;
    return true;
}

static bool mock_send_channel_msg(void *ctx, uint8_t ch_idx, const char *text)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->msg_count++;
    m->last_ch_idx = ch_idx;
    if (text) {
        strncpy(m->last_msg_text, text, sizeof(m->last_msg_text) - 1);
        m->last_msg_text[sizeof(m->last_msg_text) - 1] = '\0';
    }
    return true;
}

static void mock_update_ble_name(void *ctx, const char *node_name)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->ble_name_updates++;
    if (node_name) {
        strncpy(m->last_ble_name, node_name, sizeof(m->last_ble_name) - 1);
        m->last_ble_name[sizeof(m->last_ble_name) - 1] = '\0';
    }
}

static void mock_update_ble_pin(void *ctx, uint32_t pin)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->ble_pin_updates++;
    m->last_ble_pin = pin;
}

static bool mock_reset_identity(void *ctx)
{
    mock_cli_ctx_t *m = (mock_cli_ctx_t *)ctx;
    m->rekey_count++;
    return true;
}

static void test_seen_packet_cache_dedup_and_ttl(void)
{
    meshcore_node_state_t st;
    meshcore_node_init(&st);

    meshcore_packet_t pkt = {
        .header = (MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_GRP_TXT & 0x0F) << 2),
        .path_len = 1,
        .path = { 0xAA },
        .payload_len = 5,
        .payload = { 1, 2, 3, 4, 5 },
    };

    TEST_ASSERT_FALSE(meshcore_node_is_seen_and_mark(&st, &pkt, 1000U));
    TEST_ASSERT_TRUE(meshcore_node_is_seen_and_mark(&st, &pkt, 1000U + MESHCORE_SEEN_PKT_TTL_MS - 1U));
    /* After TTL expires, packet is no longer considered seen and timestamp updates */
    TEST_ASSERT_FALSE(meshcore_node_is_seen_and_mark(&st, &pkt, 1000U + MESHCORE_SEEN_PKT_TTL_MS + 10U));
    TEST_ASSERT_TRUE(meshcore_node_is_seen_and_mark(&st, &pkt, 1000U + MESHCORE_SEEN_PKT_TTL_MS + 20U));
}

static void test_advert_neighbor_and_message_tables(void)
{
    meshcore_node_state_t st;
    lora_stats_t stats = {0};
    meshcore_node_init(&st);

    /* Adverts: insert, update by pub_key, remove */
    meshcore_advert_t adv1 = { .rx_time_s = 100, .node_type = 1, .rssi_dbm = -80 };
    memset(adv1.pub_key, 0x11, 32);
    strcpy(adv1.name, "Alpha");
    meshcore_node_update_advert(&st, &stats, &adv1);
    TEST_ASSERT_EQUAL_UINT8(1, stats.advert_count);

    meshcore_advert_t adv1_upd = adv1;
    adv1_upd.rssi_dbm = -70;
    meshcore_node_update_advert(&st, &stats, &adv1_upd);
    TEST_ASSERT_EQUAL_UINT8(1, stats.advert_count);
    TEST_ASSERT_EQUAL_INT16(-70, st.adverts[0].rssi_dbm);

    TEST_ASSERT_TRUE(meshcore_node_remove_advert_by_pubkey(&st, &stats, adv1.pub_key));
    TEST_ASSERT_EQUAL_UINT8(0, stats.advert_count);
    TEST_ASSERT_FALSE(meshcore_node_remove_advert_by_pubkey(&st, &stats, adv1.pub_key));

    /* Neighbors: fill + evict oldest */
    for (int i = 0; i < MESHCORE_MAX_NEIGHBORS; i++) {
        meshcore_neighbor_t nb = { .rx_time_s = (uint32_t)(200 + i), .rssi_dbm = -90 };
        memset(nb.pub_key_prefix, (uint8_t)(i + 1), 8);
        meshcore_node_update_neighbor(&st, &stats, &nb);
    }
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_MAX_NEIGHBORS, stats.neighbor_count);

    meshcore_neighbor_t nb_new = { .rx_time_s = 999, .rssi_dbm = -60 };
    memset(nb_new.pub_key_prefix, 0xFE, 8);
    meshcore_node_update_neighbor(&st, &stats, &nb_new);
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_MAX_NEIGHBORS, stats.neighbor_count);
    TEST_ASSERT_EQUAL_HEX8(0xFE, st.neighbors[0].pub_key_prefix[0]);

    /* Messages: dup detection & ring buffer */
    meshcore_msg_t msg = { .timestamp = 5000, .rx_time_s = 5000 };
    strcpy(msg.text, "Alice: Hi");
    TEST_ASSERT_FALSE(meshcore_node_is_dup_message(&st, &stats, 5000, 5001, "Alice: Hi"));
    meshcore_node_append_message(&st, &stats, &msg);
    TEST_ASSERT_EQUAL_UINT8(1, stats.msg_count);
    TEST_ASSERT_EQUAL_UINT32(1, stats.msg_seq);
    TEST_ASSERT_TRUE(meshcore_node_is_dup_message(&st, &stats, 5000, 5002, "Alice: Hi"));
    TEST_ASSERT_FALSE(meshcore_node_is_dup_message(&st, &stats, 5001, 5002, "Alice: Hi"));
}

static void test_discover_ack_and_client_repeat(void)
{
    uint8_t self_pub[32];
    for (int i = 0; i < 32; i++) self_pub[i] = (uint8_t)(0xA0 + i);

    meshcore_packet_t req = {
        .header = (MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_CONTROL & 0x0F) << 2),
        .payload_len = 6,
        .payload = { 0x81, 0x00, 0xDE, 0xAD, 0xBE, 0xEF },
    };
    uint8_t raw[64];
    size_t raw_sz = meshcore_node_build_discover_resp(&req, 12, self_pub, raw, sizeof(raw));
    TEST_ASSERT_TRUE(raw_sz > 0);

    meshcore_packet_t resp;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&resp, raw, raw_sz));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_DIRECT, meshcore_packet_get_route_type(&resp));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_CONTROL, meshcore_packet_get_payload_type(&resp));
    TEST_ASSERT_EQUAL_UINT8(0x90 | MESHCORE_ADV_CHAT, resp.payload[0]);
    TEST_ASSERT_EQUAL_INT8(12, (int8_t)resp.payload[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&req.payload[2], &resp.payload[2], 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(self_pub, &resp.payload[6], 8);

    /* ACK frame: direct path vs flood fallback */
    uint8_t plain[12] = { 1, 2, 3, 4, 0, 'O', 'K' };
    uint8_t path[2] = { 0x33, 0x44 };
    size_t ack_sz = meshcore_node_build_ack(plain, 7, self_pub, 2, path, raw, sizeof(raw));
    TEST_ASSERT_TRUE(ack_sz > 0);
    meshcore_packet_t ack;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&ack, raw, ack_sz));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_DIRECT, meshcore_packet_get_route_type(&ack));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_ACK, meshcore_packet_get_payload_type(&ack));
    TEST_ASSERT_EQUAL_UINT(4, ack.payload_len);

    size_t ack_flood_sz = meshcore_node_build_ack(plain, 7, self_pub, -1, NULL, raw, sizeof(raw));
    meshcore_packet_t ack_flood;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&ack_flood, raw, ack_flood_sz));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_FLOOD, meshcore_packet_get_route_type(&ack_flood));

    /* Client repeat: appends self pubkey hash according to path_hash_mode and prevents loops */
    meshcore_packet_t rx_flood = {
        .header = (uint8_t)((MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_GRP_TXT & 0x0F) << 2)),
        .path_len = 0,
        .payload_len = 4,
        .payload = { 0x11, 0x22, 0x33, 0x44 },
    };
    uint8_t fwd_raw[128];
    size_t fwd_len = 0;

    for (uint8_t mode = 0; mode <= 2; mode++) {
        TEST_ASSERT_TRUE(meshcore_node_prepare_client_repeat(&rx_flood, self_pub, mode, fwd_raw, sizeof(fwd_raw), &fwd_len));
        meshcore_packet_t fwd;
        TEST_ASSERT_TRUE(meshcore_packet_decode(&fwd, fwd_raw, fwd_len));
        TEST_ASSERT_EQUAL_UINT8(1, meshcore_packet_get_hop_count(&fwd));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(mode + 1U), meshcore_packet_get_hash_size(&fwd));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(self_pub, fwd.path, (size_t)(mode + 1U));

        /* Repeating again must fail loop prevention check */
        TEST_ASSERT_FALSE(meshcore_node_prepare_client_repeat(&fwd, self_pub, mode, fwd_raw, sizeof(fwd_raw), &fwd_len));
    }

    meshcore_packet_t rx_max_hops = rx_flood;
    rx_max_hops.path_len = 7;
    TEST_ASSERT_FALSE(meshcore_node_prepare_client_repeat(&rx_max_hops, self_pub, 0, fwd_raw, sizeof(fwd_raw), &fwd_len));

    meshcore_packet_t rx_direct = rx_flood;
    rx_direct.header = (uint8_t)((MESHCORE_ROUTE_DIRECT & 0x03) | ((MESHCORE_PAYLOAD_GRP_TXT & 0x0F) << 2));
    TEST_ASSERT_FALSE(meshcore_node_prepare_client_repeat(&rx_direct, self_pub, 0, fwd_raw, sizeof(fwd_raw), &fwd_len));
}

static void test_cli_commands_and_validation(void)
{
    meshcore_node_state_t st;
    meshcore_node_init(&st);
    lora_config_t cur = {
        .enabled = true,
        .freq_khz = 868000,
        .bw_hz = 250000,
        .bw_khz = 250,
        .sf = 7,
        .cr = 5,
        .tx_power_dbm = 22,
        .sync_word = 0x1424,
        .ble_pin = 123456,
        .node_name = "TestNode",
        .channel_name = "Public",
        .channel_psk = LORA_DEFAULT_PUBLIC_PSK,
    };
    lora_config_t cfg_inout = cur;
    lora_stats_t stats = {
        .initialized = true,
        .enabled = true,
        .freq_khz = 868000,
        .bw_hz = 250000,
        .bw_khz = 250,
        .sf = 7,
        .cr = 5,
        .tx_power_dbm = 22,
        .ble_pin = 123456,
    };
    strcpy(stats.pub_key_hex, "AABBCCDD");

    mock_cli_ctx_t m = {0};
    meshcore_cli_env_t env = {
        .reset_stats = mock_reset_stats,
        .send_advert = mock_send_advert,
        .send_discover = mock_send_discover,
        .send_channel_msg = mock_send_channel_msg,
        .update_ble_name = mock_update_ble_name,
        .update_ble_pin = mock_update_ble_pin,
        .reset_identity = mock_reset_identity,
        .user_data = &m,
    };

    char reply[2048];
    bool changed = false;

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "help", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "MeshCore CLI Commands"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "reset-identity"));
    TEST_ASSERT_LESS_THAN(sizeof(reply) - 1, strlen(reply));
    TEST_ASSERT_FALSE(changed);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "ver", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "v1.1.0"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "MeshCore v1.18.0"));

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "get freq", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_STRING("868000\n", reply);

    /* Out-of-range frequency & TX clamping */
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set freq 999999", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(928000U, cfg_inout.freq_khz);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set tx 99", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT8(22, cfg_inout.tx_power_dbm);

    /* Invalid BW rejected cleanly */
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set bw 12345", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_NOT_NULL(strstr(reply, "Error: invalid bandwidth"));
    TEST_ASSERT_EQUAL_UINT32(250000U, cfg_inout.bw_hz);

    /* Valid BW 62.5 kHz */
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set bw 62.5", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(62500U, cfg_inout.bw_hz);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set sf 15", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT8(12, cfg_inout.sf);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set cr 4", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT8(5, cfg_inout.cr);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set repeat on", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_TRUE(cfg_inout.client_repeat);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set name MyNode", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT(1, m.ble_name_updates);
    TEST_ASSERT_EQUAL_STRING("MyNode", m.last_ble_name);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set pin 654321", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT(1, m.ble_pin_updates);
    TEST_ASSERT_EQUAL_UINT32(654321U, m.last_ble_pin);

    /* Actions */
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "advert", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, m.advert_count);
    TEST_ASSERT_TRUE(m.last_advert_flood);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "discover.neighbors", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, m.discover_count);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "msg hello world", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, m.msg_count);
    TEST_ASSERT_EQUAL_STRING("hello world", m.last_msg_text);

    /* Listing commands */
    meshcore_advert_t adv = { .node_type = 1, .hops = 0, .rssi_dbm = -75 };
    strcpy(adv.name, "PeerOne");
    meshcore_node_update_advert(&st, &stats, &adv);
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "adverts", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "PeerOne"));

    meshcore_neighbor_t nb = { .rssi_dbm = -82, .snr_in_q4 = 20 };
    nb.pub_key_prefix[0] = 0xDE;
    nb.pub_key_prefix[1] = 0xAD;
    nb.pub_key_prefix[2] = 0xBE;
    nb.pub_key_prefix[3] = 0xEF;
    meshcore_node_update_neighbor(&st, &stats, &nb);
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "neighbors", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "deadbeef"));

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set psk AQIDBAUGBwgJCgsMDQ4PEA==", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_STRING("AQIDBAUGBwgJCgsMDQ4PEA==", cfg_inout.channel_psk);
    /* Private PSK must be masked in CLI replies */
    TEST_ASSERT_NULL(strstr(reply, "AQIDBAUGBwgJCgsMDQ4PEA=="));
    TEST_ASSERT_NOT_NULL(strstr(reply, "<set>"));
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "get psk", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_NULL(strstr(reply, "AQIDBAUGBwgJCgsMDQ4PEA=="));
    TEST_ASSERT_NOT_NULL(strstr(reply, "<set>"));
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "psk", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_NULL(strstr(reply, "AQIDBAUGBwgJCgsMDQ4PEA=="));
    TEST_ASSERT_NOT_NULL(strstr(reply, "<set>"));

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "set radio 869.525,62.5,8,5", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(869525U, cfg_inout.freq_khz);
    TEST_ASSERT_EQUAL_UINT32(62500U, cfg_inout.bw_hz);
    TEST_ASSERT_EQUAL_UINT8(8U, cfg_inout.sf);
    TEST_ASSERT_EQUAL_UINT8(5U, cfg_inout.cr);
    TEST_ASSERT_EQUAL_STRING("AQIDBAUGBwgJCgsMDQ4PEA==", cfg_inout.channel_psk);
    meshcore_node_exec_cli(&st, &cur, &stats, &env, "get radio", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_STRING("869.525,62.5,8,5\n", reply);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "clear stats", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, m.reset_count);

    meshcore_node_exec_cli(&st, &cur, &stats, &env, "reset-identity", &cfg_inout, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, m.rekey_count);
    TEST_ASSERT_NOT_NULL(strstr(reply, "Identity reset: OK"));
}

static void test_tx_airtime_budget_and_direct_txt_ack(void)
{
    meshcore_node_state_t st;
    meshcore_node_init(&st);

    /* Airtime TX budget pacing */
    TEST_ASSERT_EQUAL_UINT32(0, meshcore_node_tx_wait_ms(&st, 1000U));
    meshcore_node_record_tx_airtime(&st, 1000U, 400U, 25U); /* 400 * 2.5 = 1000 ms silence */
    TEST_ASSERT_EQUAL_UINT32(500U, meshcore_node_tx_wait_ms(&st, 1500U));
    TEST_ASSERT_EQUAL_UINT32(0U, meshcore_node_tx_wait_ms(&st, 2000U));
    TEST_ASSERT_EQUAL_UINT32(0U, meshcore_node_tx_wait_ms(&st, 2500U));

    /* End-to-end encrypted direct TXT_MSG + ACK + RTT */
    uint8_t seed_alice[32], seed_bob[32];
    for (int i = 0; i < 32; i++) {
        seed_alice[i] = (uint8_t)(0x10 + i);
        seed_bob[i] = (uint8_t)(0x80 + i);
    }
    uint8_t alice_pub[32], alice_prv[64];
    uint8_t bob_pub[32], bob_prv[64];
    meshcore_ed25519_create_keypair(alice_pub, alice_prv, seed_alice);
    meshcore_ed25519_create_keypair(bob_pub, bob_prv, seed_bob);

    uint8_t ack_tag[4] = {0};
    uint8_t txt_raw[256];
    const char *msg_text = "Hello Bob!";
    size_t txt_len = meshcore_node_build_direct_txt(alice_pub, alice_prv, bob_pub, -1, NULL,
                                                    0, 0, 1700001111U, msg_text, ack_tag,
                                                    txt_raw, sizeof(txt_raw));
    TEST_ASSERT_TRUE(txt_len > 0U);

    meshcore_packet_t rx_txt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&rx_txt, txt_raw, txt_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_FLOOD, meshcore_packet_get_route_type(&rx_txt));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_TXT_MSG, meshcore_packet_get_payload_type(&rx_txt));
    TEST_ASSERT_EQUAL_UINT8(bob_pub[0], rx_txt.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(alice_pub[0], rx_txt.payload[1]);
    TEST_ASSERT_TRUE(rx_txt.payload_len > 4U);

    uint8_t shared_bob[32];
    ed25519_key_exchange(shared_bob, alice_pub, bob_prv);
    uint8_t plain[200] = {0};
    int plain_len = meshcore_mac_then_decrypt(shared_bob, plain, sizeof(plain),
                                              &rx_txt.payload[2], rx_txt.payload_len - 2U);
    TEST_ASSERT_EQUAL_INT(16, plain_len); /* AES-128-ECB pads 15B plaintext to 16B */
    uint32_t rx_ts = 0;
    memcpy(&rx_ts, plain, 4);
    TEST_ASSERT_EQUAL_UINT32(1700001111U, rx_ts);
    TEST_ASSERT_EQUAL_UINT8(0, plain[4]);
    TEST_ASSERT_EQUAL_STRING(msg_text, (const char *)&plain[5]);

    size_t unpadded_len = 5U + strlen((const char *)&plain[5]);
    TEST_ASSERT_EQUAL_UINT(15U, unpadded_len);
    uint8_t ack_raw[64];
    size_t ack_len = meshcore_node_build_ack(plain, unpadded_len, alice_pub, -1, NULL, ack_raw, sizeof(ack_raw));
    TEST_ASSERT_TRUE(ack_len > 0U);
    meshcore_packet_t rx_ack;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&rx_ack, ack_raw, ack_len));

    meshcore_node_expect_ack(&st, ack_tag, 5000U);
    uint8_t got_tag[4] = {0};
    uint32_t rtt_ms = 0;
    TEST_ASSERT_TRUE(meshcore_node_check_ack(&st, &rx_ack, 5350U, got_tag, &rtt_ms));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ack_tag, got_tag, 4);
    TEST_ASSERT_EQUAL_UINT32(350U, rtt_ms);

    /* Duplicate ACK must be ignored once pending ACK was cleared */
    TEST_ASSERT_FALSE(meshcore_node_check_ack(&st, &rx_ack, 5400U, got_tag, &rtt_ms));
}

static void mock_list_contacts(void *ctx, char *reply, size_t reply_sz)
{
    int *called = (int *)ctx;
    if (called) (*called)++;
    snprintf(reply, reply_sz, "Contacts: Alice\n");
}

static void mock_list_channels(void *ctx, char *reply, size_t reply_sz)
{
    int *called = (int *)ctx;
    if (called) (*called)++;
    snprintf(reply, reply_sz, "Channels: #0 Public\n");
}

static void test_message_id_ring_parsing_and_acks(void)
{
    meshcore_node_state_t st;
    lora_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    meshcore_node_init(&st);

    /* 1. Auto-split "Alice: Hi" and "[#Emerg] Bob: SOS" */
    meshcore_msg_t m1 = {0};
    snprintf(m1.text, sizeof(m1.text), "Alice: Hi");
    meshcore_node_append_message(&st, &stats, &m1);
    TEST_ASSERT_EQUAL_UINT32(1U, st.messages[0].id);
    TEST_ASSERT_EQUAL_STRING("Alice", st.messages[0].sender);
    TEST_ASSERT_EQUAL_STRING("Hi", st.messages[0].body);

    meshcore_msg_t m2 = {0};
    snprintf(m2.text, sizeof(m2.text), "[#Emerg] Bob: SOS");
    meshcore_node_append_message(&st, &stats, &m2);
    TEST_ASSERT_EQUAL_UINT32(2U, st.messages[1].id);
    TEST_ASSERT_EQUAL_STRING("Bob", st.messages[1].sender);
    TEST_ASSERT_EQUAL_STRING("SOS", st.messages[1].body);

    /* 2. Direct message ACK confirm & timeout */
    uint8_t tag1[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t tag2[4] = {0x55, 0x66, 0x77, 0x88};
    meshcore_msg_t dm1 = {0};
    dm1.is_direct = true;
    dm1.outgoing = true;
    dm1.ack_status = MESHCORE_ACK_PENDING;
    memcpy(dm1.ack_tag, tag1, 4);
    snprintf(dm1.text, sizeof(dm1.text), "To Alice: ping");
    meshcore_node_append_message(&st, &stats, &dm1);

    meshcore_msg_t dm2 = {0};
    dm2.is_direct = true;
    dm2.outgoing = true;
    dm2.ack_status = MESHCORE_ACK_PENDING;
    memcpy(dm2.ack_tag, tag2, 4);
    snprintf(dm2.text, sizeof(dm2.text), "To Bob: ping2");
    meshcore_node_append_message(&st, &stats, &dm2);

    uint32_t seq_before = stats.msg_seq;
    TEST_ASSERT_TRUE(meshcore_node_confirm_msg_ack(&st, &stats, tag1, 123U));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ACK_DELIVERED, st.messages[2].ack_status);
    TEST_ASSERT_EQUAL_UINT16(123U, st.messages[2].ack_rtt_ms);
    TEST_ASSERT_EQUAL_UINT32(seq_before + 1U, stats.msg_seq);

    TEST_ASSERT_TRUE(meshcore_node_timeout_msg_ack(&st, &stats, tag2));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ACK_TIMEOUT, st.messages[3].ack_status);
    TEST_ASSERT_EQUAL_UINT32(seq_before + 2U, stats.msg_seq);

    /* 3. Ring eviction at MESHCORE_MAX_MESSAGES (64) */
    for (uint32_t i = 0; i < 65U; i++) {
        meshcore_msg_t fill = {0};
        snprintf(fill.text, sizeof(fill.text), "msg-%lu", (unsigned long)i);
        meshcore_node_append_message(&st, &stats, &fill);
    }
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_MAX_MESSAGES, stats.msg_count);
    TEST_ASSERT_EQUAL_UINT32(69U, st.messages[MESHCORE_MAX_MESSAGES - 1].id);
    TEST_ASSERT_EQUAL_UINT32(6U, st.messages[0].id);
}

static void test_cli_contacts_and_channels_callbacks(void)
{
    meshcore_node_state_t st;
    lora_config_t cfg = {0};
    lora_stats_t stats = {0};
    meshcore_node_init(&st);

    int contacts_called = 0;
    int channels_called = 0;
    meshcore_cli_env_t env = {0};
    char reply[256];
    bool changed = false;

    /* Without callbacks */
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "contacts", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "Contacts unavailable"));
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "channels", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "Channels unavailable"));

    /* With callbacks */
    env.list_contacts = mock_list_contacts;
    env.list_channels = mock_list_channels;
    env.user_data = &contacts_called;
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "contacts", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, contacts_called);
    TEST_ASSERT_NOT_NULL(strstr(reply, "Contacts: Alice"));

    env.user_data = &channels_called;
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "channels", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_EQUAL_INT(1, channels_called);
    TEST_ASSERT_NOT_NULL(strstr(reply, "Channels: #0 Public"));
}

static void test_multi_pending_acks_and_3byte_direct_path(void)
{
    meshcore_node_state_t st;
    lora_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    meshcore_node_init(&st);

    uint8_t t1[4] = {0x01, 0x02, 0x03, 0x04};
    uint8_t t2[4] = {0x05, 0x06, 0x07, 0x08};
    uint8_t t3[4] = {0x09, 0x0A, 0x0B, 0x0C};

    meshcore_msg_t m1 = { .is_direct = true, .outgoing = true, .ack_status = MESHCORE_ACK_PENDING };
    memcpy(m1.ack_tag, t1, 4);
    snprintf(m1.text, sizeof(m1.text), "DM1");
    meshcore_node_append_message(&st, &stats, &m1);
    meshcore_node_expect_ack(&st, t1, 1000U);

    meshcore_msg_t m2 = { .is_direct = true, .outgoing = true, .ack_status = MESHCORE_ACK_PENDING };
    memcpy(m2.ack_tag, t2, 4);
    snprintf(m2.text, sizeof(m2.text), "DM2");
    meshcore_node_append_message(&st, &stats, &m2);
    meshcore_node_expect_ack(&st, t2, 1100U);

    meshcore_msg_t m3 = { .is_direct = true, .outgoing = true, .ack_status = MESHCORE_ACK_PENDING };
    memcpy(m3.ack_tag, t3, 4);
    snprintf(m3.text, sizeof(m3.text), "DM3");
    meshcore_node_append_message(&st, &stats, &m3);
    meshcore_node_expect_ack(&st, t3, 1200U);

    /* Out-of-order ACK for DM3 arrives first, then DM1; DM2 must remain pending until timeout */
    meshcore_packet_t ack_pkt = {0};
    ack_pkt.header = (uint8_t)((MESHCORE_ROUTE_DIRECT & 0x03) | ((MESHCORE_PAYLOAD_ACK & 0x0F) << 2));
    ack_pkt.payload_len = 4;

    uint8_t got_tag[4] = {0};
    uint32_t rtt = 0;
    memcpy(ack_pkt.payload, t3, 4);
    TEST_ASSERT_TRUE(meshcore_node_check_ack(&st, &ack_pkt, 1450U, got_tag, &rtt));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(t3, got_tag, 4);
    TEST_ASSERT_EQUAL_UINT32(250U, rtt);
    TEST_ASSERT_TRUE(meshcore_node_confirm_msg_ack(&st, &stats, got_tag, (uint16_t)rtt));

    memcpy(ack_pkt.payload, t1, 4);
    TEST_ASSERT_TRUE(meshcore_node_check_ack(&st, &ack_pkt, 1600U, got_tag, &rtt));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(t1, got_tag, 4);
    TEST_ASSERT_EQUAL_UINT32(600U, rtt);
    TEST_ASSERT_TRUE(meshcore_node_confirm_msg_ack(&st, &stats, got_tag, (uint16_t)rtt));

    /* Expire remaining pending ACK (DM2) after 15000 ms */
    TEST_ASSERT_EQUAL_UINT(0U, meshcore_node_expire_acks(&st, &stats, 16099U, 15000U));
    TEST_ASSERT_EQUAL_UINT(1U, meshcore_node_expire_acks(&st, &stats, 16100U, 15000U));

    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ACK_DELIVERED, st.messages[0].ack_status);
    TEST_ASSERT_EQUAL_UINT16(600U, st.messages[0].ack_rtt_ms);
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ACK_TIMEOUT, st.messages[1].ack_status);
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ACK_DELIVERED, st.messages[2].ack_status);
    TEST_ASSERT_EQUAL_UINT16(250U, st.messages[2].ack_rtt_ms);

    /* Verify 3-byte path hash mode (0x80 | 2 hops = -126 as int8_t) builds DIRECT packets */
    uint8_t seed_a[32], seed_b[32], pub_a[32], prv_a[64], pub_b[32], prv_b[64];
    memset(seed_a, 0x21, sizeof(seed_a));
    memset(seed_b, 0x42, sizeof(seed_b));
    meshcore_ed25519_create_keypair(pub_a, prv_a, seed_a);
    meshcore_ed25519_create_keypair(pub_b, prv_b, seed_b);

    uint8_t path_3b[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    int8_t path_len_3b = (int8_t)0x82; /* hash_size=3 ((2)+1), hops=2 */
    uint8_t raw[256];
    uint8_t dm_tag[4];
    size_t raw_len = meshcore_node_build_direct_txt(pub_a, prv_a, pub_b, path_len_3b, path_3b,
                                                    0, 0, 1700002222U, "3B-route", dm_tag, raw, sizeof(raw));
    TEST_ASSERT_TRUE(raw_len > 0U);
    meshcore_packet_t dec_dm;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&dec_dm, raw, raw_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_DIRECT, meshcore_packet_get_route_type(&dec_dm));
    TEST_ASSERT_EQUAL_UINT8(3U, meshcore_packet_get_hash_size(&dec_dm));
    TEST_ASSERT_EQUAL_UINT8(2U, meshcore_packet_get_hop_count(&dec_dm));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(path_3b, dec_dm.path, 6);

    uint8_t ack_raw[64];
    uint8_t dummy_plain[10] = {1, 2, 3, 4, 0, 'h', 'i', 0};
    size_t ack_raw_len = meshcore_node_build_ack(dummy_plain, 7, pub_a, path_len_3b, path_3b, ack_raw, sizeof(ack_raw));
    TEST_ASSERT_TRUE(ack_raw_len > 0U);
    meshcore_packet_t dec_ack;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&dec_ack, ack_raw, ack_raw_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_DIRECT, meshcore_packet_get_route_type(&dec_ack));
    TEST_ASSERT_EQUAL_UINT8(3U, meshcore_packet_get_hash_size(&dec_ack));
    TEST_ASSERT_EQUAL_UINT8(2U, meshcore_packet_get_hop_count(&dec_ack));
}

static void test_cli_messages_tail_utf8_truncation_and_neighbor_names(void)
{
    meshcore_node_state_t st;
    lora_config_t cfg = {0};
    lora_stats_t stats = {0};
    meshcore_cli_env_t env = {0};
    bool changed = false;
    meshcore_node_init(&st);

    /* 1. Neighbor name resolution from adverts table */
    meshcore_advert_t adv = { .node_type = 2, .rssi_dbm = -72 };
    memset(adv.pub_key, 0x00, sizeof(adv.pub_key));
    adv.pub_key[0] = 0x44; adv.pub_key[1] = 0x62; adv.pub_key[2] = 0x5F; adv.pub_key[3] = 0x6C;
    strcpy(adv.name, "SQ7ECZ-P1");
    meshcore_node_update_advert(&st, &stats, &adv);

    meshcore_neighbor_t nb = { .rssi_dbm = -80, .snr_in_q4 = 29 };
    memcpy(nb.pub_key_prefix, adv.pub_key, 8);
    meshcore_node_update_neighbor(&st, &stats, &nb);

    char reply[2048];
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "neighbors", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "SQ7ECZ-P1 (prefix 44625f6c)"));

    /* 2. Fill 30 messages including double-hash legacy text "[##test] Zeno: Ping" and UTF-8 emoji */
    for (uint32_t i = 1; i <= 30U; i++) {
        meshcore_msg_t m = {0};
        m.channel_idx = (i == 30U) ? 1U : 0U;
        if (i == 30U) {
            snprintf(m.text, sizeof(m.text), "[##test] Zeno☀️: Ostatnia wiadomość");
        } else {
            snprintf(m.text, sizeof(m.text), "User%lu: msg-%lu", (unsigned long)i, (unsigned long)i);
        }
        meshcore_node_append_message(&st, &stats, &m);
    }

    /* Default 'messages' shows newest 20 (omits 10 older) and strips duplicate [##test] */
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "messages", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "(10 older omitted)"));
    TEST_ASSERT_NULL(strstr(reply, "msg-10 "));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#11 [Public] User11: msg-11"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#30 [#1] Zeno☀️: Ostatnia wiadomość [RX]"));
    TEST_ASSERT_NULL(strstr(reply, "[##test]"));

    /* 'messages 3' shows only newest 3 */
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "messages 3", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "(27 older omitted)"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#28 "));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#30 "));

    /* Small BLE-sized buffer (128B) truncates cleanly with footer and valid UTF-8 */
    char small_reply[128];
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "messages 10", &cfg, &changed, small_reply, sizeof(small_reply));
    TEST_ASSERT_NOT_NULL(strstr(small_reply, "more)"));
}

static void test_cli_messages_channel_names_and_noise_masking(void)
{
    meshcore_node_state_t st;
    lora_config_t cfg = {0};
    lora_stats_t stats = {0};
    meshcore_node_init(&st);

    char ch_names[4][LORA_CHANNEL_NAME_LEN] = {
        "Public",
        "Ops",
        "#bot",
        ""
    };
    meshcore_cli_env_t env = {
        .channel_names = ch_names,
        .channel_names_count = 4,
    };

    meshcore_msg_t m0 = {0};
    m0.channel_idx = 0;
    snprintf(m0.sender, sizeof(m0.sender), "Alice");
    snprintf(m0.body, sizeof(m0.body), "hello public");
    meshcore_node_append_message(&st, &stats, &m0);

    meshcore_msg_t m1 = {0};
    m1.channel_idx = 1;
    snprintf(m1.sender, sizeof(m1.sender), "Bob");
    snprintf(m1.body, sizeof(m1.body), "ops update");
    meshcore_node_append_message(&st, &stats, &m1);

    meshcore_msg_t m2 = {0};
    m2.channel_idx = 2;
    snprintf(m2.sender, sizeof(m2.sender), "Cara");
    snprintf(m2.body, sizeof(m2.body), "bot ping");
    meshcore_node_append_message(&st, &stats, &m2);

    char reply[512];
    bool changed = false;
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "messages", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#1 [Public] Alice: hello public [RX]"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#2 [Ops] Bob: ops update [RX]"));
    TEST_ASSERT_NULL(strstr(reply, "[#Ops]"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "#3 [#bot] Cara: bot ping [RX]"));
    TEST_ASSERT_NULL(strstr(reply, "[#2]"));

    /* stats masks unsampled noise_floor_dbm == 0 as "Noise --" */
    stats.noise_floor_dbm = 0;
    meshcore_node_exec_cli(&st, &cfg, &stats, &env, "stats", &cfg, &changed, reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "Noise --"));
    TEST_ASSERT_NULL(strstr(reply, "Noise 0 dBm"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_seen_packet_cache_dedup_and_ttl);
    RUN_TEST(test_advert_neighbor_and_message_tables);
    RUN_TEST(test_discover_ack_and_client_repeat);
    RUN_TEST(test_tx_airtime_budget_and_direct_txt_ack);
    RUN_TEST(test_cli_commands_and_validation);
    RUN_TEST(test_message_id_ring_parsing_and_acks);
    RUN_TEST(test_cli_contacts_and_channels_callbacks);
    RUN_TEST(test_multi_pending_acks_and_3byte_direct_path);
    RUN_TEST(test_cli_messages_tail_utf8_truncation_and_neighbor_names);
    RUN_TEST(test_cli_messages_channel_names_and_noise_masking);
    return UNITY_END();
}
