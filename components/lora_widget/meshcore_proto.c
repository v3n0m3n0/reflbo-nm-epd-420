#include "meshcore_proto.h"
#include "util_base64.h"
#include "util_sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Packet Decoding & Encoding                                                */
/* ========================================================================= */

uint8_t meshcore_packet_get_route_type(const meshcore_packet_t *pkt)
{
    return pkt ? (pkt->header & MESHCORE_PH_ROUTE_MASK) : 0;
}

uint8_t meshcore_packet_get_payload_type(const meshcore_packet_t *pkt)
{
    return pkt ? ((pkt->header >> MESHCORE_PH_TYPE_SHIFT) & MESHCORE_PH_TYPE_MASK) : 0;
}

uint8_t meshcore_packet_get_hash_size(const meshcore_packet_t *pkt)
{
    return pkt ? ((pkt->path_len >> 6) + 1) : 1;
}

uint8_t meshcore_packet_get_hop_count(const meshcore_packet_t *pkt)
{
    return pkt ? (pkt->path_len & 63) : 0;
}

bool meshcore_packet_decode(meshcore_packet_t *pkt, const uint8_t *raw, size_t len)
{
    if (pkt == NULL || raw == NULL || len < 2) {
        return false;
    }
    memset(pkt, 0, sizeof(*pkt));

    size_t i = 0;
    pkt->header = raw[i++];

    uint8_t route = pkt->header & MESHCORE_PH_ROUTE_MASK;
    if (route == MESHCORE_ROUTE_TRANSPORT_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_DIRECT) {
        if (i + 4 > len) {
            return false;
        }
        memcpy(&pkt->transport_codes[0], &raw[i], 2); i += 2;
        memcpy(&pkt->transport_codes[1], &raw[i], 2); i += 2;
    } else {
        pkt->transport_codes[0] = 0;
        pkt->transport_codes[1] = 0;
    }

    if (i >= len) {
        return false;
    }
    pkt->path_len = raw[i++];

    uint8_t hash_size = (pkt->path_len >> 6) + 1;
    if (hash_size == 4) {
        return false; /* Reserved */
    }
    uint8_t hop_count = pkt->path_len & 63;
    size_t path_bytes = (size_t)hash_size * hop_count;

    if (path_bytes > sizeof(pkt->path) || (i + path_bytes) > len) {
        return false;
    }
    if (path_bytes > 0) {
        memcpy(pkt->path, &raw[i], path_bytes);
        i += path_bytes;
    }

    pkt->payload_len = len - i;
    if (pkt->payload_len > sizeof(pkt->payload)) {
        return false;
    }
    if (pkt->payload_len > 0) {
        memcpy(pkt->payload, &raw[i], pkt->payload_len);
    }

    return true;
}

size_t meshcore_packet_encode(const meshcore_packet_t *pkt, uint8_t *raw, size_t cap)
{
    if (pkt == NULL || raw == NULL) {
        return 0;
    }
    uint8_t route = pkt->header & MESHCORE_PH_ROUTE_MASK;
    bool has_tc = (route == MESHCORE_ROUTE_TRANSPORT_FLOOD || route == MESHCORE_ROUTE_TRANSPORT_DIRECT);

    uint8_t hash_size = (pkt->path_len >> 6) + 1;
    if (hash_size == 4) {
        return 0;
    }
    uint8_t hop_count = pkt->path_len & 63;
    size_t path_bytes = (size_t)hash_size * hop_count;

    size_t needed = 1 + (has_tc ? 4 : 0) + 1 + path_bytes + pkt->payload_len;
    if (needed > cap) {
        return 0;
    }

    size_t i = 0;
    raw[i++] = pkt->header;
    if (has_tc) {
        memcpy(&raw[i], &pkt->transport_codes[0], 2); i += 2;
        memcpy(&raw[i], &pkt->transport_codes[1], 2); i += 2;
    }
    raw[i++] = pkt->path_len;
    if (path_bytes > 0) {
        memcpy(&raw[i], pkt->path, path_bytes);
        i += path_bytes;
    }
    if (pkt->payload_len > 0) {
        memcpy(&raw[i], pkt->payload, pkt->payload_len);
        i += pkt->payload_len;
    }

    return i;
}

/* ========================================================================= */
/* Advertisement Encoding & Parsing                                          */
/* ========================================================================= */

