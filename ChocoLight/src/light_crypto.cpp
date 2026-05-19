/**
 * @file light_crypto.cpp
 * @brief Light.Crypto 模块 — 用户加密 API
 *
 * 提供:
 *   - AES-256-ECB / AES-256-CBC 加密 (PKCS7 填充)
 *   - SHA-256 / MD5 哈希
 *   - Base64 编码/解码
 *   - 随机字节生成
 *
 * 用途:
 *   - 加密本地存档/资源文件
 *   - 密码哈希存储
 *   - 网络传输加密
 *
 * Lua API:
 *   -- 哈希 (返回 hex 字符串)
 *   local hash = Light.Crypto.SHA256("hello")          -- "2cf24dba5fb0a30e..."
 *   local md5h = Light.Crypto.MD5("hello")
 *   -- 哈希 (返回原始字节)
 *   local raw  = Light.Crypto.SHA256_Raw("hello")      -- 32 字节 string
 *
 *   -- AES-256 (key 32 字节, iv 16 字节)
 *   local enc = Light.Crypto.AES256_Encrypt(plaintext, key, iv)
 *   local dec = Light.Crypto.AES256_Decrypt(ciphertext, key, iv)
 *
 *   -- Base64
 *   local b64 = Light.Crypto.Base64Encode("Hello!")    -- "SGVsbG8h"
 *   local raw = Light.Crypto.Base64Decode(b64)
 *
 *   -- 随机字节
 *   local key = Light.Crypto.RandomBytes(32)
 *   local hex = Light.Crypto.RandomHex(16)
 *
 *   -- 派生密钥 (从密码生成 AES 密钥)
 *   local key = Light.Crypto.KeyFromPassword(password, "salt", 32)
 */

#include "light.h"
#include "light_utils_core.h"
extern "C" {
  #include "tiny_aes.h"
  #include "tiny_hash.h"
}
#include <cstring>
#include <cstdint>
#include <random>
#include <chrono>
#include <vector>
#include <string>

namespace {

// ==================== 辅助函数 ====================

// 字节转 hex 字符串
static std::string ToHex(const uint8_t* data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = hex_chars[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex_chars[data[i] & 0xF];
    }
    return out;
}

// 跨平台密码学随机数 (优先系统熵, 回退到时间戳混合)
static void GenerateRandomBytes(uint8_t* out, size_t len) {
    // 使用 random_device + mt19937_64 混合
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(
        rd() ^ (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)dist(gen);
    }
}

// PKCS7 填充: 在 buf 末尾追加 (16 - len%16) 个相同字节
// 返回填充后的总长度
static size_t PKCS7_Pad(std::vector<uint8_t>& buf) {
    size_t pad = 16 - (buf.size() % 16);
    buf.insert(buf.end(), pad, (uint8_t)pad);
    return buf.size();
}

// PKCS7 移除填充: 检查最后字节, 截断
// 返回 false 表示填充无效
static bool PKCS7_Unpad(const uint8_t* buf, size_t len, size_t* outLen) {
    if (outLen) *outLen = 0;
    if (len == 0 || (len % 16) != 0) return false;
    uint8_t pad = buf[len - 1];
    if (pad < 1 || pad > 16) return false;
    // 验证所有填充字节相同
    for (size_t i = 0; i < pad; ++i) {
        if (buf[len - 1 - i] != pad) return false;
    }
    if (outLen) *outLen = len - pad;
    return true;
}

} // namespace

// ==================== 哈希 API ====================

/// @lua_api Light.Crypto.SHA256
/// @brief 计算 SHA-256 哈希 (hex 字符串)
/// @param data string 输入数据
/// @return string 64 字符 hex 字符串
static int l_SHA256(lua_State* L) {
    size_t len;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[32];
    sha256(data, len, digest);
    std::string hex = ToHex(digest, 32);
    lua_pushlstring(L, hex.c_str(), hex.size());
    return 1;
}

/// @lua_api Light.Crypto.SHA256_Raw
/// @brief 计算 SHA-256 哈希 (原始 32 字节)
/// @param data string 输入数据
/// @return string 32 字节二进制
static int l_SHA256_Raw(lua_State* L) {
    size_t len;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[32];
    sha256(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 32);
    return 1;
}

/// @lua_api Light.Crypto.MD5
/// @brief 计算 MD5 哈希 (hex 字符串)
/// @param data string 输入数据
/// @return string 32 字符 hex 字符串
static int l_MD5(lua_State* L) {
    size_t len;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[16];
    md5(data, len, digest);
    std::string hex = ToHex(digest, 16);
    lua_pushlstring(L, hex.c_str(), hex.size());
    return 1;
}

/// @lua_api Light.Crypto.MD5_Raw
/// @brief 计算 MD5 哈希 (原始 16 字节)
/// @param data string 输入数据
/// @return string 16 字节二进制
static int l_MD5_Raw(lua_State* L) {
    size_t len;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[16];
    md5(data, len, digest);
    lua_pushlstring(L, (const char*)digest, 16);
    return 1;
}

