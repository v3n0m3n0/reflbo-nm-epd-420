#include "meshcore_proto.h"
#include "meshcore_companion.h"
#include "unity.h"
#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

void test_ed25519_keypair_sign_verify_tamper(void)
{
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(i * 7 + 0x3A);
    }

    uint8_t pub[32], prv[64];
    meshcore_ed25519_create_keypair(pub, prv, seed);

    /* Verify public key is not all zeros */
    uint8_t zero32[32] = {0};
    TEST_ASSERT_FALSE(memcmp(pub, zero32, 32) == 0);

    const char *msg = "MeshCore Ed25519 Digital Signature Verification";
    size_t msg_len = strlen(msg);

    uint8_t sig[64];
    meshcore_ed25519_sign(sig, (const uint8_t *)msg, msg_len, pub, prv);

    /* Verify signature is valid */
    TEST_ASSERT_TRUE(meshcore_ed25519_verify(sig, (const uint8_t *)msg, msg_len, pub));

    /* Tamper message */
    uint8_t bad_msg[64];
    memcpy(bad_msg, msg, msg_len);
    bad_msg[0] ^= 0x01;
    TEST_ASSERT_FALSE(meshcore_ed25519_verify(sig, bad_msg, msg_len, pub));

    /* Tamper signature */
    uint8_t bad_sig[64];
    memcpy(bad_sig, sig, 64);
    bad_sig[10] ^= 0x40;
    TEST_ASSERT_FALSE(meshcore_ed25519_verify(bad_sig, (const uint8_t *)msg, msg_len, pub));

    /* Tamper public key */
    uint8_t bad_pub[32];
    memcpy(bad_pub, pub, 32);
    bad_pub[5] ^= 0x80;
    TEST_ASSERT_FALSE(meshcore_ed25519_verify(sig, (const uint8_t *)msg, msg_len, bad_pub));
}

void test_aes128_ecb_standard_vectors(void)
{
    /* NIST SP 800-38A / FIPS-197 AES-128 test vector */
    static const uint8_t key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    static const uint8_t plain[16] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a
    };
    static const uint8_t expected_cipher[16] = {
        0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
        0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97
    };

    uint8_t cipher[16];
    meshcore_aes128_ecb_encrypt(key, plain, cipher);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_cipher, cipher, 16);

    uint8_t decrypted[16];
    meshcore_aes128_ecb_decrypt(key, cipher, decrypted);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(plain, decrypted, 16);
}

void test_encrypt_then_mac_roundtrip_and_tamper(void)
{
    uint8_t secret32[32] = {0};
    uint8_t ch_hash = 0;
    TEST_ASSERT_TRUE(meshcore_channel_key_from_psk(MESHCORE_DEFAULT_PUBLIC_PSK, secret32, &ch_hash));

    const char *test_str = "MeshCore Encrypt-Then-MAC Roundtrip Test Payload!";
    size_t test_len = strlen(test_str) + 1;

    uint8_t cipher[128];
    int enc_len = meshcore_encrypt_then_mac(secret32, cipher, sizeof(cipher), (const uint8_t *)test_str, test_len);
    TEST_ASSERT_TRUE(enc_len > 0);
    /* enc_len = 2 (MAC) + aligned 16 */
    TEST_ASSERT_EQUAL_INT(2 + ((test_len + 15) / 16) * 16, enc_len);

    uint8_t dec[128];
    int dec_len = meshcore_mac_then_decrypt(secret32, dec, sizeof(dec), cipher, (size_t)enc_len);
    TEST_ASSERT_TRUE(dec_len > 0);
    TEST_ASSERT_EQUAL_STRING(test_str, (char *)dec);

    /* Tamper each MAC byte */
    cipher[0] ^= 0x55;
    TEST_ASSERT_EQUAL_INT(0, meshcore_mac_then_decrypt(secret32, dec, sizeof(dec), cipher, (size_t)enc_len));
    cipher[0] ^= 0x55; /* restore */

    cipher[1] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(0, meshcore_mac_then_decrypt(secret32, dec, sizeof(dec), cipher, (size_t)enc_len));
    cipher[1] ^= 0x01; /* restore */

    /* Tamper ciphertext byte */
    cipher[5] ^= 0xAA;
    TEST_ASSERT_EQUAL_INT(0, meshcore_mac_then_decrypt(secret32, dec, sizeof(dec), cipher, (size_t)enc_len));
    cipher[5] ^= 0xAA; /* restore */

    /* Wrong key */
    uint8_t wrong_secret[32];
    memcpy(wrong_secret, secret32, 32);
    wrong_secret[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(0, meshcore_mac_then_decrypt(wrong_secret, dec, sizeof(dec), cipher, (size_t)enc_len));
}

void test_meshcore_packet_encode_decode(void)
{
    meshcore_packet_t in_pkt = {
        .header = (MESHCORE_PAYLOAD_REQ << MESHCORE_PH_TYPE_SHIFT) | MESHCORE_ROUTE_FLOOD,
        .transport_codes = {0, 0},
        .path_len = (1 << 6) | 2, /* 2-byte hashes, 2 hops */
        .path = {0x11, 0x22, 0x33, 0x44},
        .payload = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE},
        .payload_len = 5
    };

    uint8_t raw[128];
    size_t raw_len = meshcore_packet_encode(&in_pkt, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(1 + 1 + 4 + 5, (int)raw_len);

    meshcore_packet_t out_pkt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&out_pkt, raw, raw_len));
    TEST_ASSERT_EQUAL_HEX8(in_pkt.header, out_pkt.header);
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_FLOOD, meshcore_packet_get_route_type(&out_pkt));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_REQ, meshcore_packet_get_payload_type(&out_pkt));
    TEST_ASSERT_EQUAL_UINT8(2, meshcore_packet_get_hash_size(&out_pkt));
    TEST_ASSERT_EQUAL_UINT8(2, meshcore_packet_get_hop_count(&out_pkt));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in_pkt.path, out_pkt.path, 4);
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)out_pkt.payload_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in_pkt.payload, out_pkt.payload, 5);
}

