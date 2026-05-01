/**
 * tiny_aes.h — 紧凑 AES 实现 (基于公共领域参考实现)
 * 支持 AES-128 / AES-192 / AES-256, ECB / CBC 模式
 *
 * 使用约定:
 *   1. 调用 AES_init_ctx(&ctx, key) 或 AES_init_ctx_iv(&ctx, key, iv)
 *   2. 调用 AES_ECB_encrypt_buffer / AES_CBC_encrypt_buffer 进行加密
 *   3. 输入长度必须为 16 字节倍数 (使用 PKCS7 填充)
 *
 * 默认编译为 AES-256 (CRYPTO_AES_KEY_SIZE = 32)
 */
#ifndef TINY_AES_H
#define TINY_AES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AES_BLOCKLEN 16

// 默认 AES-256 (32 字节密钥)
#ifndef AES_KEYLEN
#define AES_KEYLEN 32
#endif

#if (AES_KEYLEN == 32)
#define AES_NR     14
#define AES_NK     8
#elif (AES_KEYLEN == 24)
#define AES_NR     12
#define AES_NK     6
#elif (AES_KEYLEN == 16)
#define AES_NR     10
#define AES_NK     4
#else
#error "AES_KEYLEN must be 16, 24, or 32"
#endif

#define AES_KEYEXPSIZE (4 * (AES_NR + 1) * 4)

typedef struct {
    uint8_t RoundKey[AES_KEYEXPSIZE];
    uint8_t Iv[AES_BLOCKLEN];
} AES_ctx;

/// 初始化上下文 (无 IV, 仅 ECB 模式可用)
void AES_init_ctx(AES_ctx* ctx, const uint8_t* key);

/// 初始化上下文 + IV (CBC/CTR 模式必需)
void AES_init_ctx_iv(AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);

/// 重置 IV (CBC 模式重复使用同 ctx 时调用)
void AES_ctx_set_iv(AES_ctx* ctx, const uint8_t* iv);

/// ECB: 单块原地加密 (16 字节)
void AES_ECB_encrypt(const AES_ctx* ctx, uint8_t* buf);

/// ECB: 单块原地解密 (16 字节)
void AES_ECB_decrypt(const AES_ctx* ctx, uint8_t* buf);

/// CBC: 整块原地加密, length 必须为 16 倍数
void AES_CBC_encrypt_buffer(AES_ctx* ctx, uint8_t* buf, size_t length);

/// CBC: 整块原地解密, length 必须为 16 倍数
void AES_CBC_decrypt_buffer(AES_ctx* ctx, uint8_t* buf, size_t length);

#ifdef __cplusplus
}
#endif

#endif // TINY_AES_H
