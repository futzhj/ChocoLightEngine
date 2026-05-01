/**
 * tiny_hash.c — SHA-256 / MD5 / Base64 紧凑实现
 * 公共领域 (Public Domain)
 */

#include "tiny_hash.h"
#include <string.h>

// ============================================================
// SHA-256 (FIPS 180-4)
// ============================================================
static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t a, b, c, d, e, f, g, h, i;
    uint32_t W[64];

    for (i = 0; i < 16; ++i) {
        W[i] = ((uint32_t)block[i*4]   << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8)  |
               ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 64; ++i) {
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; ++i) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + SHA256_K[i] + W[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };

    const uint8_t* p = (const uint8_t*)data;
    size_t remaining = len;
    while (remaining >= 64) {
        sha256_compress(state, p);
        p += 64;
        remaining -= 64;
    }

    // 填充: 0x80, 0...0, 长度 (bits) 大端 8 字节
    uint8_t buf[128];
    memcpy(buf, p, remaining);
    buf[remaining] = 0x80;
    size_t pad_len = (remaining < 56) ? 64 : 128;
    memset(buf + remaining + 1, 0, pad_len - remaining - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) {
        buf[pad_len - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    sha256_compress(state, buf);
    if (pad_len == 128) sha256_compress(state, buf + 64);

    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (uint8_t)(state[i] >> 24);
        out[i*4+1] = (uint8_t)(state[i] >> 16);
        out[i*4+2] = (uint8_t)(state[i] >> 8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
}

// ============================================================
// MD5 (RFC 1321)
// ============================================================
#define MD5_F(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x,y,z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x,y,z) ((x) ^ (y) ^ (z))
#define MD5_I(x,y,z) ((y) ^ ((x) | ~(z)))
#define MD5_LR(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const uint32_t MD5_K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const int MD5_S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void md5_compress(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = ((uint32_t)block[i*4])       |
               ((uint32_t)block[i*4+1] << 8)|
               ((uint32_t)block[i*4+2] << 16)|
               ((uint32_t)block[i*4+3] << 24);
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16)      { f = MD5_F(b,c,d); g = i; }
        else if (i < 32) { f = MD5_G(b,c,d); g = (5*i + 1) % 16; }
        else if (i < 48) { f = MD5_H(b,c,d); g = (3*i + 5) % 16; }
        else             { f = MD5_I(b,c,d); g = (7*i) % 16; }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + MD5_LR(a + f + MD5_K[i] + M[g], MD5_S[i]);
        a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void md5(const void* data, size_t len, uint8_t out[16]) {
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t* p = (const uint8_t*)data;
    size_t remaining = len;
    while (remaining >= 64) {
        md5_compress(state, p);
        p += 64; remaining -= 64;
    }

    uint8_t buf[128];
    memcpy(buf, p, remaining);
    buf[remaining] = 0x80;
    size_t pad_len = (remaining < 56) ? 64 : 128;
    memset(buf + remaining + 1, 0, pad_len - remaining - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) {
        buf[pad_len - 8 + i] = (uint8_t)(bits >> (i * 8));  // 小端
    }
    md5_compress(state, buf);
    if (pad_len == 128) md5_compress(state, buf + 64);

    for (int i = 0; i < 4; ++i) {
        out[i*4]   = (uint8_t)(state[i]);
        out[i*4+1] = (uint8_t)(state[i] >> 8);
        out[i*4+2] = (uint8_t)(state[i] >> 16);
        out[i*4+3] = (uint8_t)(state[i] >> 24);
    }
}

// ============================================================
// Base64 (RFC 4648 标准字母表)
// ============================================================
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(const uint8_t* in, size_t in_len, char* out) {
    int n = 0;
    size_t i = 0;
    while (i + 3 <= in_len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[n++] = B64_CHARS[(v >> 18) & 0x3F];
        out[n++] = B64_CHARS[(v >> 12) & 0x3F];
        out[n++] = B64_CHARS[(v >> 6) & 0x3F];
        out[n++] = B64_CHARS[v & 0x3F];
        i += 3;
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[n++] = B64_CHARS[(v >> 18) & 0x3F];
        out[n++] = B64_CHARS[(v >> 12) & 0x3F];
        out[n++] = '=';
        out[n++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[n++] = B64_CHARS[(v >> 18) & 0x3F];
        out[n++] = B64_CHARS[(v >> 12) & 0x3F];
        out[n++] = B64_CHARS[(v >> 6) & 0x3F];
        out[n++] = '=';
    }
    return n;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;  // padding
    return -1;                // invalid
}

int base64_decode(const char* in, size_t in_len, uint8_t* out) {
    int n = 0;
    int buf[4];
    int bi = 0;
    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = b64_decode_char(c);
        if (v == -1) return -1;
        buf[bi++] = v;
        if (bi == 4) {
            // 解析一组 4 字符
            if (buf[0] < 0 || buf[1] < 0) return -1;
            out[n++] = (uint8_t)((buf[0] << 2) | (buf[1] >> 4));
            if (buf[2] >= 0) {
                out[n++] = (uint8_t)((buf[1] << 4) | (buf[2] >> 2));
                if (buf[3] >= 0) {
                    out[n++] = (uint8_t)((buf[2] << 6) | buf[3]);
                }
            }
            bi = 0;
        }
    }
    return n;
}
