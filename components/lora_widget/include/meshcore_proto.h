#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MeshCore packet header bit-fields (matching MeshCore_mod/src/Packet.h) */
#define MESHCORE_PH_ROUTE_MASK     0x03   /* 2-bits */
#define MESHCORE_PH_TYPE_SHIFT     2
#define MESHCORE_PH_TYPE_MASK      0x0F   /* 4-bits */
#define MESHCORE_PH_VER_SHIFT      6
#define MESHCORE_PH_VER_MASK       0x03   /* 2-bits */

/* Route types */
#define MESHCORE_ROUTE_TRANSPORT_FLOOD   0x00
#define MESHCORE_ROUTE_FLOOD             0x01
#define MESHCORE_ROUTE_DIRECT            0x02
#define MESHCORE_ROUTE_TRANSPORT_DIRECT  0x03

/* Payload types */
#define MESHCORE_PAYLOAD_REQ         0x00
#define MESHCORE_PAYLOAD_RESPONSE    0x01
#define MESHCORE_PAYLOAD_TXT_MSG     0x02
#define MESHCORE_PAYLOAD_ACK         0x03
#define MESHCORE_PAYLOAD_ADVERT      0x04
#define MESHCORE_PAYLOAD_GRP_TXT     0x05
#define MESHCORE_PAYLOAD_GRP_DATA    0x06
#define MESHCORE_PAYLOAD_ANON_REQ    0x07
#define MESHCORE_PAYLOAD_PATH        0x08
#define MESHCORE_PAYLOAD_TRACE       0x09
#define MESHCORE_PAYLOAD_MULTIPART   0x0A
#define MESHCORE_PAYLOAD_CONTROL     0x0B
#define MESHCORE_PAYLOAD_RAW_CUSTOM  0x0F

/* Node types (flags & 0x0F in Advert appdata) */
#define MESHCORE_ADV_NONE      0
#define MESHCORE_ADV_CHAT      1
#define MESHCORE_ADV_REPEATER  2
#define MESHCORE_ADV_ROOM      3
#define MESHCORE_ADV_SENSOR    4

/* Advert appdata flags */
#define MESHCORE_ADV_LATLON_MASK 0x10
#define MESHCORE_ADV_FEAT1_MASK  0x20
#define MESHCORE_ADV_FEAT2_MASK  0x40
#define MESHCORE_ADV_NAME_MASK   0x80

/* Control frame sub-types */
#define MESHCORE_CTL_DISCOVER_REQ   0x80
#define MESHCORE_CTL_DISCOVER_RESP  0x90

/* Crypto & protocol dimensions */
#define MESHCORE_DEFAULT_PUBLIC_PSK  "izOH6cXN6mrJ5e26oRXNcg=="
#define MESHCORE_PUB_KEY_SIZE        32
#define MESHCORE_PRV_KEY_SIZE        64
#define MESHCORE_SEED_SIZE           32
#define MESHCORE_SIGNATURE_SIZE      64
#define MESHCORE_CIPHER_KEY_SIZE     16
#define MESHCORE_CIPHER_BLOCK_SIZE   16
#define MESHCORE_CIPHER_MAC_SIZE     2
#define MESHCORE_MAX_PATH_SIZE       64
#define MESHCORE_MAX_PACKET_PAYLOAD  184
#define MESHCORE_MAX_ADVERT_DATA_SIZE 32
#define MESHCORE_NAME_LEN            32

/* Cache & ring buffer capacity */
#define MESHCORE_MAX_ADVERTS         12
#define MESHCORE_MAX_NEIGHBORS       12
#define MESHCORE_MAX_MESSAGES        64

/* Decoded MeshCore advertisement record */
typedef struct {
    uint8_t  pub_key[MESHCORE_PUB_KEY_SIZE];
    uint8_t  node_type;
    char     name[MESHCORE_NAME_LEN];
    bool     has_loc;
    int32_t  lat_e6;
    int32_t  lon_e6;
    uint8_t  hops;
    int16_t  rssi_dbm;
    int8_t   snr_q4; /* SNR * 4 for 0.25 dB resolution */
    uint32_t timestamp;
    uint32_t rx_time_s;
} meshcore_advert_t;

/* Zero-Hop neighbor discovery record */
typedef struct {
    uint8_t  pub_key_prefix[8];
    uint8_t  node_type;
    char     name[MESHCORE_NAME_LEN];
    int8_t   snr_in_q4;  /* remote node SNR reported back */
    int8_t   snr_out_q4; /* local measured SNR */
    int16_t  rssi_dbm;
    uint32_t rx_time_s;
} meshcore_neighbor_t;

/* ACK status for outgoing direct messages */
typedef enum {
    MESHCORE_ACK_NONE = 0,
    MESHCORE_ACK_PENDING = 1,
    MESHCORE_ACK_DELIVERED = 2,
    MESHCORE_ACK_TIMEOUT = 3
} meshcore_ack_status_t;

/* Group channel / direct chat message */
typedef struct {
    char     text[160];
    uint32_t timestamp;
    uint32_t rx_time_s;
    int16_t  rssi_dbm;
    int8_t   snr_q4;
    uint8_t  hops;
    bool     outgoing;
    uint32_t id;
    char     sender[MESHCORE_NAME_LEN];
    char     body[160];
    bool     is_direct;
    uint8_t  channel_idx;
    uint8_t  peer_pub[MESHCORE_PUB_KEY_SIZE];
    uint8_t  ack_status;
    uint8_t  ack_tag[4];
    uint16_t ack_rtt_ms;
    bool     route_direct;
} meshcore_msg_t;