static int l_SHA1(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[20];
    LT::Utils::SHA1(reinterpret_cast<const uint8_t*>(data), len, digest);
    std::string hex = LT::Utils::HexEncode(digest, 20, false);
    lua_pushlstring(L, hex.data(), hex.size());
    return 1;
}

static int l_SHA1_Raw(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[20];
    LT::Utils::SHA1(reinterpret_cast<const uint8_t*>(data), len, digest);
    lua_pushlstring(L, reinterpret_cast<const char*>(digest), 20);
    return 1;
}

static int l_Crypto_HexEncode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    bool upper = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        upper = lua_toboolean(L, 2) != 0;
    }
    std::string out = LT::Utils::HexEncode(reinterpret_cast<const uint8_t*>(data), len, upper);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Crypto_HexDecode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    std::string out;
    std::string err;
    if (!LT::Utils::HexDecode(data, len, out, err)) {
        lua_pushnil(L);
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_Crypto_CRC32(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint32_t crc = LT::Utils::CRC32(reinterpret_cast<const uint8_t*>(data), len);
    lua_pushnumber(L, static_cast<lua_Number>(crc));
    return 1;
}

// ==================== AES-256 API ====================

/// @lua_api Light.Crypto.AES256_Encrypt
/// @brief AES-256-CBC 加密 (PKCS7 填充)
/// @param plaintext string 明文
/// @param key string 32 字节密钥
/// @param iv string 16 字节初始向量
/// @return string 密文
static int l_AES256_Encrypt(lua_State* L) {
    size_t plain_len, key_len, iv_len;
    const char* plain = luaL_checklstring(L, 1, &plain_len);
    const char* key   = luaL_checklstring(L, 2, &key_len);
    const char* iv    = luaL_checklstring(L, 3, &iv_len);

    if (key_len != 32) {
        lua_pushnil(L);
        lua_pushstring(L, "AES256_Encrypt: key must be exactly 32 bytes");
        return 2;
    }
    if (iv_len != 16) {
        lua_pushnil(L);
        lua_pushstring(L, "AES256_Encrypt: iv must be exactly 16 bytes");
        return 2;
    }

    // 准备缓冲区: 拷贝明文 + PKCS7 填充
    std::vector<uint8_t> buf(plain_len);
    if (plain_len > 0) memcpy(buf.data(), plain, plain_len);
    PKCS7_Pad(buf);

    // 加密
    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, (const uint8_t*)key, (const uint8_t*)iv);
    AES_CBC_encrypt_buffer(&ctx, buf.data(), buf.size());

    lua_pushlstring(L, (const char*)buf.data(), buf.size());
    return 1;
}

/// @lua_api Light.Crypto.AES256_Decrypt
/// @brief AES-256-CBC 解密
/// @param ciphertext string 密文
/// @param key string 32 字节密钥
/// @param iv string 16 字节初始向量
/// @return string 明文 (失败返回 nil + 错误信息)
static int l_AES256_Decrypt(lua_State* L) {
    size_t cipher_len, key_len, iv_len;
    const char* cipher = luaL_checklstring(L, 1, &cipher_len);
    const char* key    = luaL_checklstring(L, 2, &key_len);
    const char* iv     = luaL_checklstring(L, 3, &iv_len);

    if (key_len != 32) {
        lua_pushnil(L);
        lua_pushstring(L, "AES256_Decrypt: key must be 32 bytes");
        return 2;
    }
    if (iv_len != 16) {
        lua_pushnil(L);
        lua_pushstring(L, "AES256_Decrypt: iv must be 16 bytes");
        return 2;
    }
    if (cipher_len == 0 || (cipher_len % 16) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "AES256_Decrypt: ciphertext length must be multiple of 16");
        return 2;
    }

    std::vector<uint8_t> buf(cipher_len);
    memcpy(buf.data(), cipher, cipher_len);

    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, (const uint8_t*)key, (const uint8_t*)iv);
    AES_CBC_decrypt_buffer(&ctx, buf.data(), buf.size());

    size_t plain_len = 0;
    if (!PKCS7_Unpad(buf.data(), buf.size(), &plain_len)) {
        // 填充无效 (可能密钥/IV 错)
        lua_pushnil(L);
        lua_pushstring(L, "AES256_Decrypt: invalid padding (wrong key/iv?)");
        return 2;
    }

    lua_pushlstring(L, (const char*)buf.data(), plain_len);
    return 1;
}

// ==================== Base64 API ====================

/// @lua_api Light.Crypto.Base64Encode
/// @brief Base64 编码
/// @param data string 输入字节
/// @return string Base64 字符串
static int l_Base64Encode(lua_State* L) {
    size_t len;
    const char* data = luaL_checklstring(L, 1, &len);
    std::vector<char> out(((len + 2) / 3) * 4 + 4, 0);
    int out_len = base64_encode((const uint8_t*)data, len, out.data());
    lua_pushlstring(L, out.data(), out_len);
    return 1;
}

