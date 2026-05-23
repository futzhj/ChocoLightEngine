#include "resource_package.h"

#include "miniz.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LT::Resource {
namespace {

std::string Dirname(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string Stem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) dot = path.size();
    return path.substr(start, dot - start);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") return name;
    char tail = dir[dir.size() - 1];
    if (tail == '/' || tail == '\\') return dir + name;
#if defined(_WIN32)
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

std::string LowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

 constexpr uint16_t kWpkMagicAc = 0x4341u;
 constexpr uint16_t kWpkMagicXc = 0x4358u;
 constexpr uint16_t kWpkMagicNc = 0x434Eu;
 constexpr uint32_t kNeoxMagicZstd = 0x5A535444u;
 constexpr uint32_t kNeoxMagicZlib = 0x5A4C4942u;
 constexpr uint32_t kNeoxMagicZlia = 0x5A4C4941u;
 constexpr uint32_t kNeoxMagicLz4f = 0x4C5A3446u;
 constexpr uint32_t kNeoxMagicNone = 0x4E4F4E45u;
 constexpr size_t kWpkDecodeCapLimit = 256u * 1024u * 1024u;

 struct Aes128Ctx {
     std::array<uint8_t, 176> roundKey{};
 };

 using AesState = uint8_t[4][4];

 const uint8_t kAesSBox[256] = {
     0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
     0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
     0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
     0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
     0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
     0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
     0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
     0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
     0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
     0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
     0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
     0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
     0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
     0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
     0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
     0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
 };

 const uint8_t kAesInvSBox[256] = {
     0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
     0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
     0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
     0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
     0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
     0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
     0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
     0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
     0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
     0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
     0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
     0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
     0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
     0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
     0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
     0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
 };

 const uint8_t kAesRcon[11] = {
     0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
 };

 uint8_t AesXTime(uint8_t x) {
     return static_cast<uint8_t>((x << 1) ^ (((x >> 7) & 1u) * 0x1Bu));
 }

 uint8_t AesMultiply(uint8_t x, uint8_t y) {
     return static_cast<uint8_t>(((y & 1u) * x)
         ^ (((y >> 1u) & 1u) * AesXTime(x))
         ^ (((y >> 2u) & 1u) * AesXTime(AesXTime(x)))
         ^ (((y >> 3u) & 1u) * AesXTime(AesXTime(AesXTime(x))))
         ^ (((y >> 4u) & 1u) * AesXTime(AesXTime(AesXTime(AesXTime(x))))));
 }

 void AesAddRoundKey(uint8_t round, AesState* state, const uint8_t* roundKey) {
     for (uint8_t i = 0; i < 4; ++i) {
         for (uint8_t j = 0; j < 4; ++j) {
             (*state)[i][j] ^= roundKey[(round * 16u) + (i * 4u) + j];
         }
     }
 }

 void AesInvSubBytes(AesState* state) {
     for (uint8_t i = 0; i < 4; ++i) {
         for (uint8_t j = 0; j < 4; ++j) {
             (*state)[j][i] = kAesInvSBox[(*state)[j][i]];
         }
     }
 }

 void AesInvShiftRows(AesState* state) {
     uint8_t temp = (*state)[3][1];
     (*state)[3][1] = (*state)[2][1];
     (*state)[2][1] = (*state)[1][1];
     (*state)[1][1] = (*state)[0][1];
     (*state)[0][1] = temp;
     temp = (*state)[0][2];
     (*state)[0][2] = (*state)[2][2];
     (*state)[2][2] = temp;
     temp = (*state)[1][2];
     (*state)[1][2] = (*state)[3][2];
     (*state)[3][2] = temp;
     temp = (*state)[0][3];
     (*state)[0][3] = (*state)[1][3];
     (*state)[1][3] = (*state)[2][3];
     (*state)[2][3] = (*state)[3][3];
     (*state)[3][3] = temp;
 }