void test_advert_build_and_parse_verified(void)
{
    uint8_t seed[32];
    memset(seed, 0xA5, sizeof(seed));
    uint8_t pub[32], prv[64];
    meshcore_ed25519_create_keypair(pub, prv, seed);

    uint8_t raw[256];
    size_t raw_len = meshcore_build_advert(raw, sizeof(raw), true, pub, prv,
                                           1700000000, "RockBase-420", MESHCORE_ADV_CHAT,
                                           true, 50061400, 19936600);
    TEST_ASSERT_TRUE(raw_len > 100);

    meshcore_packet_t pkt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&pkt, raw, raw_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_FLOOD, meshcore_packet_get_route_type(&pkt));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_ADVERT, meshcore_packet_get_payload_type(&pkt));

    meshcore_advert_t adv;
    TEST_ASSERT_TRUE(meshcore_parse_advert_payload(pkt.payload, pkt.payload_len, &adv));
    TEST_ASSERT_EQUAL_STRING("RockBase-420", adv.name);
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ADV_CHAT, adv.node_type);
    TEST_ASSERT_TRUE(adv.has_loc);
    TEST_ASSERT_EQUAL_INT32(50061400, adv.lat_e6);
    TEST_ASSERT_EQUAL_INT32(19936600, adv.lon_e6);
    TEST_ASSERT_EQUAL_UINT32(1700000000, adv.timestamp);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pub, adv.pub_key, 32);

    /* Tampering payload byte must fail signature verification */
    pkt.payload[102] ^= 0x01; /* tamper node name byte */
    TEST_ASSERT_FALSE(meshcore_parse_advert_payload(pkt.payload, pkt.payload_len, &adv));
}

void test_discover_req_resp_roundtrip(void)
{
    uint32_t tag = 0x87654321;
    uint8_t req_raw[32];
    size_t req_len = meshcore_build_discover_req(req_raw, sizeof(req_raw), tag);
    TEST_ASSERT_EQUAL_INT(8, (int)req_len);

    meshcore_packet_t req_pkt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&req_pkt, req_raw, req_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_DIRECT, meshcore_packet_get_route_type(&req_pkt));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_CONTROL, meshcore_packet_get_payload_type(&req_pkt));
    TEST_ASSERT_EQUAL_HEX8(0x81, req_pkt.payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, req_pkt.payload[1]);
    uint32_t req_tag;
    memcpy(&req_tag, &req_pkt.payload[2], 4);
    TEST_ASSERT_EQUAL_HEX32(tag, req_tag);

    /* Build discovery response */
    uint8_t pub_prefix[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    uint8_t resp_raw[32];
    size_t resp_len = meshcore_build_discover_resp(resp_raw, sizeof(resp_raw), tag,
                                                  MESHCORE_ADV_REPEATER, 24 /* +6 dB */, pub_prefix);
    TEST_ASSERT_EQUAL_INT(16, (int)resp_len);

    meshcore_packet_t resp_pkt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&resp_pkt, resp_raw, resp_len));
    meshcore_neighbor_t nbr;
    uint32_t out_tag = 0;
    TEST_ASSERT_TRUE(meshcore_parse_discover_resp(resp_pkt.payload, resp_pkt.payload_len, &nbr, &out_tag));
    TEST_ASSERT_EQUAL_HEX32(tag, out_tag);
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ADV_REPEATER, nbr.node_type);
    TEST_ASSERT_EQUAL_INT8(24, nbr.snr_in_q4);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pub_prefix, nbr.pub_key_prefix, 8);
}

void test_group_text_public_channel_polish_utf8(void)
{
    const char *sender = "Kraków-Node";
    const char *polish_msg = "Zażółć gęślą jaźń / 1234567890 ĄĆĘŁŃÓŚŹŻ";
    uint32_t ts = 1705000000;

    uint8_t raw[256];
    size_t raw_len = meshcore_build_grp_txt(raw, sizeof(raw), MESHCORE_DEFAULT_PUBLIC_PSK,
                                            ts, sender, polish_msg);
    TEST_ASSERT_TRUE(raw_len > 0);

    meshcore_packet_t pkt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&pkt, raw, raw_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_FLOOD, meshcore_packet_get_route_type(&pkt));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_GRP_TXT, meshcore_packet_get_payload_type(&pkt));

    uint32_t out_ts = 0;
    char out_text[160] = {0};
    TEST_ASSERT_TRUE(meshcore_parse_grp_txt_payload(pkt.payload, pkt.payload_len,
                                                   MESHCORE_DEFAULT_PUBLIC_PSK,
                                                   &out_ts, out_text, sizeof(out_text)));
    TEST_ASSERT_EQUAL_UINT32(ts, out_ts);

    char expected[160];
    snprintf(expected, sizeof(expected), "%s: %s", sender, polish_msg);
    TEST_ASSERT_EQUAL_STRING(expected, out_text);

    /* Test with wrong PSK fails MAC */
    const char *wrong_psk = "AAAAAAAAAAAAAAAAAAAAAA==";
    TEST_ASSERT_FALSE(meshcore_parse_grp_txt_payload(pkt.payload, pkt.payload_len,
                                                    wrong_psk,
                                                    &out_ts, out_text, sizeof(out_text)));

    /* Test with tampered ciphertext fails MAC */
    pkt.payload[5] ^= 0x01;
    TEST_ASSERT_FALSE(meshcore_parse_grp_txt_payload(pkt.payload, pkt.payload_len,
                                                    MESHCORE_DEFAULT_PUBLIC_PSK,
                                                    &out_ts, out_text, sizeof(out_text)));
}

