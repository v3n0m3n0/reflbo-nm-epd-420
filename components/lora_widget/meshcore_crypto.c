#include "meshcore_proto.h"
#include "util_sha256.h"
#include "ed_25519.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ========================================================================= */
/* 1. Standard FIPS-197 AES-128 (ECB Mode)                                   */
/*    Based on tiny-AES-c (Public Domain / Unlicense)                        */
/* ========================================================================= */

#define AES_BLOCKLEN 16
#define Nb 4
#define Nk 4
#define Nr 10

typedef uint8_t state_t[4][4];

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t rsbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint8_t Rcon[11] = {
    0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static void aes_key_expansion(uint8_t *RoundKey, const uint8_t *Key)
{
    unsigned i, j, k;
    uint8_t tempa[4];

    for (i = 0; i < Nk; ++i) {
        RoundKey[(i * 4) + 0] = Key[(i * 4) + 0];
        RoundKey[(i * 4) + 1] = Key[(i * 4) + 1];
        RoundKey[(i * 4) + 2] = Key[(i * 4) + 2];
        RoundKey[(i * 4) + 3] = Key[(i * 4) + 3];
    }

    for (i = Nk; i < Nb * (Nr + 1); ++i) {
        k = (i - 1) * 4;
        tempa[0] = RoundKey[k + 0];
        tempa[1] = RoundKey[k + 1];
        tempa[2] = RoundKey[k + 2];
        tempa[3] = RoundKey[k + 3];

        if (i % Nk == 0) {
            const uint8_t u8tmp = tempa[0];
            tempa[0] = tempa[1];
            tempa[1] = tempa[2];
            tempa[2] = tempa[3];
            tempa[3] = u8tmp;

            tempa[0] = sbox[tempa[0]];
            tempa[1] = sbox[tempa[1]];
            tempa[2] = sbox[tempa[2]];
            tempa[3] = sbox[tempa[3]];

            tempa[0] ^= Rcon[i / Nk];
        }

        j = i * 4;
        k = (i - Nk) * 4;
        RoundKey[j + 0] = RoundKey[k + 0] ^ tempa[0];
        RoundKey[j + 1] = RoundKey[k + 1] ^ tempa[1];
        RoundKey[j + 2] = RoundKey[k + 2] ^ tempa[2];
        RoundKey[j + 3] = RoundKey[k + 3] ^ tempa[3];
    }
}

static void aes_add_round_key(uint8_t round, state_t *state, const uint8_t *RoundKey)
{
    for (uint8_t i = 0; i < 4; ++i) {
        for (uint8_t j = 0; j < 4; ++j) {
            (*state)[i][j] ^= RoundKey[(round * Nb * 4) + (i * Nb) + j];
        }
    }
}

static void aes_sub_bytes(state_t *state)
{
    for (uint8_t i = 0; i < 4; ++i) {
        for (uint8_t j = 0; j < 4; ++j) {
            (*state)[j][i] = sbox[(*state)[j][i]];
        }
    }
}

static void aes_shift_rows(state_t *state)
{
    uint8_t temp;

    temp           = (*state)[0][1];
    (*state)[0][1] = (*state)[1][1];
    (*state)[1][1] = (*state)[2][1];
    (*state)[2][1] = (*state)[3][1];
    (*state)[3][1] = temp;

    temp           = (*state)[0][2];
    (*state)[0][2] = (*state)[2][2];
    (*state)[2][2] = temp;
    temp           = (*state)[1][2];
    (*state)[1][2] = (*state)[3][2];
    (*state)[3][2] = temp;

    temp           = (*state)[0][3];
    (*state)[0][3] = (*state)[3][3];
    (*state)[3][3] = (*state)[2][3];
    (*state)[2][3] = (*state)[1][3];
    (*state)[1][3] = temp;
}

static inline uint8_t aes_xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static void aes_mix_columns(state_t *state)
{
    for (uint8_t i = 0; i < 4; ++i) {
        uint8_t t   = (*state)[i][0];
        uint8_t Tmp = (*state)[i][0] ^ (*state)[i][1] ^ (*state)[i][2] ^ (*state)[i][3];
        uint8_t Tm;

        Tm = (*state)[i][0] ^ (*state)[i][1]; Tm = aes_xtime(Tm); (*state)[i][0] ^= Tm ^ Tmp;
        Tm = (*state)[i][1] ^ (*state)[i][2]; Tm = aes_xtime(Tm); (*state)[i][1] ^= Tm ^ Tmp;
        Tm = (*state)[i][2] ^ (*state)[i][3]; Tm = aes_xtime(Tm); (*state)[i][2] ^= Tm ^ Tmp;
        Tm = (*state)[i][3] ^ t;              Tm = aes_xtime(Tm); (*state)[i][3] ^= Tm ^ Tmp;
    }
}

static inline uint8_t aes_multiply(uint8_t x, uint8_t y)
{
    return (uint8_t)(
        ((y & 1) * x) ^
        (((y >> 1) & 1) * aes_xtime(x)) ^
        (((y >> 2) & 1) * aes_xtime(aes_xtime(x))) ^
        (((y >> 3) & 1) * aes_xtime(aes_xtime(aes_xtime(x)))) ^
        (((y >> 4) & 1) * aes_xtime(aes_xtime(aes_xtime(aes_xtime(x)))))
    );
}

static void aes_inv_mix_columns(state_t *state)
{
    for (int i = 0; i < 4; ++i) {
        uint8_t a = (*state)[i][0];
        uint8_t b = (*state)[i][1];
        uint8_t c = (*state)[i][2];
        uint8_t d = (*state)[i][3];

        (*state)[i][0] = aes_multiply(a, 0x0e) ^ aes_multiply(b, 0x0b) ^ aes_multiply(c, 0x0d) ^ aes_multiply(d, 0x09);
        (*state)[i][1] = aes_multiply(a, 0x09) ^ aes_multiply(b, 0x0e) ^ aes_multiply(c, 0x0b) ^ aes_multiply(d, 0x0d);
        (*state)[i][2] = aes_multiply(a, 0x0d) ^ aes_multiply(b, 0x09) ^ aes_multiply(c, 0x0e) ^ aes_multiply(d, 0x0b);
        (*state)[i][3] = aes_multiply(a, 0x0b) ^ aes_multiply(b, 0x0d) ^ aes_multiply(c, 0x09) ^ aes_multiply(d, 0x0e);
    }
}

static void aes_inv_sub_bytes(state_t *state)
{
    for (uint8_t i = 0; i < 4; ++i) {
        for (uint8_t j = 0; j < 4; ++j) {
            (*state)[j][i] = rsbox[(*state)[j][i]];
        }
    }
}

static void aes_inv_shift_rows(state_t *state)
{
    uint8_t temp;

    temp           = (*state)[3][1];
    (*state)[3][1] = (*state)[2][1];
    (*state)[2][1] = (*state)[1][1];
    (*state)[1][1] = (*state)[0][1];
    (*state)[0][1] = temp;

    temp           = (*state)[0][2];
    (*state)[0][2] = (*state)[2][2];
    (*state)[2][2] = temp;
    temp           = (*state)[1][2];
    (*state)[1][2] = (*state)[3][2];
    (*state)[3][2] = temp;

    temp           = (*state)[0][3];
    (*state)[0][3] = (*state)[1][3];
    (*state)[1][3] = (*state)[2][3];
    (*state)[2][3] = (*state)[3][3];
    (*state)[3][3] = temp;
}

static void aes_cipher(state_t *state, const uint8_t *RoundKey)
{
    aes_add_round_key(0, state, RoundKey);

    for (uint8_t round = 1;; ++round) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        if (round == Nr) {
            break;
        }
        aes_mix_columns(state);
        aes_add_round_key(round, state, RoundKey);
    }
    aes_add_round_key(Nr, state, RoundKey);
}