 void AesInvMixColumns(AesState* state) {
     for (int i = 0; i < 4; ++i) {
         const uint8_t a = (*state)[i][0];
         const uint8_t b = (*state)[i][1];
         const uint8_t c = (*state)[i][2];
         const uint8_t d = (*state)[i][3];
         (*state)[i][0] = static_cast<uint8_t>(AesMultiply(a, 0x0E) ^ AesMultiply(b, 0x0B) ^ AesMultiply(c, 0x0D) ^ AesMultiply(d, 0x09));
         (*state)[i][1] = static_cast<uint8_t>(AesMultiply(a, 0x09) ^ AesMultiply(b, 0x0E) ^ AesMultiply(c, 0x0B) ^ AesMultiply(d, 0x0D));
         (*state)[i][2] = static_cast<uint8_t>(AesMultiply(a, 0x0D) ^ AesMultiply(b, 0x09) ^ AesMultiply(c, 0x0E) ^ AesMultiply(d, 0x0B));
         (*state)[i][3] = static_cast<uint8_t>(AesMultiply(a, 0x0B) ^ AesMultiply(b, 0x0D) ^ AesMultiply(c, 0x09) ^ AesMultiply(d, 0x0E));
     }
 }

 void Aes128KeyExpansion(const uint8_t key[16], uint8_t roundKey[176]) {
     for (int i = 0; i < 16; ++i) {
         roundKey[i] = key[i];
     }
     for (int i = 4; i < 44; ++i) {
         uint8_t temp[4] = {
             roundKey[(i - 1) * 4 + 0],
             roundKey[(i - 1) * 4 + 1],
             roundKey[(i - 1) * 4 + 2],
             roundKey[(i - 1) * 4 + 3]
         };
         if ((i % 4) == 0) {
             const uint8_t t = temp[0];
             temp[0] = kAesSBox[temp[1]];
             temp[1] = kAesSBox[temp[2]];
             temp[2] = kAesSBox[temp[3]];
             temp[3] = kAesSBox[t];
             temp[0] ^= kAesRcon[i / 4];
         }
         roundKey[i * 4 + 0] = static_cast<uint8_t>(roundKey[(i - 4) * 4 + 0] ^ temp[0]);
         roundKey[i * 4 + 1] = static_cast<uint8_t>(roundKey[(i - 4) * 4 + 1] ^ temp[1]);
         roundKey[i * 4 + 2] = static_cast<uint8_t>(roundKey[(i - 4) * 4 + 2] ^ temp[2]);
         roundKey[i * 4 + 3] = static_cast<uint8_t>(roundKey[(i - 4) * 4 + 3] ^ temp[3]);
     }
 }

 void Aes128Init(Aes128Ctx& ctx, const uint8_t key[16]) {
     Aes128KeyExpansion(key, ctx.roundKey.data());
 }

 void Aes128DecryptBlock(const Aes128Ctx& ctx, uint8_t block[16]) {
     auto* state = reinterpret_cast<AesState*>(block);
     AesAddRoundKey(10, state, ctx.roundKey.data());
     for (int round = 9; round >= 1; --round) {
         AesInvShiftRows(state);
         AesInvSubBytes(state);
         AesAddRoundKey(static_cast<uint8_t>(round), state, ctx.roundKey.data());
         AesInvMixColumns(state);
     }
     AesInvShiftRows(state);
     AesInvSubBytes(state);
     AesAddRoundKey(0, state, ctx.roundKey.data());
 }

 bool LooksLikeGzipMagic(const uint8_t* p, size_t n) {
     return n >= 2 && p[0] == 0x1F && p[1] == 0x8B;
 }

 bool LooksLikeZlibHeader(const uint8_t* p, size_t n) {
     if (n < 2) return false;
     const uint8_t cmf = p[0];
     const uint8_t flg = p[1];
     if ((cmf & 0x0F) != 8) return false;
     const uint32_t chk = (static_cast<uint32_t>(cmf) << 8) | static_cast<uint32_t>(flg);
     if ((chk % 31u) != 0u) return false;
     if ((flg & 0x20) != 0 && n < 6) return false;
     return true;
 }