void test_group_text_custom_channel_secret_roundtrip(void)
{
    const uint8_t secret[16] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
        0x0f, 0xed, 0xcb, 0xa9, 0x87, 0x65, 0x43, 0x21
    };
    const char *sender = "SecondaryNode";
    const char *msg = "Custom channel secret payload test";
    uint32_t ts = 1705001234;

    uint8_t raw[256];
    size_t raw_len = meshcore_build_grp_txt_secret(raw, sizeof(raw), secret, ts, sender, msg);
    TEST_ASSERT_TRUE(raw_len > 0);

    meshcore_packet_t pkt;
    TEST_ASSERT_TRUE(meshcore_packet_decode(&pkt, raw, raw_len));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_ROUTE_FLOOD, meshcore_packet_get_route_type(&pkt));
    TEST_ASSERT_EQUAL_UINT8(MESHCORE_PAYLOAD_GRP_TXT, meshcore_packet_get_payload_type(&pkt));

    uint32_t out_ts = 0;
    char out_text[160] = {0};
    TEST_ASSERT_TRUE(meshcore_parse_grp_txt_payload_secret(pkt.payload, pkt.payload_len,
                                                           secret, &out_ts, out_text, sizeof(out_text)));
    TEST_ASSERT_EQUAL_UINT32(ts, out_ts);

    char expected[160];
    snprintf(expected, sizeof(expected), "%s: %s", sender, msg);
    TEST_ASSERT_EQUAL_STRING(expected, out_text);

    /* Verify wrong secret fails parsing */
    uint8_t wrong_secret[16];
    memcpy(wrong_secret, secret, 16);
    wrong_secret[0] ^= 0x01;
    char fail_text[160] = {0};
    TEST_ASSERT_FALSE(meshcore_parse_grp_txt_payload_secret(pkt.payload, pkt.payload_len,
                                                            wrong_secret, &out_ts, fail_text, sizeof(fail_text)));
}

void test_lora_airtime_calculation(void)
{
    /* Semtech SX1262 LoRa airtime test */
    /* 62.5 kHz, SF8, CR 4/5 (cr=5), preamble 32, payload 100 bytes */
    uint32_t air_sf8 = meshcore_calc_airtime_ms(62500, 8, 5, 32, 100);
    TEST_ASSERT_TRUE(air_sf8 > 300);
    TEST_ASSERT_TRUE(air_sf8 < 3000);

    /* SF10 should have significantly higher airtime than SF8 */
    uint32_t air_sf10 = meshcore_calc_airtime_ms(62500, 10, 5, 16, 100);
    TEST_ASSERT_TRUE(air_sf10 > air_sf8);

    /* Higher bandwidth (250 kHz) should have lower airtime than 62.5 kHz */
    uint32_t air_250k = meshcore_calc_airtime_ms(250000, 8, 5, 32, 100);
    TEST_ASSERT_TRUE(air_250k < air_sf8);

    /* Invalid parameters return 0 */
    TEST_ASSERT_EQUAL_UINT32(0, meshcore_calc_airtime_ms(0, 8, 5, 32, 100));
    TEST_ASSERT_EQUAL_UINT32(0, meshcore_calc_airtime_ms(62500, 4, 5, 32, 100));
    TEST_ASSERT_EQUAL_UINT32(0, meshcore_calc_airtime_ms(62500, 13, 5, 32, 100));
}

static uint32_t s_mock_time = 1700000000;
static uint32_t mock_get_time(void) { return s_mock_time; }
static void mock_set_time(uint32_t t) { s_mock_time = t; }
static void mock_exec_cli(const char *cmd, char *reply, size_t reply_sz) {
    snprintf(reply, reply_sz, "MOCK:%s", cmd);
}

static char s_last_sent_channel_txt[128] = {0};
static uint8_t s_last_sent_channel_idx = 0;
static bool mock_send_channel_txt(uint8_t channel_idx, const char *text) {
    s_last_sent_channel_idx = channel_idx;
    strncpy(s_last_sent_channel_txt, text, sizeof(s_last_sent_channel_txt) - 1);
    s_last_sent_channel_txt[sizeof(s_last_sent_channel_txt) - 1] = '\0';
    return true;
}

static int s_send_direct_txt_calls = 0;
static uint8_t s_last_direct_pub_key[32] = {0};
static uint8_t s_last_direct_txt_type = 0;
static uint32_t s_last_direct_msg_ts = 0;
static char s_last_direct_txt[128] = {0};
static const uint8_t s_mock_ack_tag[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
static bool mock_send_direct_txt(const uint8_t pub_key[32], uint8_t txt_type, uint32_t msg_ts,
                                 const char *text, uint8_t out_ack_tag[4]) {
    s_send_direct_txt_calls++;
    memcpy(s_last_direct_pub_key, pub_key, 32);
    s_last_direct_txt_type = txt_type;
    s_last_direct_msg_ts = msg_ts;
    strncpy(s_last_direct_txt, text, sizeof(s_last_direct_txt) - 1);
    s_last_direct_txt[sizeof(s_last_direct_txt) - 1] = '\0';
    if (out_ack_tag) {
        memcpy(out_ack_tag, s_mock_ack_tag, 4);
    }
    return true;
}

#define MOCK_FRAMES_MAX 8
static uint8_t s_cb_frames[MOCK_FRAMES_MAX][COMPANION_CONTACT_FRAME_SIZE];
static size_t s_cb_frame_lens[MOCK_FRAMES_MAX];
static size_t s_cb_frame_count = 0;
static void mock_send_frame_cb(const uint8_t *frame, size_t len, void *user_data) {
    (void)user_data;
    if (s_cb_frame_count < MOCK_FRAMES_MAX) {
        size_t cpy = len > COMPANION_CONTACT_FRAME_SIZE ? COMPANION_CONTACT_FRAME_SIZE : len;
        memcpy(s_cb_frames[s_cb_frame_count], frame, cpy);
        s_cb_frame_lens[s_cb_frame_count] = len;
        s_cb_frame_count++;
    }
}

void test_companion_app_start(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);

    lora_config_t cfg = {
        .enabled = true,
        .freq_khz = 869618,
        .bw_hz = 62500,
        .bw_khz = 62,
        .sf = 8,
        .cr = 5,
        .tx_power_dbm = 22,
        .sync_word = 0x1424,
        .node_name = "CompanionNode"
    };

    companion_env_t env = {
        .lat_e6 = 50061400,
        .lon_e6 = 19936600,
        .batt_mv = 4120,
        .storage_used_kb = 128,
        .storage_total_kb = 2048,
        .get_time_s = mock_get_time,
        .set_time_s = mock_set_time,
    };
    for (int i = 0; i < 32; i++) env.pub_key[i] = (uint8_t)(i + 1);

    uint8_t cmd[] = { CMD_APP_START };
    uint8_t resp[128];
    bool changed = false;
    size_t resp_len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, sizeof(cmd), resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_len >= 58 + strlen("CompanionNode"));
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SELF_INFO, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(1, resp[1]); /* ADV_TYPE_CHAT */
    TEST_ASSERT_EQUAL_UINT8(22, resp[2]); /* tx_power_dbm */
    TEST_ASSERT_EQUAL_UINT8(22, resp[3]); /* MAX_LORA_TX_POWER */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(env.pub_key, &resp[4], 32);

    int32_t lat, lon;
    memcpy(&lat, &resp[36], 4);
    memcpy(&lon, &resp[40], 4);
    TEST_ASSERT_EQUAL_INT32(50061400, lat);
    TEST_ASSERT_EQUAL_INT32(19936600, lon);

    uint32_t freq, bw;
    memcpy(&freq, &resp[48], 4);
    memcpy(&bw, &resp[52], 4);
    TEST_ASSERT_EQUAL_UINT32(869618, freq);
    TEST_ASSERT_EQUAL_UINT32(62500, bw);
    TEST_ASSERT_EQUAL_UINT8(8, resp[56]); /* sf */
    TEST_ASSERT_EQUAL_UINT8(5, resp[57]); /* cr */
    TEST_ASSERT_EQUAL_STRING_LEN("CompanionNode", (char *)&resp[58], strlen("CompanionNode"));
}

