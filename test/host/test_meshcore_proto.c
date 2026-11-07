#include "meshcore_proto.h"
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

    /* Tamper MAC byte */
    cipher[0] ^= 0x55;
    TEST_ASSERT_EQUAL_INT(0, meshcore_mac_then_decrypt(secret32, dec, sizeof(dec), cipher, (size_t)enc_len));
    cipher[0] ^= 0x55; /* restore */

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ed25519_keypair_sign_verify_tamper);
    RUN_TEST(test_aes128_ecb_standard_vectors);
    RUN_TEST(test_encrypt_then_mac_roundtrip_and_tamper);
    RUN_TEST(test_meshcore_packet_encode_decode);
    RUN_TEST(test_advert_build_and_parse_verified);
    RUN_TEST(test_discover_req_resp_roundtrip);
    RUN_TEST(test_group_text_public_channel_polish_utf8);
    RUN_TEST(test_lora_airtime_calculation);
    return UNITY_END();
}