 bool TryInflateWithWindowBits(const uint8_t* src, size_t srcSize, int windowBits, std::string& out) {
     if (!src || srcSize == 0) return false;
     mz_stream strm{};
     strm.next_in = reinterpret_cast<const unsigned char*>(src);
     strm.avail_in = static_cast<unsigned int>(std::min<size_t>(srcSize, 0xFFFFFFFFu));
     if (mz_inflateInit2(&strm, windowBits) != MZ_OK) return false;

     size_t outCap = srcSize * 8u;
     if (outCap < 65536u) outCap = 65536u;
     if (outCap > kWpkDecodeCapLimit) outCap = kWpkDecodeCapLimit;
     std::vector<unsigned char> outBuf(outCap ? outCap : 1u);
     bool ok = false;
     int safety = 0;

     while (true) {
         const size_t outSize = static_cast<size_t>(strm.total_out);
         if (outSize >= outBuf.size()) {
             if (outBuf.size() >= kWpkDecodeCapLimit) break;
             size_t newCap = outBuf.size() * 2u;
             if (newCap < outBuf.size() || newCap > kWpkDecodeCapLimit) newCap = kWpkDecodeCapLimit;
             if (newCap <= outBuf.size()) break;
             outBuf.resize(newCap);
         }
         const size_t outSizeNow = static_cast<size_t>(strm.total_out);
         strm.next_out = outBuf.data() + outSizeNow;
         size_t availOut = outBuf.size() - outSizeNow;
         if (availOut > 0xFFFFFFFFu) availOut = 0xFFFFFFFFu;
         strm.avail_out = static_cast<unsigned int>(availOut);

         const int ret = mz_inflate(&strm, MZ_SYNC_FLUSH);
         if (ret == MZ_STREAM_END) {
             ok = true;
             break;
         }
         if (ret != MZ_OK && ret != MZ_BUF_ERROR) break;
         if (strm.avail_out != 0 && ret == MZ_BUF_ERROR) break;
         if (++safety > 2048) break;
     }

     mz_inflateEnd(&strm);
     if (!ok) return false;
     out.assign(reinterpret_cast<const char*>(outBuf.data()), static_cast<size_t>(strm.total_out));
     return true;
 }

 bool TryZlibDecompress(const uint8_t* src, size_t srcSize, std::string& out) {
     if (!LooksLikeZlibHeader(src, srcSize) && !LooksLikeGzipMagic(src, srcSize)) return false;
     if (TryInflateWithWindowBits(src, srcSize, 15 + 32, out)) return true;
     return TryInflateWithWindowBits(src, srcSize, -15, out);
 }

 void GenerateAesKeyFromHeader(const uint8_t* in, size_t inSize, uint8_t outKey[16]) {
     const uint32_t dataSize = static_cast<uint32_t>(inSize - 8u);
     outKey[0] = static_cast<uint8_t>(dataSize % 0xFDu);
     outKey[1] = static_cast<uint8_t>(in[3] + dataSize);
     outKey[2] = static_cast<uint8_t>((dataSize >> 8) & 0xFFu);
     outKey[3] = static_cast<uint8_t>((dataSize >> 16) & 0xFFu);
     outKey[4] = 0x6Au;
     outKey[5] = 0x6Bu;
     outKey[6] = 0x2Eu;
     outKey[7] = 0x7Cu;
     outKey[8] = 0x30u;
     outKey[9] = 0x36u;
     outKey[10] = static_cast<uint8_t>(outKey[1] ^ 0x33u);
     outKey[11] = static_cast<uint8_t>(outKey[1] | 0x2Eu);
     outKey[12] = 0x6Eu;
     outKey[13] = 0x65u;
     outKey[14] = 0x74u;
     outKey[15] = 0x5Cu;
 }

