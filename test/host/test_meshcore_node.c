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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_seen_packet_cache_dedup_and_ttl);
    RUN_TEST(test_advert_neighbor_and_message_tables);
    RUN_TEST(test_discover_ack_and_client_repeat);
    RUN_TEST(test_tx_airtime_budget_and_direct_txt_ack);
    RUN_TEST(test_cli_commands_and_validation);
    return UNITY_END();
}