size_t meshcore_build_advert(uint8_t *out_raw,
                             size_t cap,
                             bool flood,
                             const uint8_t pub[MESHCORE_PUB_KEY_SIZE],
                             const uint8_t prv[MESHCORE_PRV_KEY_SIZE],
                             uint32_t timestamp,
                             const char *name,
                             uint8_t node_type,
                             bool share_loc,
                             int32_t lat_e6,
                             int32_t lon_e6)
{
    if (out_raw == NULL || pub == NULL || prv == NULL) {
        return 0;
    }

    uint8_t route_type = flood ? MESHCORE_ROUTE_FLOOD : MESHCORE_ROUTE_DIRECT;
    uint8_t header = (MESHCORE_PAYLOAD_ADVERT << MESHCORE_PH_TYPE_SHIFT) | (route_type & MESHCORE_PH_ROUTE_MASK);

    uint8_t appdata[MESHCORE_MAX_ADVERT_DATA_SIZE] = {0};
    appdata[0] = (node_type & 0x0F);
    size_t app_len = 1;

    if (share_loc) {
        appdata[0] |= MESHCORE_ADV_LATLON_MASK;
        memcpy(&appdata[app_len], &lat_e6, 4); app_len += 4;
        memcpy(&appdata[app_len], &lon_e6, 4); app_len += 4;
    }

    if (name != NULL && name[0] != '\0') {
        size_t nlen = strlen(name);
        if (nlen > sizeof(appdata) - app_len) {
            nlen = sizeof(appdata) - app_len;
        }
        if (nlen > 0) {
            appdata[0] |= MESHCORE_ADV_NAME_MASK;
            memcpy(&appdata[app_len], name, nlen);
            app_len += nlen;
        }
    }

    /* Signed message: pub_key(32) || timestamp_le(4) || appdata(app_len) */
    uint8_t sign_msg[MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_MAX_ADVERT_DATA_SIZE];
    memcpy(&sign_msg[0], pub, MESHCORE_PUB_KEY_SIZE);
    memcpy(&sign_msg[32], &timestamp, 4);
    memcpy(&sign_msg[36], appdata, app_len);
    size_t sign_msg_len = 36 + app_len;

    uint8_t sig[MESHCORE_SIGNATURE_SIZE];
    meshcore_ed25519_sign(sig, sign_msg, sign_msg_len, pub, prv);

    /* Total raw packet: header(1) + path_len(1) + payload(100 + app_len) */
    size_t total_len = 2 + MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_SIGNATURE_SIZE + app_len;
    if (cap < total_len) {
        return 0;
    }

    out_raw[0] = header;
    out_raw[1] = 0x00; /* Zero hop / path_len = 0 */
    memcpy(&out_raw[2], pub, MESHCORE_PUB_KEY_SIZE);
    memcpy(&out_raw[34], &timestamp, 4);
    memcpy(&out_raw[38], sig, MESHCORE_SIGNATURE_SIZE);
    memcpy(&out_raw[102], appdata, app_len);

    return total_len;
}

bool meshcore_parse_advert_payload(const uint8_t *payload,
                                   size_t len,
                                   meshcore_advert_t *out)
{
    /* Minimum payload: pub(32) + ts(4) + sig(64) + flags(1) = 101 bytes */
    if (payload == NULL || out == NULL || len < 101) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    memcpy(out->pub_key, &payload[0], MESHCORE_PUB_KEY_SIZE);
    memcpy(&out->timestamp, &payload[32], 4);
    const uint8_t *sig = &payload[36];
    const uint8_t *appdata = &payload[100];
    size_t app_len = len - 100;
    if (app_len > MESHCORE_MAX_ADVERT_DATA_SIZE) {
        app_len = MESHCORE_MAX_ADVERT_DATA_SIZE;
    }

    /* Verify signature */
    uint8_t sign_msg[MESHCORE_PUB_KEY_SIZE + 4 + MESHCORE_MAX_ADVERT_DATA_SIZE];
    memcpy(&sign_msg[0], out->pub_key, MESHCORE_PUB_KEY_SIZE);
    memcpy(&sign_msg[32], &out->timestamp, 4);
    memcpy(&sign_msg[36], appdata, app_len);

    if (!meshcore_ed25519_verify(sig, sign_msg, 36 + app_len, out->pub_key)) {
        return false;
    }

    /* Parse appdata fields */
    uint8_t flags = appdata[0];
    out->node_type = flags & 0x0F;
    size_t idx = 1;

    if (flags & MESHCORE_ADV_LATLON_MASK) {
        if (app_len < idx + 8) {
            return false;
        }
        memcpy(&out->lat_e6, &appdata[idx], 4); idx += 4;
        memcpy(&out->lon_e6, &appdata[idx], 4); idx += 4;
        out->has_loc = true;
    } else {
        out->has_loc = false;
        out->lat_e6 = 0;
        out->lon_e6 = 0;
    }

    if (flags & MESHCORE_ADV_FEAT1_MASK) {
        if (app_len < idx + 2) {
            return false;
        }
        idx += 2;
    }
    if (flags & MESHCORE_ADV_FEAT2_MASK) {
        if (app_len < idx + 2) {
            return false;
        }
        idx += 2;
    }

    if (flags & MESHCORE_ADV_NAME_MASK) {
        if (app_len > idx) {
            size_t nlen = app_len - idx;
            if (nlen >= sizeof(out->name)) {
                nlen = sizeof(out->name) - 1;
            }
            memcpy(out->name, &appdata[idx], nlen);
            out->name[nlen] = '\0';
        }
    }

    return true;
}

