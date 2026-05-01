/**
 * tiny_hash.h — 紧凑 SHA-256 / MD5 / Base64 实现 (公共领域)
 *
 * 用法:
 *   uint8_t digest[32];
 *   sha256(data, len, digest);
 *
 *   uint8_t md5_digest[16];
 *   md5(data, len, md5_digest);
 *
 *   char b64[len * 4 / 3 + 4];
 *   int b64_len = base64_encode(data, len, b64);
 *   int decoded = base64_decode(b64, b64_len, out_buf);
 */
#ifndef TINY_HASH_H
#define TINY_HASH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// SHA-256: 输出 32 字节
void sha256(const void* data, size_t len, uint8_t out[32]);

// MD5: 输出 16 字节
void md5(const void* data, size_t len, uint8_t out[16]);

// Base64 编码: 返回写入字节数 (不含末尾 \0)
// 输出缓冲必须 >= ((len + 2) / 3) * 4 字节
int base64_encode(const uint8_t* in, size_t in_len, char* out);

// Base64 解码: 返回写入字节数, -1 表示输入无效
// 输出缓冲必须 >= (in_len / 4) * 3 字节
int base64_decode(const char* in, size_t in_len, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif // TINY_HASH_H