void test_companion_device_query(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);

    lora_config_t cfg = {
        .ble_pin = 482915,
        .client_repeat = true,
        .path_hash_mode = 1
    };
    companion_env_t env = {0};

    uint8_t cmd[] = { CMD_DEVICE_QUERY };
    uint8_t resp[128];
    bool changed = false;
    size_t resp_len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, sizeof(cmd), resp, sizeof(resp));

    TEST_ASSERT_EQUAL_INT(82, (int)resp_len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_DEVICE_INFO, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(10, resp[1]); /* COMPANION_FIRMWARE_VER_CODE */
    TEST_ASSERT_EQUAL_UINT8(32, resp[2]); /* max_contacts / 2 = 64 / 2 */
    TEST_ASSERT_EQUAL_UINT8(8, resp[3]);  /* max_channels */

    uint32_t pin;
    memcpy(&pin, &resp[4], 4);
    TEST_ASSERT_EQUAL_UINT32(482915, pin);

    TEST_ASSERT_TRUE(strlen((char *)&resp[8]) > 0);
    TEST_ASSERT_EQUAL_STRING("RockBase NM-EPD-420", (char *)&resp[20]);
    TEST_ASSERT_EQUAL_STRING("v1.18.0-reflbo-1.1", (char *)&resp[60]);
    TEST_ASSERT_EQUAL_UINT8(1, resp[80]); /* client_repeat */
    TEST_ASSERT_EQUAL_UINT8(1, resp[81]); /* path_hash_mode */
}