/* MeshCore wire packet structure */
typedef struct {
    uint8_t  header;
    uint16_t transport_codes[2];
    uint8_t  path_len; /* bits 0..5 = hop_count, bits 6..7 = hash_size - 1 */
    uint8_t  path[MESHCORE_MAX_PATH_SIZE];
    uint8_t  payload[MESHCORE_MAX_PACKET_PAYLOAD];
    size_t   payload_len;
} meshcore_packet_t;

/* --- Cryptographic Primitives (meshcore_crypto.c) --- */

void meshcore_ed25519_create_keypair(uint8_t pub[MESHCORE_PUB_KEY_SIZE],
                                    uint8_t prv[MESHCORE_PRV_KEY_SIZE],
                                    const uint8_t seed[MESHCORE_SEED_SIZE]);

void meshcore_ed25519_sign(uint8_t sig[MESHCORE_SIGNATURE_SIZE],
                           const uint8_t *msg,
                           size_t msg_len,
                           const uint8_t pub[MESHCORE_PUB_KEY_SIZE],
                           const uint8_t prv[MESHCORE_PRV_KEY_SIZE]);

bool meshcore_ed25519_verify(const uint8_t sig[MESHCORE_SIGNATURE_SIZE],
                            const uint8_t *msg,
                            size_t msg_len,
                            const uint8_t pub[MESHCORE_PUB_KEY_SIZE]);

void meshcore_aes128_ecb_encrypt(const uint8_t key[MESHCORE_CIPHER_KEY_SIZE],
                                 const uint8_t in[MESHCORE_CIPHER_BLOCK_SIZE],
                                 uint8_t out[MESHCORE_CIPHER_BLOCK_SIZE]);

void meshcore_aes128_ecb_decrypt(const uint8_t key[MESHCORE_CIPHER_KEY_SIZE],
                                 const uint8_t in[MESHCORE_CIPHER_BLOCK_SIZE],
                                 uint8_t out[MESHCORE_CIPHER_BLOCK_SIZE]);

int meshcore_encrypt_then_mac(const uint8_t secret32[32],
                              uint8_t *dest,
                              size_t dest_cap,
                              const uint8_t *src,
                              size_t src_len);

int meshcore_mac_then_decrypt(const uint8_t secret32[32],
                              uint8_t *dest,
                              size_t dest_cap,
                              const uint8_t *src,
                              size_t src_len);

/* --- Protocol Wire Encoders & Parsers (meshcore_proto.c) --- */

bool meshcore_packet_decode(meshcore_packet_t *pkt, const uint8_t *raw, size_t len);

size_t meshcore_packet_encode(const meshcore_packet_t *pkt, uint8_t *raw, size_t cap);

/**
 * Calculate the 8-byte MeshCore packet deduplication hash:
 * SHA-256 over [payload_type (1B)] + [path_len if TRACE] + [payload (payload_len B)].
 * Independent of hop count and path bytes, matching MeshCore Packet::calculatePacketHash().
 */
void meshcore_packet_hash(const meshcore_packet_t *pkt, uint8_t hash_out[8]);

uint8_t meshcore_packet_get_route_type(const meshcore_packet_t *pkt);

uint8_t meshcore_packet_get_payload_type(const meshcore_packet_t *pkt);

uint8_t meshcore_packet_get_hash_size(const meshcore_packet_t *pkt);

uint8_t meshcore_packet_get_hop_count(const meshcore_packet_t *pkt);

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
                             int32_t lon_e6);

bool meshcore_parse_advert_payload(const uint8_t *payload,
                                   size_t len,
                                   meshcore_advert_t *out);

size_t meshcore_build_discover_req(uint8_t *out_raw, size_t cap, uint32_t tag);

bool meshcore_parse_discover_resp(const uint8_t *payload,
                                  size_t len,
                                  meshcore_neighbor_t *out,
                                  uint32_t *out_tag);

size_t meshcore_build_discover_resp(uint8_t *out_raw,
                                    size_t cap,
                                    uint32_t tag,
                                    uint8_t node_type,
                                    int8_t snr_in_q4,
                                    const uint8_t pub_key_prefix[8]);

bool meshcore_channel_key_from_psk(const char *psk_b64,
                                   uint8_t secret32[32],
                                   uint8_t *channel_hash);

bool meshcore_channel_key_from_secret(const uint8_t secret[16],
                                      uint8_t secret32[32],
                                      uint8_t *channel_hash);

size_t meshcore_build_grp_txt(uint8_t *out_raw,
                              size_t cap,
                              const char *psk_b64,
                              uint32_t timestamp,
                              const char *sender_name,
                              const char *message);

size_t meshcore_build_grp_txt_secret(uint8_t *out_raw,
                                     size_t cap,
                                     const uint8_t secret[16],
                                     uint32_t timestamp,
                                     const char *sender_name,
                                     const char *message);

bool meshcore_parse_grp_txt_payload(const uint8_t *payload,
                                    size_t len,
                                    const char *psk_b64,
                                    uint32_t *out_timestamp,
                                    char *out_text,
                                    size_t text_cap);

bool meshcore_parse_grp_txt_payload_secret(const uint8_t *payload,
                                           size_t len,
                                           const uint8_t secret[16],
                                           uint32_t *out_timestamp,
                                           char *out_text,
                                           size_t text_cap);

uint32_t meshcore_calc_airtime_ms(uint32_t bw_hz,
                                  uint8_t sf,
                                  uint8_t cr,
                                  uint16_t preamble_len,
                                  size_t payload_bytes);

#ifdef __cplusplus
}
#endif