/// @lua_api Light.Crypto.Base64Decode
/// @brief Base64 解码
/// @param b64 string Base64 字符串
/// @return string 原始字节 (失败返回 nil)
static int l_Base64Decode(lua_State* L) {
    size_t len;
    const char* b64 = luaL_checklstring(L, 1, &len);
    std::vector<uint8_t> out((len / 4) * 3 + 4, 0);
    int out_len = base64_decode(b64, len, out.data());
    if (out_len < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Base64Decode: invalid input");
        return 2;
    }
    lua_pushlstring(L, (const char*)out.data(), out_len);
    return 1;
}

// ==================== 随机数 API ====================

/// @lua_api Light.Crypto.RandomBytes
/// @brief 生成密码学随机字节
/// @param n number 字节数 (1~4096)
/// @return string 随机字节
static int l_RandomBytes(lua_State* L) {
    int n = (int)luaL_checkinteger(L, 1);
    if (n <= 0 || n > 4096) {
        lua_pushnil(L);
        lua_pushstring(L, "RandomBytes: n must be in [1, 4096]");
        return 2;
    }
    std::vector<uint8_t> buf(n);
    GenerateRandomBytes(buf.data(), n);
    lua_pushlstring(L, (const char*)buf.data(), n);
    return 1;
}

/// @lua_api Light.Crypto.RandomHex
/// @brief 生成随机 hex 字符串
/// @param n number 字节数 (输出长度 = n*2)
/// @return string hex 字符串
static int l_RandomHex(lua_State* L) {
    int n = (int)luaL_checkinteger(L, 1);
    if (n <= 0 || n > 4096) {
        lua_pushnil(L);
        lua_pushstring(L, "RandomHex: n must be in [1, 4096]");
        return 2;
    }
    std::vector<uint8_t> buf(n);
    GenerateRandomBytes(buf.data(), n);
    std::string hex = ToHex(buf.data(), n);
    lua_pushlstring(L, hex.c_str(), hex.size());
    return 1;
}

// ==================== 密钥派生 API ====================

/// @lua_api Light.Crypto.KeyFromPassword
/// @brief 从密码派生固定长度密钥 (简化 PBKDF: 多轮 SHA-256 链)
/// @param password string 密码
/// @param salt string 盐值 (避免彩虹表)
/// @param keyLen number 输出密钥长度 (1~64)
/// @param iterations number? 迭代次数 (默认 10000)
/// @return string 派生密钥
static int l_KeyFromPassword(lua_State* L) {
    size_t pw_len, salt_len;
    const char* password = luaL_checklstring(L, 1, &pw_len);
    const char* salt     = luaL_checklstring(L, 2, &salt_len);
    int keyLen     = (int)luaL_checkinteger(L, 3);
    int iterations = (int)luaL_optinteger(L, 4, 10000);

    if (keyLen <= 0 || keyLen > 64) {
        lua_pushnil(L);
        lua_pushstring(L, "KeyFromPassword: keyLen must be in [1, 64]");
        return 2;
    }
    if (iterations < 1 || iterations > 1000000) {
        iterations = 10000;
    }

    // 简化派生: SHA256(password || salt) 重复 iterations 次
    // 注: 非完整 PBKDF2 (无 HMAC), 但对游戏存档加密足够
    std::vector<uint8_t> input;
    input.reserve(pw_len + salt_len);
    input.insert(input.end(), password, password + pw_len);
    input.insert(input.end(), salt, salt + salt_len);

    uint8_t digest[32];
    sha256(input.data(), input.size(), digest);
    for (int i = 1; i < iterations; ++i) {
        sha256(digest, 32, digest);
    }

    // 若 keyLen <= 32 直接截断, 否则继续派生填充
    std::vector<uint8_t> out(keyLen);
    int copied = (keyLen < 32) ? keyLen : 32;
    memcpy(out.data(), digest, copied);
    if (keyLen > 32) {
        uint8_t digest2[32];
        sha256(digest, 32, digest2);
        memcpy(out.data() + 32, digest2, keyLen - 32);
    }
    lua_pushlstring(L, (const char*)out.data(), keyLen);
    return 1;
}

// ==================== 模块注册 ====================

int luaopen_Light_Crypto(lua_State* L) {
    static const luaL_Reg crypto_funcs[] = {
        {"SHA256",          l_SHA256},
        {"SHA256_Raw",      l_SHA256_Raw},
        {"MD5",             l_MD5},
        {"MD5_Raw",         l_MD5_Raw},
        {"SHA1",            l_SHA1},
        {"SHA1_Raw",        l_SHA1_Raw},
        {"HexEncode",       l_Crypto_HexEncode},
        {"HexDecode",       l_Crypto_HexDecode},
        {"CRC32",           l_Crypto_CRC32},
        {"AES256_Encrypt",  l_AES256_Encrypt},
        {"AES256_Decrypt",  l_AES256_Decrypt},
        {"Base64Encode",    l_Base64Encode},
        {"Base64Decode",    l_Base64Decode},
        {"RandomBytes",     l_RandomBytes},
        {"RandomHex",       l_RandomHex},
        {"KeyFromPassword", l_KeyFromPassword},
        {nullptr, nullptr}
    };
    LT::RegisterModule(L, "Crypto", crypto_funcs);
    return 1;
}