 void GenerateNcKeyFromHeader(const uint8_t* in, size_t inSize, uint8_t outKey[16]) {
     uint32_t s = static_cast<uint32_t>(inSize - 8u);
     s ^= ReadLE32(in + 3);
     s ^= (static_cast<uint32_t>(in[7]) << 11);
     s ^= 0x9E3779B9u;
     for (int i = 0; i < 16; ++i) {
         s ^= s << 13;
         s ^= s >> 17;
         s ^= s << 5;
         outKey[i] = static_cast<uint8_t>(s & 0xFFu);
         s += 0x7F4A7C15u + static_cast<uint32_t>(i) * 0x85EBCA6Bu;
     }
 }

 void XorDecryptAc(uint8_t* buf, uint32_t dataSize, uint32_t actualSize, uint32_t blockSize, const uint8_t key[16]) {
     if (!buf) return;
     if (blockSize > dataSize) blockSize = dataSize;
     if (actualSize == 0) return;
     if (actualSize > blockSize) actualSize = blockSize;
     if (actualSize >= blockSize) return;
     const uint32_t tailCount = blockSize - actualSize;
     const uint8_t k = key[1];
     uint8_t* dst = buf + actualSize;
     for (uint32_t i = 0; i < tailCount; ++i) {
         dst[i] = static_cast<uint8_t>(dst[i] ^ static_cast<uint8_t>(k + static_cast<uint8_t>(i) + buf[i]));
     }
 }

 void XorDecryptXc(uint8_t* buf, uint32_t dataSize, uint32_t blockSize) {
     if (!buf) return;
     if (blockSize > dataSize) blockSize = dataSize;
     uint8_t xorKey[128];
     for (int i = 0; i < 128; ++i) {
         xorKey[i] = static_cast<uint8_t>(dataSize + static_cast<uint32_t>(i));
     }
     for (uint32_t i = 0; i < blockSize; ++i) {
         buf[i] = static_cast<uint8_t>(buf[i] ^ xorKey[i & 0x7Fu]);
     }
 }

 void DeobfuscateNc(uint8_t* data, size_t n, const uint8_t* header) {
     if (!data || !header || n == 0) return;
     size_t block = 64u + static_cast<size_t>(header[3] & 0x1Fu);
     if (block > n) block = n;
     if (block == 0) return;
     if (block > 96u) block = 96u;
     const uint8_t k = static_cast<uint8_t>(0xA5u ^ header[4] ^ header[7]);
     uint8_t tmp[96]{};
     for (size_t j = 0; j < block; ++j) {
         tmp[j] = static_cast<uint8_t>(data[block - 1u - j] ^ k);
     }
     for (size_t j = 0; j < block; ++j) {
         data[j] = tmp[j];
     }
 }

 void DeobfuscateXor5AReverse64(uint8_t* data, size_t n) {
     if (!data || n == 0) return;
     const size_t block = std::min<size_t>(n, 64u);
     uint8_t tmp[64]{};
     for (size_t j = 0; j < block; ++j) {
         tmp[j] = static_cast<uint8_t>(data[block - 1u - j] ^ 0x5Au);
     }
     for (size_t j = 0; j < block; ++j) {
         data[j] = tmp[j];
     }
 }

 bool TryDecodeNeox(const std::string& src, std::string& out) {
     if (src.size() < 4) return false;
     const auto* p = reinterpret_cast<const uint8_t*>(src.data());
     const uint32_t magic = ReadLE32(p);
     if (magic == kNeoxMagicNone) {
         if (src.size() <= 4) return false;
         if (src.size() >= 8) {
             const uint32_t maybeSize = ReadLE32(p + 4);
             if (maybeSize == static_cast<uint32_t>(src.size() - 8u)) {
                 out.assign(src.data() + 8, src.size() - 8u);
                 return true;
             }
         }
         out.assign(src.data() + 4, src.size() - 4u);
         return true;
     }
     if (magic == kNeoxMagicZlib || magic == kNeoxMagicZlia) {
         if (src.size() <= 4) return false;
         if (TryZlibDecompress(p + 4, src.size() - 4u, out)) return true;
         if (src.size() > 8) return TryZlibDecompress(p + 8, src.size() - 8u, out);
         return false;
     }
     if (magic == kNeoxMagicZstd || magic == kNeoxMagicLz4f) {
         return false;
     }
     return false;
 }