void test_companion_channel_management(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    companion_env_t env = {0};
    uint8_t resp[128];
    bool changed = false;

    /* Get slot 0 (Public) */
    uint8_t get_ch0[] = { CMD_GET_CHANNEL, 0 };
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_ch0, sizeof(get_ch0), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(50, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CHANNEL_INFO, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(0, resp[1]);
    TEST_ASSERT_EQUAL_STRING("Public", (char *)&resp[2]);

    /* Set slot 1 (Custom channel) */
    uint8_t set_ch1[2 + 32 + 16] = {0};
    set_ch1[0] = CMD_SET_CHANNEL;
    set_ch1[1] = 1;
    strncpy((char *)&set_ch1[2], "EmergencyNet", 31);
    for (int i = 0; i < 16; i++) set_ch1[34 + i] = (uint8_t)(0xAA + i);

    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_ch1, sizeof(set_ch1), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);

    /* Get slot 1 */
    uint8_t get_ch1[] = { CMD_GET_CHANNEL, 1 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_ch1, sizeof(get_ch1), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(50, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CHANNEL_INFO, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(1, resp[1]);
    TEST_ASSERT_EQUAL_STRING("EmergencyNet", (char *)&resp[2]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&set_ch1[34], &resp[34], 16);

    /* Get invalid slot 8 */
    uint8_t get_ch8[] = { CMD_GET_CHANNEL, 8 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_ch8, sizeof(get_ch8), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
}

void test_companion_radio_params_and_tx_power(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = { .freq_khz = 868000, .bw_hz = 250000, .sf = 7, .cr = 5, .tx_power_dbm = 22 };
    companion_env_t env = {0};
    uint8_t resp[64];
    bool changed = false;

    /* SET_RADIO_PARAMS: freq=869400, bw=62500, sf=8, cr=6, client_repeat=1 */
    uint8_t set_radio[12];
    set_radio[0] = CMD_SET_RADIO_PARAMS;
    uint32_t f = 869400, bw = 62500;
    memcpy(&set_radio[1], &f, 4);
    memcpy(&set_radio[5], &bw, 4);
    set_radio[9] = 8;
    set_radio[10] = 6;
    set_radio[11] = 1;

    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_radio, sizeof(set_radio), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(869400, cfg.freq_khz);
    TEST_ASSERT_EQUAL_UINT32(62500, cfg.bw_hz);
    TEST_ASSERT_EQUAL_UINT16(62, cfg.bw_khz);
    TEST_ASSERT_EQUAL_UINT8(8, cfg.sf);
    TEST_ASSERT_EQUAL_UINT8(6, cfg.cr);
    TEST_ASSERT_TRUE(cfg.client_repeat);

    /* SET_RADIO_TX_POWER: 14 dBm */
    changed = false;
    uint8_t set_pwr[] = { CMD_SET_RADIO_TX_POWER, 14 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_pwr, sizeof(set_pwr), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT8(14, cfg.tx_power_dbm);

    /* Invalid power: 30 dBm */
    uint8_t bad_pwr[] = { CMD_SET_RADIO_TX_POWER, 30 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, bad_pwr, sizeof(bad_pwr), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
}

void test_companion_device_pin(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = { .ble_pin = 0 };
    companion_env_t env = {0};
    uint8_t resp[64];
    bool changed = false;

    /* Valid PIN 482915 */
    uint8_t set_pin[5] = { CMD_SET_DEVICE_PIN };
    uint32_t pin = 482915;
    memcpy(&set_pin[1], &pin, 4);

    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_pin, sizeof(set_pin), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT32(482915, cfg.ble_pin);

    /* Invalid PIN 12345 */
    changed = false;
    pin = 12345;
    memcpy(&set_pin[1], &pin, 4);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_pin, sizeof(set_pin), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(482915, cfg.ble_pin);
}

void test_companion_contacts_crud(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    companion_env_t env = {0};
    uint8_t resp[256];
    bool changed = false;

    uint8_t add_cmd[1 + 32 + 1 + 1 + 1 + 64 + 32 + 4 + 8 + 4] = {0};
    add_cmd[0] = CMD_ADD_UPDATE_CONTACT;
    uint8_t pub[32];
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0xC0 + i);
    memcpy(&add_cmd[1], pub, 32);
    add_cmd[33] = 1; /* type */
    add_cmd[34] = 0; /* flags */
    add_cmd[35] = 2; /* out_path_len */
    add_cmd[36] = 0x11; add_cmd[37] = 0x22; /* path */
    strncpy((char *)&add_cmd[100], "Alice-Node", 31);
    uint32_t ts = 1710000000;
    memcpy(&add_cmd[132], &ts, 4);
    int32_t lat = 50000000, lon = 20000000;
    memcpy(&add_cmd[136], &lat, 4);
    memcpy(&add_cmd[140], &lon, 4);
    uint32_t lastmod = 1710000005;
    memcpy(&add_cmd[144], &lastmod, 4);

    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, add_cmd, sizeof(add_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)ctx.contact_count);

    /* Query contact by key */
    uint8_t get_cmd[1 + 32];
    get_cmd[0] = CMD_GET_CONTACT_BY_KEY;
    memcpy(&get_cmd[1], pub, 32);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_cmd, sizeof(get_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(COMPANION_CONTACT_FRAME_SIZE, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CONTACT, resp[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pub, &resp[1], 32);
    TEST_ASSERT_EQUAL_STRING("Alice-Node", (char *)&resp[1 + 32 + 1 + 1 + 1 + 64]);

    /* Remove contact */
    uint8_t del_cmd[1 + 32];
    del_cmd[0] = CMD_REMOVE_CONTACT;
    memcpy(&del_cmd[1], pub, 32);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, del_cmd, sizeof(del_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)ctx.contact_count);

    /* Query contact by key again -> not found */
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_cmd, sizeof(get_cmd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
}

void test_companion_get_stats(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    lora_stats_t stats = {
        .noise_floor_dbm = -118,
        .last_rssi_dbm = -75,
        .last_snr_q4 = 28, /* 7 dB */
        .packet_count = 100,
        .packets_sent = 50,
        .sent_flood = 30,
        .sent_direct = 20,
        .recv_flood = 60,
        .recv_direct = 40,
        .recv_errors = 5,
        .tx_air_ms = 4000,
        .rx_air_ms = 8000
    };
    companion_env_t env = {
        .batt_mv = 3950,
        .get_time_s = mock_get_time,
        .stats = &stats
    };
    uint8_t resp[64];
    bool changed = false;

    /* Core stats (type 0) */
    uint8_t cmd_core[] = { CMD_GET_STATS, STATS_TYPE_CORE };
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_core, sizeof(cmd_core), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(11, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_STATS, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(STATS_TYPE_CORE, resp[1]);
    uint16_t mv;
    memcpy(&mv, &resp[2], 2);
    TEST_ASSERT_EQUAL_UINT16(3950, mv);

    /* Radio stats (type 1) */
    uint8_t cmd_radio[] = { CMD_GET_STATS, STATS_TYPE_RADIO };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_radio, sizeof(cmd_radio), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(14, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_STATS, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(STATS_TYPE_RADIO, resp[1]);
    int16_t nf;
    memcpy(&nf, &resp[2], 2);
    TEST_ASSERT_EQUAL_INT16(-118, nf);
    TEST_ASSERT_EQUAL_INT8(-75, (int8_t)resp[4]);
    TEST_ASSERT_EQUAL_INT8(28, (int8_t)resp[5]);

    /* Packet stats (type 2) */
    uint8_t cmd_pkt[] = { CMD_GET_STATS, STATS_TYPE_PACKETS };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_pkt, sizeof(cmd_pkt), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(30, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_STATS, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(STATS_TYPE_PACKETS, resp[1]);
    uint32_t total, sent;
    memcpy(&total, &resp[2], 4);
    memcpy(&sent, &resp[6], 4);
    TEST_ASSERT_EQUAL_UINT32(100, total);
    TEST_ASSERT_EQUAL_UINT32(50, sent);
}

void test_companion_time_and_cli(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    companion_env_t env = {
        .get_time_s = mock_get_time,
        .set_time_s = mock_set_time,
        .exec_cli = mock_exec_cli
    };
    uint8_t resp[128];
    bool changed = false;

    /* GET_DEVICE_TIME */
    s_mock_time = 1720000000;
    uint8_t get_time[] = { CMD_GET_DEVICE_TIME };
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_time, sizeof(get_time), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(5, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CURR_TIME, resp[0]);
    uint32_t t;
    memcpy(&t, &resp[1], 4);
    TEST_ASSERT_EQUAL_UINT32(1720000000, t);

    /* SET_DEVICE_TIME */
    uint8_t set_time[5] = { CMD_SET_DEVICE_TIME };
    uint32_t new_t = 1730000000;
    memcpy(&set_time[1], &new_t, 4);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_time, sizeof(set_time), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT32(1730000000, s_mock_time);

    /* RUN_CLI_COMMAND */
    const char *cli_cmd = "help";
    uint8_t run_cli[1 + 4];
    run_cli[0] = CMD_RUN_CLI_COMMAND;
    memcpy(&run_cli[1], cli_cmd, 4);
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, run_cli, sizeof(run_cli), resp, sizeof(resp));
    TEST_ASSERT_TRUE(len > 1);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CLI_REPLY, resp[0]);
    TEST_ASSERT_EQUAL_STRING_LEN("MOCK:help", (char *)&resp[1], len - 1);
}

void test_companion_send_txt_msgs(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    companion_env_t env = {
        .send_channel_txt = mock_send_channel_txt,
        .send_direct_txt = mock_send_direct_txt
    };
    uint8_t resp[128];
    bool changed = false;

    /* CMD_SEND_CHANNEL_TXT_MSG */
    s_last_sent_channel_txt[0] = '\0';
    s_last_sent_channel_idx = 0xFF;
    const char *ch_text = "Hello Channel 2!";
    uint8_t ch_cmd[7 + 32] = {0};
    ch_cmd[0] = CMD_SEND_CHANNEL_TXT_MSG;
    ch_cmd[1] = 0; /* plain text */
    ch_cmd[2] = 2; /* channel index 2 */
    uint32_t ts = 1715000000;
    memcpy(&ch_cmd[3], &ts, 4);
    memcpy(&ch_cmd[7], ch_text, strlen(ch_text));

    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, ch_cmd, 7 + strlen(ch_text), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(2, s_last_sent_channel_idx);
    TEST_ASSERT_EQUAL_STRING("Hello Channel 2!", s_last_sent_channel_txt);

    /* CMD_SEND_TXT_MSG: unknown recipient prefix must fail with COMPANION_ERR_NOT_FOUND */
    s_send_direct_txt_calls = 0;
    s_last_direct_txt[0] = '\0';
    memset(s_last_direct_pub_key, 0, sizeof(s_last_direct_pub_key));
    const char *dm_text = "Direct message!";
    uint8_t dm_cmd[13 + 32] = {0};
    dm_cmd[0] = CMD_SEND_TXT_MSG;
    dm_cmd[1] = 0; /* plain text */
    dm_cmd[2] = 0; /* attempt */
    uint32_t dm_ts = 1715000123;
    memcpy(&dm_cmd[3], &dm_ts, 4);
    for (int i = 0; i < 6; i++) dm_cmd[7 + i] = (uint8_t)(0x10 + i);
    memcpy(&dm_cmd[13], dm_text, strlen(dm_text));

    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, dm_cmd, 13 + strlen(dm_text), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(2, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(COMPANION_ERR_NOT_FOUND, resp[1]);
    TEST_ASSERT_EQUAL_INT(0, s_send_direct_txt_calls);

    /* Register matching contact (flood path out_path_len = -1) and resend */
    companion_contact_t peer = { .out_path_len = -1 };
    for (int i = 0; i < 32; i++) peer.pub_key[i] = (uint8_t)(0x10 + i);
    strncpy(peer.name, "DirectPeer", sizeof(peer.name) - 1);
    TEST_ASSERT_TRUE(meshcore_companion_add_update_contact(&ctx, &peer));

    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, dm_cmd, 13 + strlen(dm_text), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(10, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SENT, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(1, resp[1]); /* flood */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_mock_ack_tag, &resp[2], 4);
    uint32_t ret_timeout;
    memcpy(&ret_timeout, &resp[6], 4);
    TEST_ASSERT_EQUAL_UINT32(5000, ret_timeout);
    TEST_ASSERT_EQUAL_INT(1, s_send_direct_txt_calls);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(peer.pub_key, s_last_direct_pub_key, 32);
    TEST_ASSERT_EQUAL_UINT8(0, s_last_direct_txt_type);
    TEST_ASSERT_EQUAL_UINT32(dm_ts, s_last_direct_msg_ts);
    TEST_ASSERT_EQUAL_STRING("Direct message!", s_last_direct_txt);
}

void test_companion_get_contacts_streaming_and_since(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};

    /* Add Contact 1 (lastmod = 100) */
    companion_contact_t c1 = { .lastmod = 100 };
    memset(c1.pub_key, 0x11, 32);
    strncpy(c1.name, "NodeA", sizeof(c1.name) - 1);
    TEST_ASSERT_TRUE(meshcore_companion_add_update_contact(&ctx, &c1));

    /* Add Contact 2 (lastmod = 200) */
    companion_contact_t c2 = { .lastmod = 200 };
    memset(c2.pub_key, 0x22, 32);
    strncpy(c2.name, "NodeB", sizeof(c2.name) - 1);
    TEST_ASSERT_TRUE(meshcore_companion_add_update_contact(&ctx, &c2));

    uint8_t resp[256];
    bool changed = false;

    /* 1. Test buffer mode (send_frame_cb == NULL) with since = 150 */
    companion_env_t env_buf = {0};
    uint8_t cmd_since[5] = { CMD_GET_CONTACTS };
    uint32_t since = 150;
    memcpy(&cmd_since[1], &since, 4);

    size_t len = meshcore_companion_handle_cmd(&ctx, &env_buf, &cfg, &changed, cmd_since, sizeof(cmd_since), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(5, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CONTACTS_START, resp[0]);
    uint32_t match_count;
    memcpy(&match_count, &resp[1], 4);
    TEST_ASSERT_EQUAL_UINT32(2, match_count); /* total contacts count as per MyMesh.cpp:1274 */

    /* 2. Test streaming mode (send_frame_cb != NULL) with ContactsIterator */
    companion_env_t env_stream = {
        .send_frame_cb = mock_send_frame_cb
    };
    s_cb_frame_count = 0;
    uint8_t cmd_all[1] = { CMD_GET_CONTACTS };
    len = meshcore_companion_handle_cmd(&ctx, &env_stream, &cfg, &changed, cmd_all, sizeof(cmd_all), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(0, (int)len); /* Return 0 so caller doesn't send resp_out duplicate */
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_cb_frame_count); /* Initial frame: START */
    TEST_ASSERT_TRUE(ctx.contact_iter_active);

    /* Step the iterator until complete */
    while (meshcore_companion_step_iter(&ctx, &env_stream)) {
        /* each step transmits 1 contact */
    }
    TEST_ASSERT_FALSE(ctx.contact_iter_active);
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)s_cb_frame_count); /* START + Contact1 + Contact2 + END */

    /* Frame 0: START (5 bytes) */
    TEST_ASSERT_EQUAL_INT(5, (int)s_cb_frame_lens[0]);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CONTACTS_START, s_cb_frames[0][0]);
    memcpy(&match_count, &s_cb_frames[0][1], 4);
    TEST_ASSERT_EQUAL_UINT32(2, match_count);

    /* Frame 1: CONTACT NodeA (148 bytes) */
    TEST_ASSERT_EQUAL_INT(COMPANION_CONTACT_FRAME_SIZE, (int)s_cb_frame_lens[1]);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CONTACT, s_cb_frames[1][0]);

    /* Frame 2: CONTACT NodeB (148 bytes) */
    TEST_ASSERT_EQUAL_INT(COMPANION_CONTACT_FRAME_SIZE, (int)s_cb_frame_lens[2]);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_CONTACT, s_cb_frames[2][0]);

    /* Frame 3: END_OF_CONTACTS (5 bytes) with most_recent_lastmod = 200 */
    TEST_ASSERT_EQUAL_INT(5, (int)s_cb_frame_lens[3]);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_END_OF_CONTACTS, s_cb_frames[3][0]);
    uint32_t end_lastmod;
    memcpy(&end_lastmod, &s_cb_frames[3][1], 4);
    TEST_ASSERT_EQUAL_UINT32(200, end_lastmod);

    /* 3. Test streaming mode with since = 150 */
    s_cb_frame_count = 0;
    len = meshcore_companion_handle_cmd(&ctx, &env_stream, &cfg, &changed, cmd_since, sizeof(cmd_since), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(0, (int)len);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_cb_frame_count); /* Start frame */
    TEST_ASSERT_TRUE(ctx.contact_iter_active);

    while (meshcore_companion_step_iter(&ctx, &env_stream)) {
        /* step transmits filtered contact */
    }
    TEST_ASSERT_FALSE(ctx.contact_iter_active);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)s_cb_frame_count); /* START + NodeB + END */

    /* Frame 0: START with total count = 2 */
    TEST_ASSERT_EQUAL_INT(5, (int)s_cb_frame_lens[0]);
    memcpy(&match_count, &s_cb_frames[0][1], 4);
    TEST_ASSERT_EQUAL_UINT32(2, match_count);

    /* Frame 1: CONTACT NodeB */
    TEST_ASSERT_EQUAL_INT(COMPANION_CONTACT_FRAME_SIZE, (int)s_cb_frame_lens[1]);

    /* Frame 2: END_OF_CONTACTS (5 bytes) with most_recent_lastmod = 200 */
    TEST_ASSERT_EQUAL_INT(5, (int)s_cb_frame_lens[2]);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_END_OF_CONTACTS, s_cb_frames[2][0]);
    memcpy(&end_lastmod, &s_cb_frames[2][1], 4);
    TEST_ASSERT_EQUAL_UINT32(200, end_lastmod);
}