/* ========================================================================= */
/* Discovery Encoding & Parsing                                              */
/* ========================================================================= */

size_t meshcore_build_discover_req(uint8_t *out_raw, size_t cap, uint32_t tag)
{
    if (out_raw == NULL || cap < 8) {
        return 0;
    }
    out_raw[0] = (MESHCORE_PAYLOAD_CONTROL << MESHCORE_PH_TYPE_SHIFT) | MESHCORE_ROUTE_DIRECT;
    out_raw[1] = 0x00; /* path_len = 0 */
    out_raw[2] = 0x81; /* CTL_TYPE_NODE_DISCOVER_REQ | 1 (prefix only) */
    out_raw[3] = 0xFF; /* filter */
    memcpy(&out_raw[4], &tag, 4);
    return 8;
}

bool meshcore_parse_discover_resp(const uint8_t *payload,
                                  size_t len,
                                  meshcore_neighbor_t *out,
                                  uint32_t *out_tag)
{
    if (payload == NULL || len < 14) {
        return false;
    }
    if ((payload[0] & 0xF0) != MESHCORE_CTL_DISCOVER_RESP) {
        return false;
    }

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->node_type = payload[0] & 0x0F;
        out->snr_in_q4 = (int8_t)payload[1];
        memcpy(out->pub_key_prefix, &payload[6], 8);
    }
    if (out_tag != NULL) {
        memcpy(out_tag, &payload[2], 4);
    }
    return true;
}

size_t meshcore_build_discover_resp(uint8_t *out_raw,
                                    size_t cap,
                                    uint32_t tag,
                                    uint8_t node_type,
                                    int8_t snr_in_q4,
                                    const uint8_t pub_key_prefix[8])
{
    if (out_raw == NULL || pub_key_prefix == NULL || cap < 16) {
        return 0;
    }
    out_raw[0] = (MESHCORE_PAYLOAD_CONTROL << MESHCORE_PH_TYPE_SHIFT) | MESHCORE_ROUTE_DIRECT;
    out_raw[1] = 0x00;
    out_raw[2] = (uint8_t)(MESHCORE_CTL_DISCOVER_RESP | (node_type & 0x0F));
    out_raw[3] = (uint8_t)snr_in_q4;
    memcpy(&out_raw[4], &tag, 4);
    memcpy(&out_raw[8], pub_key_prefix, 8);
    return 16;
}

/* ========================================================================= */
/* Group Channel Message Encoding & Parsing                                  */
/* ========================================================================= */

bool meshcore_channel_key_from_psk(const char *psk_b64,
                                   uint8_t secret32[32],
                                   uint8_t *channel_hash)
{
    if (psk_b64 == NULL || secret32 == NULL || channel_hash == NULL) {
        return false;
    }
    memset(secret32, 0, 32);
    int len = util_base64_decode(psk_b64, secret32, 32);
    if (len != 16 && len != 32) {
        return false;
    }

    uint8_t hash[32];
    util_sha256(secret32, (len == 16) ? 16 : 32, hash);
    *channel_hash = hash[0];
    return true;
}