static void aes_inv_cipher(state_t *state, const uint8_t *RoundKey)
{
    aes_add_round_key(Nr, state, RoundKey);

    for (uint8_t round = (Nr - 1);; --round) {
        aes_inv_shift_rows(state);
        aes_inv_sub_bytes(state);
        aes_add_round_key(round, state, RoundKey);
        if (round == 0) {
            break;
        }
        aes_inv_mix_columns(state);
    }
}

void meshcore_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t round_key[176];
    aes_key_expansion(round_key, key);
    memcpy(out, in, 16);
    aes_cipher((state_t *)out, round_key);
}

void meshcore_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t round_key[176];
    aes_key_expansion(round_key, key);
    memcpy(out, in, 16);
    aes_inv_cipher((state_t *)out, round_key);
}

/* ========================================================================= */
/* 2. MeshCore Encrypt-Then-MAC and MAC-Then-Decrypt                         */
/*    Matches MeshCore_mod/src/Utils.cpp                                     */
/*    AES-128-ECB + HMAC-SHA256 (first 2 bytes as MAC)                      */
/* ========================================================================= */

#define MESHCORE_CIPHER_KEY_SIZE   16
#define MESHCORE_CIPHER_BLOCK_SIZE 16
#define MESHCORE_CIPHER_MAC_SIZE   2
#define MESHCORE_HMAC_KEY_SIZE     32