void test_companion_v10_startup_commands(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = {0};
    companion_env_t env = {0};
    uint8_t resp[64];
    bool changed = false;

    /* CMD_GET_AUTOADD_CONFIG (59) */
    uint8_t get_autoadd[] = { CMD_GET_AUTOADD_CONFIG };
    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_autoadd, sizeof(get_autoadd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(3, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_AUTOADD_CONFIG, resp[0]);
    TEST_ASSERT_EQUAL_UINT8(0x1E, resp[1]);
    TEST_ASSERT_EQUAL_UINT8(64, resp[2]);

    /* CMD_SET_AUTOADD_CONFIG (58) */
    uint8_t set_autoadd[] = { CMD_SET_AUTOADD_CONFIG, 0x1E, 64 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_autoadd, sizeof(set_autoadd), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);

    /* CMD_GET_DEFAULT_FLOOD_SCOPE (64) */
    uint8_t get_scope[] = { CMD_GET_DEFAULT_FLOOD_SCOPE };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_scope, sizeof(get_scope), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_DEFAULT_FLOOD_SCOPE, resp[0]);

    /* CMD_SET_DEFAULT_FLOOD_SCOPE (63) */
    uint8_t set_scope[] = { CMD_SET_DEFAULT_FLOOD_SCOPE, 1 };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, set_scope, sizeof(set_scope), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);

    /* CMD_GET_ALLOWED_REPEAT_FREQ (60) */
    uint8_t get_freq[] = { CMD_GET_ALLOWED_REPEAT_FREQ };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, get_freq, sizeof(get_freq), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(25, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_ALLOWED_REPEAT_FREQ, resp[0]);
    uint32_t f1, f2, f3, f4, f5, f6;
    memcpy(&f1, &resp[1], 4);
    memcpy(&f2, &resp[5], 4);
    memcpy(&f3, &resp[9], 4);
    memcpy(&f4, &resp[13], 4);
    memcpy(&f5, &resp[17], 4);
    memcpy(&f6, &resp[21], 4);
    TEST_ASSERT_EQUAL_UINT32(433000, f1);
    TEST_ASSERT_EQUAL_UINT32(433000, f2);
    TEST_ASSERT_EQUAL_UINT32(869495, f3);
    TEST_ASSERT_EQUAL_UINT32(869495, f4);
    TEST_ASSERT_EQUAL_UINT32(918000, f5);
    TEST_ASSERT_EQUAL_UINT32(918000, f6);
}

