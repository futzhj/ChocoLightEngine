/**
 * ChocoLight Script Protection — Shared encryption/decryption header
 * Used by both Android JNI and iOS runtime to decrypt packed Lua scripts
 *
 * File format (.enc):
 *   [4 bytes] magic: "CLPK"
 *   [4 bytes] version: 1
 *   [4 bytes] payload_size (encrypted lua data)
 *   [1 byte]  xor_key_len
 *   [63 bytes] xor_key (encrypted with master key, padded with 0)
 *   [payload_size bytes] XOR-encrypted Lua script data
 *
 * Total header: 76 bytes
 */

#ifndef CHOCO_CRYPT_H
#define CHOCO_CRYPT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHOCO_MAGIC     "CLPK"
#define CHOCO_VERSION   1
#define CHOCO_HDR_SIZE  76
#define CHOCO_XK_MAX    63

// Master key — obfuscated (XOR with index), same as lightpack_mobile.py
// Actual key: "ChocoLight2026!@#"
static const uint8_t _choco_mk_enc[] = {
    0x43^0,  0x68^1,  0x6F^2,  0x63^3,  0x6F^4,  0x4C^5,  0x69^6,  0x67^7,
    0x68^8,  0x74^9,  0x32^10, 0x30^11, 0x32^12, 0x36^13, 0x21^14, 0x40^15,
    0x23^16
};
#define CHOCO_MK_LEN 17

static void choco_get_master_key(uint8_t *out) {
    for (int i = 0; i < CHOCO_MK_LEN; i++) {
        out[i] = _choco_mk_enc[i] ^ (uint8_t)i;
    }
}

/**
 * Decrypt a .enc file buffer in-place.
 *
 * @param enc_data   pointer to the entire .enc file content
 * @param enc_size   total size of enc_data
 * @param out_data   [OUT] pointer to decrypted Lua data (caller must free)
 * @param out_size   [OUT] size of decrypted data
 * @return 0 on success, -1 on error
 */
static int choco_decrypt(const uint8_t *enc_data, size_t enc_size,
                         uint8_t **out_data, size_t *out_size) {
    // Validate minimum size
    if (enc_size < CHOCO_HDR_SIZE) return -1;

    // Check magic
    if (memcmp(enc_data, CHOCO_MAGIC, 4) != 0) return -1;

    // Parse header (little-endian)
    uint32_t version = enc_data[4] | (enc_data[5] << 8) |
                       (enc_data[6] << 16) | (enc_data[7] << 24);
    if (version != CHOCO_VERSION) return -1;

    uint32_t payload_size = enc_data[8] | (enc_data[9] << 8) |
                            (enc_data[10] << 16) | (enc_data[11] << 24);

    uint8_t xk_len = enc_data[12];
    if (xk_len == 0 || xk_len > CHOCO_XK_MAX) return -1;

    // Verify we have enough data
    if (enc_size < CHOCO_HDR_SIZE + payload_size) return -1;

    // Recover master key
    uint8_t mk[CHOCO_MK_LEN];
    choco_get_master_key(mk);

    // Decrypt XOR key from header
    uint8_t xor_key[CHOCO_XK_MAX];
    const uint8_t *enc_xk = enc_data + 13;
    for (int i = 0; i < xk_len; i++) {
        xor_key[i] = enc_xk[i] ^ mk[i % CHOCO_MK_LEN];
    }

    // Decrypt payload
    const uint8_t *payload = enc_data + CHOCO_HDR_SIZE;
    uint8_t *result = (uint8_t *)malloc(payload_size + 1);
    if (!result) return -1;

    for (uint32_t i = 0; i < payload_size; i++) {
        result[i] = payload[i] ^ xor_key[i % xk_len];
    }
    result[payload_size] = '\0';

    *out_data = result;
    *out_size = payload_size;
    return 0;
}

#endif // CHOCO_CRYPT_H