int meshcore_encrypt_then_mac(const uint8_t secret32[32],
                              uint8_t *dest,
                              size_t dest_cap,
                              const uint8_t *src,
                              size_t src_len)
{
    if (!secret32 || !dest || !src || src_len == 0) {
        return 0;
    }

    size_t full_blocks = src_len / MESHCORE_CIPHER_BLOCK_SIZE;
    size_t remainder   = src_len % MESHCORE_CIPHER_BLOCK_SIZE;
    size_t total_blocks = full_blocks + (remainder > 0 ? 1 : 0);
    size_t enc_len = total_blocks * MESHCORE_CIPHER_BLOCK_SIZE;

    if (dest_cap < MESHCORE_CIPHER_MAC_SIZE + enc_len) {
        return 0;
    }

    uint8_t *ct_ptr = dest + MESHCORE_CIPHER_MAC_SIZE;
    const uint8_t *sp = src;

    for (size_t b = 0; b < full_blocks; b++) {
        meshcore_aes128_ecb_encrypt(secret32, sp, ct_ptr);
        sp += MESHCORE_CIPHER_BLOCK_SIZE;
        ct_ptr += MESHCORE_CIPHER_BLOCK_SIZE;
    }

    if (remainder > 0) {
        uint8_t pad_block[MESHCORE_CIPHER_BLOCK_SIZE] = {0};
        memcpy(pad_block, sp, remainder);
        meshcore_aes128_ecb_encrypt(secret32, pad_block, ct_ptr);
    }

    /* Compute HMAC-SHA256 over ciphertext (dest + 2, enc_len) using 32-byte secret */
    uint8_t full_hmac[32];
    util_hmac_sha256(secret32, MESHCORE_HMAC_KEY_SIZE,
                     dest + MESHCORE_CIPHER_MAC_SIZE, enc_len,
                     full_hmac);

    /* First 2 bytes become the MAC */
    dest[0] = full_hmac[0];
    dest[1] = full_hmac[1];

    return (int)(MESHCORE_CIPHER_MAC_SIZE + enc_len);
}

int meshcore_mac_then_decrypt(const uint8_t secret32[32],
                              uint8_t *dest,
                              size_t dest_cap,
                              const uint8_t *src,
                              size_t src_len)
{
    if (!secret32 || !dest || !src) {
        return 0;
    }

    if (src_len <= MESHCORE_CIPHER_MAC_SIZE) {
        return 0;
    }

    size_t enc_len = src_len - MESHCORE_CIPHER_MAC_SIZE;
    if (enc_len % MESHCORE_CIPHER_BLOCK_SIZE != 0) {
        return 0;
    }

    if (dest_cap < enc_len) {
        return 0;
    }

    /* Verify HMAC-SHA256 */
    uint8_t full_hmac[32];
    util_hmac_sha256(secret32, MESHCORE_HMAC_KEY_SIZE,
                     src + MESHCORE_CIPHER_MAC_SIZE, enc_len,
                     full_hmac);

    if (src[0] != full_hmac[0] || src[1] != full_hmac[1]) {
        return 0;
    }

    /* Decrypt ciphertext blocks */
    const uint8_t *sp = src + MESHCORE_CIPHER_MAC_SIZE;
    uint8_t *dp = dest;
    size_t num_blocks = enc_len / MESHCORE_CIPHER_BLOCK_SIZE;

    for (size_t b = 0; b < num_blocks; b++) {
        meshcore_aes128_ecb_decrypt(secret32, sp, dp);
        sp += MESHCORE_CIPHER_BLOCK_SIZE;
        dp += MESHCORE_CIPHER_BLOCK_SIZE;
    }

    return (int)enc_len;
}

/* ========================================================================= */
/* 3. RFC 8032 Ed25519 Operations (Ported from MeshCore_mod)                 */
/* ========================================================================= */

void meshcore_ed25519_create_keypair(uint8_t pub[32], uint8_t prv[64], const uint8_t seed[32])
{
    ed25519_create_keypair(pub, prv, seed);
}

void meshcore_ed25519_sign(uint8_t sig[64],
                           const uint8_t *msg,
                           size_t msg_len,
                           const uint8_t pub[32],
                           const uint8_t prv[64])
{
    ed25519_sign(sig, msg, msg_len, pub, prv);
}

bool meshcore_ed25519_verify(const uint8_t sig[64],
                             const uint8_t *msg,
                             size_t msg_len,
                             const uint8_t pub[32])
{
    return ed25519_verify(sig, msg, msg_len, pub) != 0;
}
