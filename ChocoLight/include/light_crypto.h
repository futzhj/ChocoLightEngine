#pragma once
/**
 * @file light_crypto.h
 * @brief Shared XOR crypto utilities for ChocoLight engine protection
 */

#include <cstddef>
#include <cstring>
#include <cstdlib>

namespace LightCrypto {

// DLL master key (different from exe's key, obfuscated with index XOR)
// Actual: "LightDLL!Secure7"
static const unsigned char g_dllKeyEnc[] = {
    0x4C^0, 0x69^1, 0x67^2, 0x68^3, 0x74^4, 0x44^5, 0x4C^6, 0x4C^7,
    0x21^8, 0x53^9, 0x65^10, 0x63^11, 0x75^12, 0x72^13, 0x65^14, 0x37^15
};
static const int g_dllKeyLen = sizeof(g_dllKeyEnc);

inline unsigned char dllKey(int i) {
    return g_dllKeyEnc[i % g_dllKeyLen] ^ (unsigned char)(i % g_dllKeyLen);
}

// XOR decrypt in-place, returns malloc'd buffer (caller must free)
inline char* decryptScript(const unsigned char *enc, size_t len) {
    char *out = (char*)malloc(len + 1);
    if (!out) return nullptr;
    for (size_t i = 0; i < len; ++i)
        out[i] = (char)(enc[i] ^ dllKey((int)i));
    out[len] = '\0';
    return out;
}

} // namespace LightCrypto