size_t meshcore_build_grp_txt(uint8_t *out_raw,
                              size_t cap,
                              const char *psk_b64,
                              uint32_t timestamp,
                              const char *sender_name,
                              const char *message)
{
    if (out_raw == NULL || psk_b64 == NULL || message == NULL) {
        return 0;
    }

    uint8_t secret32[32] = {0};
    uint8_t channel_hash = 0;
    if (!meshcore_channel_key_from_psk(psk_b64, secret32, &channel_hash)) {
        return 0;
    }

    /* Plaintext: [timestamp_le(4)] [0x00 (txt_type)] ["<sender_name>: <message>"] */
    uint8_t plain[MESHCORE_MAX_PACKET_PAYLOAD] = {0};
    memcpy(&plain[0], &timestamp, 4);
    plain[4] = 0x00; /* TXT_TYPE_PLAIN */

    int written = 0;
    if (sender_name != NULL && sender_name[0] != '\0') {
        written = snprintf((char *)&plain[5], sizeof(plain) - 5, "%s: %s", sender_name, message);
    } else {
        written = snprintf((char *)&plain[5], sizeof(plain) - 5, "%s", message);
    }
    if (written < 0) {
        return 0;
    }

    size_t plain_len = 5 + (size_t)written;
    if (plain_len > sizeof(plain)) {
        plain_len = sizeof(plain);
    }

    uint8_t cipher[MESHCORE_MAX_PACKET_PAYLOAD];
    int enc_len = meshcore_encrypt_then_mac(secret32, cipher, sizeof(cipher), plain, plain_len);
    if (enc_len <= 0) {
        return 0;
    }

    /* Raw packet: header(1) + path_len(1) + channel_hash(1) + cipher(enc_len) */
    size_t total_len = 2 + 1 + (size_t)enc_len;
    if (cap < total_len) {
        return 0;
    }

    out_raw[0] = (MESHCORE_PAYLOAD_GRP_TXT << MESHCORE_PH_TYPE_SHIFT) | MESHCORE_ROUTE_FLOOD;
    out_raw[1] = 0x00; /* path_len = 0 */
    out_raw[2] = channel_hash;
    memcpy(&out_raw[3], cipher, (size_t)enc_len);

    return total_len;
}

bool meshcore_parse_grp_txt_payload(const uint8_t *payload,
                                    size_t len,
                                    const char *psk_b64,
                                    uint32_t *out_timestamp,
                                    char *out_text,
                                    size_t text_cap)
{
    if (payload == NULL || len < 4 || psk_b64 == NULL || out_text == NULL || text_cap == 0) {
        return false;
    }

    uint8_t secret32[32] = {0};
    uint8_t channel_hash = 0;
    if (!meshcore_channel_key_from_psk(psk_b64, secret32, &channel_hash)) {
        return false;
    }

    if (payload[0] != channel_hash) {
        return false;
    }

    uint8_t plain[MESHCORE_MAX_PACKET_PAYLOAD] = {0};
    int dec_len = meshcore_mac_then_decrypt(secret32, plain, sizeof(plain), payload + 1, len - 1);
    if (dec_len < 5) {
        return false;
    }

    if (out_timestamp != NULL) {
        memcpy(out_timestamp, &plain[0], 4);
    }

    /* plain[4] is txt_type */
    size_t str_len = (size_t)dec_len - 5;
    /* Plaintext was 0-padded at block boundary, find actual string length */
    size_t actual_len = 0;
    while (actual_len < str_len && plain[5 + actual_len] != '\0') {
        actual_len++;
    }

    if (actual_len >= text_cap) {
        actual_len = text_cap - 1;
    }
    memcpy(out_text, &plain[5], actual_len);
    out_text[actual_len] = '\0';

    return true;
}

/* ========================================================================= */
/* Semtech SX1262 LoRa Airtime Calculation                                   */
/* ========================================================================= */

uint32_t meshcore_calc_airtime_ms(uint32_t bw_hz,
                                  uint8_t sf,
                                  uint8_t cr,
                                  uint16_t preamble_len,
                                  size_t payload_bytes)
{
    if (bw_hz == 0 || sf < 5 || sf > 12) {
        return 0;
    }

    /* Symbol duration in microseconds */
    uint64_t symbol_length_us = ((uint64_t)1000000 << sf) / bw_hz;

    uint8_t sf_coeff1_x4 = (sf == 5 || sf == 6) ? 25 : 17;
    uint8_t sf_coeff2 = (sf == 5 || sf == 6) ? 0 : 8;

    bool ldro = (symbol_length_us >= 16384);
    uint32_t sf_divisor = ldro ? (4 * (sf - 2)) : (4 * sf);

    /* bitCount equation per SX1262 datasheet section 6.1.4 */
    int32_t bit_count = (int32_t)(8 * payload_bytes + 16 - 4 * sf + sf_coeff2 + 20);
    if (bit_count < 0) {
        bit_count = 0;
    }

    uint32_t n_precoded = ((uint32_t)bit_count + sf_divisor - 1) / sf_divisor;
    uint32_t cr_mult = (cr <= 4) ? (uint32_t)(cr + 4) : (uint32_t)cr;

    uint64_t n_symbol_x4 = ((uint64_t)preamble_len + 8) * 4 + sf_coeff1_x4 + (uint64_t)n_precoded * cr_mult * 4;
    uint64_t toa_us = (symbol_length_us * n_symbol_x4) / 4;

    return (uint32_t)((toa_us + 999) / 1000);
}