void test_packet_hash_ignores_hops_and_path(void)
{
    meshcore_packet_t pkt1;
    memset(&pkt1, 0, sizeof(pkt1));
    pkt1.header = (uint8_t)((MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_GRP_TXT & 0x0F) << 2));
    pkt1.path_len = 0; /* hops = 0 */
    pkt1.payload_len = 16;
    memcpy(pkt1.payload, "12345678abcdefgh", 16);

    meshcore_packet_t pkt2;
    memset(&pkt2, 0, sizeof(pkt2));
    pkt2.header = (uint8_t)((MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_GRP_TXT & 0x0F) << 2));
    pkt2.path_len = 2; /* hops = 2 */
    pkt2.path[0] = 0xAA;
    pkt2.path[1] = 0xBB;
    pkt2.payload_len = 16;
    memcpy(pkt2.payload, "12345678abcdefgh", 16);

    uint8_t hash1[8] = {0};
    uint8_t hash2[8] = {0};
    meshcore_packet_hash(&pkt1, hash1);
    meshcore_packet_hash(&pkt2, hash2);

    /* Verify non-zero and identical */
    uint8_t zero8[8] = {0};
    TEST_ASSERT_FALSE(memcmp(hash1, zero8, 8) == 0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hash1, hash2, 8);

    /* Verify modifying payload changes the hash */
    meshcore_packet_t pkt3 = pkt1;
    pkt3.payload[0] ^= 0x55;
    uint8_t hash3[8] = {0};
    meshcore_packet_hash(&pkt3, hash3);
    TEST_ASSERT_FALSE(memcmp(hash1, hash3, 8) == 0);

    /* Verify for TRACE packets, path_len is part of the hash */
    meshcore_packet_t tr1;
    memset(&tr1, 0, sizeof(tr1));
    tr1.header = (uint8_t)((MESHCORE_ROUTE_FLOOD & 0x03) | ((MESHCORE_PAYLOAD_TRACE & 0x0F) << 2));
    tr1.path_len = 1;
    tr1.payload_len = 8;
    memcpy(tr1.payload, "TRACEPAY", 8);

    meshcore_packet_t tr2 = tr1;
    tr2.path_len = 2;

    uint8_t h_tr1[8] = {0}, h_tr2[8] = {0};
    meshcore_packet_hash(&tr1, h_tr1);
    meshcore_packet_hash(&tr2, h_tr2);
    TEST_ASSERT_FALSE(memcmp(h_tr1, h_tr2, 8) == 0);
}