 bool TryDecodeAcXc(const std::string& input, std::string& out) {
     if (input.size() < 10) return false;
     const auto* in = reinterpret_cast<const uint8_t*>(input.data());
     const uint16_t m = ReadLE16(in);
     if (m != kWpkMagicAc && m != kWpkMagicXc && m != kWpkMagicNc) return false;

     const uint32_t dataSize = static_cast<uint32_t>(input.size() - 8u);
     const uint32_t factor = static_cast<uint32_t>(in[2]);
     uint32_t blockSize = 0;
     if (factor > 0 && factor < 25u) {
         blockSize = 128u << (factor - 1u);
     } else if (factor >= 25u) {
         blockSize = dataSize;
     }
     if (blockSize > dataSize) blockSize = dataSize;
     const uint32_t actualSize = blockSize & 0xFFFFFFF0u;

     std::string dec(input.data() + 8, dataSize);
     if (dec.empty()) {
         out.clear();
         return true;
     }

     if (m == kWpkMagicAc || m == kWpkMagicNc) {
         uint8_t key[16]{};
         if (m == kWpkMagicNc) GenerateNcKeyFromHeader(in, input.size(), key);
         else GenerateAesKeyFromHeader(in, input.size(), key);
         if (actualSize != 0) {
             Aes128Ctx ctx;
             Aes128Init(ctx, key);
             for (uint32_t off = 0; off + 16u <= actualSize; off += 16u) {
                 Aes128DecryptBlock(ctx, reinterpret_cast<uint8_t*>(&dec[off]));
             }
         }
         XorDecryptAc(reinterpret_cast<uint8_t*>(&dec[0]), dataSize, actualSize, blockSize, key);
     } else {
         XorDecryptXc(reinterpret_cast<uint8_t*>(&dec[0]), dataSize, blockSize);
     }

     if (m == kWpkMagicNc) DeobfuscateNc(reinterpret_cast<uint8_t*>(&dec[0]), dec.size(), in);
     DeobfuscateXor5AReverse64(reinterpret_cast<uint8_t*>(&dec[0]), dec.size());

     std::string decoded;
     if (TryDecodeNeox(dec, decoded)) {
         out.swap(decoded);
         return true;
     }
     out.swap(dec);
     return true;
 }

class WpkPackage final : public IResourcePackage {
public:
    explicit WpkPackage(std::string path)
        : path_(std::move(path)) {}

    // SKPE -> SKPW 解密：去 4 字节 "SKPE" magic，剩余整段 reverse + XOR 0x5A
    // 参考：E:\jinyiNew\GGELUA_SDL3\deps\Sources\grr\mygxy\wpk.c:2870-2894
    static bool DecryptSkpeToSkpw(const std::string& body, std::string& out) {
        const size_t n = body.size();
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<char>(static_cast<uint8_t>(body[n - 1 - i]) ^ 0x5A);
        }
        return true;
    }