void test_companion_latlon_and_unsupported_cmds(void)
{
    companion_context_t ctx;
    meshcore_companion_init(&ctx);
    lora_config_t cfg = { .freq_khz = 869618, .bw_hz = 62500, .sf = 8, .cr = 5, .tx_power_dbm = 22, .node_name = "LatLonNode" };
    companion_env_t env = {
        .lat_e6 = 11111111,
        .lon_e6 = 22222222
    };
    uint8_t resp[128];
    bool changed = false;

    uint8_t cmd_latlon[9] = { CMD_SET_ADVERT_LATLON };
    int32_t new_lat = 52229700;
    int32_t new_lon = 21012200;
    memcpy(&cmd_latlon[1], &new_lat, 4);
    memcpy(&cmd_latlon[5], &new_lon, 4);

    size_t len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_latlon, sizeof(cmd_latlon), resp, sizeof(resp));
    TEST_ASSERT_EQUAL_INT(1, (int)len);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_OK, resp[0]);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_TRUE(cfg.share_loc);

    uint8_t cmd_start[] = { CMD_APP_START };
    len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd_start, sizeof(cmd_start), resp, sizeof(resp));
    TEST_ASSERT_TRUE(len >= 58);
    TEST_ASSERT_EQUAL_UINT8(RESP_CODE_SELF_INFO, resp[0]);
    int32_t out_lat = 0, out_lon = 0;
    memcpy(&out_lat, &resp[36], 4);
    memcpy(&out_lon, &resp[40], 4);
    TEST_ASSERT_EQUAL_INT32(new_lat, out_lat);
    TEST_ASSERT_EQUAL_INT32(new_lon, out_lon);

    const uint8_t unsupported[] = { CMD_SHARE_CONTACT, CMD_REBOOT, CMD_SEND_TRACE_PATH };
    for (size_t i = 0; i < sizeof(unsupported); i++) {
        uint8_t cmd[1] = { unsupported[i] };
        len = meshcore_companion_handle_cmd(&ctx, &env, &cfg, &changed, cmd, sizeof(cmd), resp, sizeof(resp));
        TEST_ASSERT_EQUAL_INT(2, (int)len);
        TEST_ASSERT_EQUAL_UINT8(RESP_CODE_ERR, resp[0]);
        TEST_ASSERT_EQUAL_UINT8(COMPANION_ERR_UNSUPPORTED_CMD, resp[1]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ed25519_keypair_sign_verify_tamper);
    RUN_TEST(test_aes128_ecb_standard_vectors);
    RUN_TEST(test_encrypt_then_mac_roundtrip_and_tamper);
    RUN_TEST(test_meshcore_packet_encode_decode);
    RUN_TEST(test_packet_hash_ignores_hops_and_path);
    RUN_TEST(test_advert_build_and_parse_verified);
    RUN_TEST(test_discover_req_resp_roundtrip);
    RUN_TEST(test_group_text_public_channel_polish_utf8);
    RUN_TEST(test_group_text_custom_channel_secret_roundtrip);
    RUN_TEST(test_lora_airtime_calculation);
    RUN_TEST(test_companion_app_start);
    RUN_TEST(test_companion_device_query);
    RUN_TEST(test_companion_channel_management);
    RUN_TEST(test_companion_radio_params_and_tx_power);
    RUN_TEST(test_companion_device_pin);
    RUN_TEST(test_companion_contacts_crud);
    RUN_TEST(test_companion_get_stats);
    RUN_TEST(test_companion_time_and_cli);
    RUN_TEST(test_companion_send_txt_msgs);
    RUN_TEST(test_companion_latlon_and_unsupported_cmds);
    RUN_TEST(test_companion_get_contacts_streaming_and_since);
    RUN_TEST(test_companion_v10_startup_commands);
    return UNITY_END();
}