    bool Init(std::string& err) {
        FILE* fp = std::fopen(path_.c_str(), "rb");
        if (!fp) {
            err = std::string("open failed: ") + path_;
            return false;
        }
        struct FileCloser {
            FILE* fp;
            ~FileCloser() { if (fp) std::fclose(fp); }
        } close{fp};

        uint64_t fileSize = 0;
        if (!GetFileSize(fp, fileSize)) {
            err = "failed to determine IDX size";
            return false;
        }
        if (fileSize < 4) {
            err = "IDX too small";
            return false;
        }

        // 读前 4 字节探外层 magic
        std::string magic;
        if (!ReadAt(fp, 0, 4, magic, err)) return false;

        // plain 持有真正的 SKPW 数据：
        //   SKPW 路径 → 原内容直读
        //   SKPE 路径 → 解密结果（reverse + XOR 0x5A）
        std::string plain;
        std::string subtype;

        if (magic == "SKPE") {
            if (fileSize <= 4) {
                err = "SKPE too small";
                return false;
            }
            std::string body;
            if (!ReadAt(fp, 4, fileSize - 4, body, err)) return false;
            if (!DecryptSkpeToSkpw(body, plain)) {
                err = "SKPE decrypt failed";
                return false;
            }
            subtype = "SKPE";
        } else if (magic == "SKPW") {
            if (fileSize < 32) {
                err = "IDX too small";
                return false;
            }
            if (!ReadAt(fp, 0, fileSize, plain, err)) return false;
            subtype = "SKPW";
        } else {
            err = "invalid IDX magic";
            return false;
        }

        // 校验解密结果（SKPE 解出后必须是 SKPW）
        if (plain.size() < 32) {
            err = "IDX too small after decode";
            return false;
        }
        if (plain.substr(0, 4) != "SKPW") {
            err = "decoded magic is not SKPW: " + plain.substr(0, 4);
            return false;
        }

        const auto* h = reinterpret_cast<const uint8_t*>(plain.data());
        uint32_t count = ReadLE32(h + 12);
        uint64_t recordsOffset = 32;
        uint64_t recordsBytes = static_cast<uint64_t>(count) * 28u;
        if (plain.size() < recordsOffset + recordsBytes) {
            err = "invalid IDX record range";
            return false;
        }

        info_.kind = "WPK";
        info_.subtype = subtype;
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = recordsOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(plain.data() + recordsOffset + static_cast<size_t>(i) * 28u);
            ResourceEntry e;
            e.md5 = HexLower(p, 16);
            e.size = ReadLE32(p + 16);
            e.packedSize = e.size;
            e.offset = ReadLE32(p + 20);
            e.archive = ReadLE16(p + 24);
            e.keyText = e.md5;

            keyToIndex_[e.keyText] = entries_.size();
            entries_.push_back(e);
        }
        return true;
    }

    const PackageInfo& Info() const override { return info_; }
    const std::vector<ResourceEntry>& Entries() const override { return entries_; }

    bool Has(const std::string& key) const override {
        return keyToIndex_.find(LowerAscii(key)) != keyToIndex_.end();
    }

    bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) override {
        auto it = keyToIndex_.find(LowerAscii(key));
        if (it == keyToIndex_.end()) {
            err = "entry not found";
            return false;
        }

        const ResourceEntry& e = entries_[it->second];
        if (e.archive == 0xFF || e.archive == 0xFFFF) {
            err = "external WPK entry is not stored in archive";
            return false;
        }
        if (opts.maxBytes > 0 && e.size > opts.maxBytes) {
            err = "entry exceeds maxBytes";
            return false;
        }

        std::string archiveName = Stem(path_) + std::to_string(e.archive) + ".wpk";
        std::string archivePath = JoinPath(Dirname(path_), archiveName);
        FILE* fp = std::fopen(archivePath.c_str(), "rb");
        if (!fp) {
            err = std::string("open WPK archive failed: ") + archivePath;
            return false;
        }
        struct FileCloser {
            FILE* fp;
            ~FileCloser() { if (fp) std::fclose(fp); }
        } close{fp};

        std::string raw;
        if (!ReadAt(fp, e.offset, e.size, raw, err)) return false;
        if (opts.raw || !opts.decode) {
            out.swap(raw);
            return true;
        }

        std::string decoded;
        if (TryDecodeAcXc(raw, decoded)) {
            out.swap(decoded);
            return true;
        }

        out.swap(raw);
        return true;
    }

private:
    std::string path_;
    PackageInfo info_;
    std::vector<ResourceEntry> entries_;
    std::unordered_map<std::string, size_t> keyToIndex_;
};

} // namespace

std::unique_ptr<IResourcePackage> OpenWpkPackage(const std::string& path, std::string& err) {
    auto pkg = std::make_unique<WpkPackage>(path);
    if (!pkg->Init(err)) return nullptr;
    return pkg;
}

} // namespace LT::Resource
