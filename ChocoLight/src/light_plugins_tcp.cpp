#include "resource_package.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t LT_MAGIC_TCP = LT::Magic4('T', 'C', 'P', 'H');
constexpr const char* MT_TCP = "Light.Plugins.TCP.Handle";

struct RGB {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct FrameInfo {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::string rgba;
};

struct TcpSprite {
    std::string path;
    std::vector<uint8_t> data;
    std::vector<RGB> palette;
    std::vector<uint32_t> offsets;
    uint16_t directions = 0;
    uint16_t framesPerDirection = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t x = 0;
    int16_t y = 0;
};

struct TcpHandle {
    uint32_t magic = LT_MAGIC_TCP;
    std::unique_ptr<TcpSprite> sprite;
};

uint16_t U16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

int16_t S16(const uint8_t* p) {
    return static_cast<int16_t>(U16(p));
}

uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadFile(const char* path, std::vector<uint8_t>& out, std::string& err) {
    size_t size = 0;
    void* data = SDL_LoadFile(path, &size);
    if (!data || size == 0) {
        const char* sdlErr = SDL_GetError();
        err = (sdlErr && *sdlErr) ? sdlErr : "load failed";
        if (data) SDL_free(data);
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.assign(bytes, bytes + size);
    SDL_free(data);
    return true;
}

RGB Rgb565(uint16_t c) {
    RGB rgb;
    const int r5 = (c >> 11) & 0x1F;
    const int g6 = (c >> 5) & 0x3F;
    const int b5 = c & 0x1F;
    rgb.r = static_cast<uint8_t>((r5 * 255 + 15) / 31);
    rgb.g = static_cast<uint8_t>((g6 * 255 + 31) / 63);
    rgb.b = static_cast<uint8_t>((b5 * 255 + 15) / 31);
    return rgb;
}

uint32_t PackArgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

uint32_t BlendArgb(uint32_t dst, uint32_t src) {
    const int a1 = (dst >> 24) & 0xFF;
    const int b1 = (dst >> 16) & 0xFF;
    const int g1 = (dst >> 8) & 0xFF;
    const int r1 = dst & 0xFF;
    const int a2 = (src >> 24) & 0xFF;
    const int b2 = (src >> 16) & 0xFF;
    const int g2 = (src >> 8) & 0xFF;
    const int r2 = src & 0xFF;
    if (a2 == 0) return dst;
    if (a2 == 255 || a1 == 0) return src;
    const int a = a2 + (a1 * (255 - a2)) / 255;
    if (a <= 0) return 0;
    const int r = (r2 * a2 + (r1 * a1 * (255 - a2)) / 255) / a;
    const int g = (g2 * a2 + (g1 * a1 * (255 - a2)) / 255) / a;
    const int b = (b2 * a2 + (b1 * a1 * (255 - a2)) / 255) / a;
    return PackArgb(static_cast<uint8_t>(std::clamp(r, 0, 255)),
                    static_cast<uint8_t>(std::clamp(g, 0, 255)),
                    static_cast<uint8_t>(std::clamp(b, 0, 255)),
                    static_cast<uint8_t>(std::clamp(a, 0, 255)));
}

void WritePixel(FrameInfo& frame, int row, int col, const std::vector<RGB>& pal, int idx, int alpha) {
    if (row < 0 || col < 0 || row >= frame.height || col >= frame.width) return;
    RGB rgb;
    if (idx >= 0 && idx < static_cast<int>(pal.size())) rgb = pal[static_cast<size_t>(idx)];
    const size_t off = (static_cast<size_t>(row) * frame.width + col) * 4;
    frame.rgba[off] = static_cast<char>(rgb.r);
    frame.rgba[off + 1] = static_cast<char>(rgb.g);
    frame.rgba[off + 2] = static_cast<char>(rgb.b);
    frame.rgba[off + 3] = static_cast<char>(alpha & 0xFF);
}

void BlendPixelLinear(FrameInfo& frame, int pixelIndex, const std::vector<RGB>& pal, int idx, int alpha) {
    if (pixelIndex < 0 || pixelIndex >= frame.width * frame.height) return;
    RGB rgb;
    if (idx >= 0 && idx < static_cast<int>(pal.size())) rgb = pal[static_cast<size_t>(idx)];
    const size_t off = static_cast<size_t>(pixelIndex) * 4;
    const uint32_t dst = PackArgb(static_cast<uint8_t>(frame.rgba[off]), static_cast<uint8_t>(frame.rgba[off + 1]),
                                  static_cast<uint8_t>(frame.rgba[off + 2]), static_cast<uint8_t>(frame.rgba[off + 3]));
    const uint32_t src = PackArgb(rgb.r, rgb.g, rgb.b, static_cast<uint8_t>(alpha & 0xFF));
    const uint32_t out = BlendArgb(dst, src);
    frame.rgba[off] = static_cast<char>(out & 0xFF);
    frame.rgba[off + 1] = static_cast<char>((out >> 8) & 0xFF);
    frame.rgba[off + 2] = static_cast<char>((out >> 16) & 0xFF);
    frame.rgba[off + 3] = static_cast<char>((out >> 24) & 0xFF);
}

bool ParseTcp(TcpSprite& sprite, std::string& err) {
    const auto& d = sprite.data;
    if (d.size() < 16) {
        err = "TCP file too short";
        return false;
    }
    const uint16_t type = U16(d.data());
    const uint16_t length = U16(d.data() + 2);
    if (type != 0x5053) {
        err = "unsupported TCP type";
        return false;
    }
    sprite.directions = U16(d.data() + 4);
    sprite.framesPerDirection = U16(d.data() + 6);
    sprite.width = U16(d.data() + 8);
    sprite.height = U16(d.data() + 10);
    sprite.x = S16(d.data() + 12);
    sprite.y = S16(d.data() + 14);
    const uint32_t total = static_cast<uint32_t>(sprite.directions) * sprite.framesPerDirection;
    if (total == 0 || total > 4096) {
        err = "invalid SP frame count";
        return false;
    }

    uint32_t headerLen = 0;
    uint32_t paletteOff = 0;
    uint32_t tableOff = 0;
    if (length == 0x800F) {
        headerLen = static_cast<uint32_t>((length & 0xFF) + 4);
        if (d.size() <= 17) {
            err = "invalid SP palette header";
            return false;
        }
        const uint32_t palCount = d[17] ? d[17] : 256;
        paletteOff = headerLen;
        tableOff = paletteOff + palCount * 3;
        if (d.size() < tableOff + total * 4) {
            err = "truncated SP palette or offset table";
            return false;
        }
        sprite.palette.resize(palCount);
        for (uint32_t i = 0; i < palCount; ++i) {
            const size_t p = paletteOff + i * 3;
            sprite.palette[i] = RGB{d[p + 2], d[p + 1], d[p]};
        }
    } else {
        headerLen = static_cast<uint32_t>(length) + 4;
        paletteOff = headerLen;
        tableOff = paletteOff + 512;
        if (d.size() < tableOff + total * 4) {
            err = "truncated SP palette or offset table";
            return false;
        }
        sprite.palette.resize(256);
        for (uint32_t i = 0; i < 256; ++i) sprite.palette[i] = Rgb565(U16(d.data() + paletteOff + i * 2));
    }

    sprite.offsets.resize(total);
    for (uint32_t i = 0; i < total; ++i) {
        const uint32_t stored = U32(d.data() + tableOff + i * 4);
        sprite.offsets[i] = stored ? stored + headerLen : 0;
    }
    return true;
}

bool DecodeFrame(const TcpSprite& sprite, int index, FrameInfo& out, std::string& err) {
    if (index < 0 || index >= static_cast<int>(sprite.offsets.size())) {
        err = "frame index out of range";
        return false;
    }
    const uint32_t frameOffset = sprite.offsets[static_cast<size_t>(index)];
    if (frameOffset == 0 || frameOffset + 16 > sprite.data.size()) {
        out = FrameInfo{};
        return true;
    }
    uint32_t endOffset = static_cast<uint32_t>(sprite.data.size());
    for (uint32_t off : sprite.offsets) {
        if (off > frameOffset && off < endOffset) endOffset = off;
    }
    const uint8_t* base = sprite.data.data();
    out.x = static_cast<int>(static_cast<int32_t>(U32(base + frameOffset)));
    out.y = static_cast<int>(static_cast<int32_t>(U32(base + frameOffset + 4)));
    out.width = static_cast<int>(U32(base + frameOffset + 8));
    out.height = static_cast<int>(U32(base + frameOffset + 12));
    if (out.width <= 0 || out.height <= 0 || out.width > 2048 || out.height > 2048) {
        err = "invalid SP frame dimensions";
        return false;
    }
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, '\0');

    const uint32_t offsPtr = frameOffset + 16;
    if (offsPtr + 4 > endOffset) {
        err = "truncated SP line table";
        return false;
    }
    const uint32_t overlayFlag = U32(base + offsPtr);
    const bool hasOverlayHeader = overlayFlag == 1 || overlayFlag == 2;
    const uint32_t lineTable = hasOverlayHeader ? offsPtr + 8 : offsPtr;
    const uint32_t tableSize = static_cast<uint32_t>(out.height) * 4;
    if (lineTable + tableSize > endOffset) {
        err = "truncated SP line table";
        return false;
    }
    const uint32_t minDataOff = hasOverlayHeader ? 16 + 8 + tableSize : 16 + tableSize;
    std::vector<uint32_t> lineOffsets(static_cast<size_t>(out.height));
    bool hasValidLine = false;
    for (int i = 0; i < out.height; ++i) {
        lineOffsets[static_cast<size_t>(i)] = U32(base + lineTable + static_cast<size_t>(i) * 4);
        const uint32_t off = lineOffsets[static_cast<size_t>(i)];
        if (off >= minDataOff && frameOffset + off < endOffset) hasValidLine = true;
    }
    if (!hasValidLine) {
        err = "invalid SP line table";
        return false;
    }

    bool interlaced = true;
    for (int row = 1; row < out.height; row += 2) {
        const uint32_t off = lineOffsets[static_cast<size_t>(row)];
        const uint32_t ptr = frameOffset + off;
        if (off && ptr < endOffset && base[ptr] != 0) {
            interlaced = false;
            break;
        }
    }

    uint32_t ptrEnd = frameOffset;
    for (int row = 0; row < out.height; ++row) {
        uint32_t ptr = frameOffset + lineOffsets[static_cast<size_t>(row)];
        if (lineOffsets[static_cast<size_t>(row)] == 0 || ptr >= endOffset) continue;
        if (base[ptr] == 0) {
            if (interlaced && (row % 2 == 1) && row > 0) {
                const size_t dst = static_cast<size_t>(row) * out.width * 4;
                std::memcpy(&out.rgba[dst], &out.rgba[dst - static_cast<size_t>(out.width) * 4], static_cast<size_t>(out.width) * 4);
            }
            continue;
        }
        int col = 0;
        while (ptr < endOffset && col < out.width) {
            const uint8_t b = base[ptr++];
            if (b == 0) break;
            const uint8_t flag = (b & 0xC0) >> 6;
            if (flag == 0) {
                if (b & 0x20) {
                    if (ptr >= endOffset) break;
                    WritePixel(out, row, col++, sprite.palette, base[ptr++], (b & 0x1F) << 3);
                } else {
                    const int cnt = b & 0x1F;
                    if (ptr + 2 > endOffset) break;
                    const int alpha = base[ptr++] << 3;
                    const int idx = base[ptr++];
                    for (int i = 0; i < cnt && col < out.width; ++i) WritePixel(out, row, col++, sprite.palette, idx, alpha);
                }
            } else if (flag == 1) {
                const int cnt = b & 0x3F;
                for (int i = 0; i < cnt && ptr < endOffset && col < out.width; ++i) WritePixel(out, row, col++, sprite.palette, base[ptr++], 255);
            } else if (flag == 2) {
                const int cnt = b & 0x3F;
                if (ptr >= endOffset) break;
                const int idx = base[ptr++];
                for (int i = 0; i < cnt && col < out.width; ++i) WritePixel(out, row, col++, sprite.palette, idx, 255);
            } else {
                const int cnt = b & 0x3F;
                if (cnt == 0) {
                    if (ptr + 2 > endOffset) break;
                    BlendPixelLinear(out, row * out.width + col - 1, sprite.palette, base[ptr + 1], (base[ptr] & 0x1F) << 3);
                    ptr += 2;
                } else {
                    col += cnt;
                }
            }
        }
        ptrEnd = std::max(ptrEnd, ptr);
    }
    return true;
}

TcpHandle* CheckTcp(lua_State* L, int idx) {
    auto* h = static_cast<TcpHandle*>(luaL_checkudata(L, idx, MT_TCP));
    if (!h || h->magic != LT_MAGIC_TCP || !h->sprite) luaL_error(L, "Light.Plugins.TCP: invalid or closed handle");
    return h;
}

void PushFrame(lua_State* L, const FrameInfo& f) {
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, f.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, f.height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, f.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, f.y);
    lua_setfield(L, -2, "y");
    lua_pushlstring(L, f.rgba.data(), f.rgba.size());
    lua_setfield(L, -2, "rgba");
}

int l_TCP_Open(lua_State* L) {
    const char* path = LT::CheckStringStrict(L, 1);
    std::string err;
    auto sprite = std::make_unique<TcpSprite>();
    sprite->path = path;
    if (!ReadFile(path, sprite->data, err)) return LT::PushNilError(L, "TCP.Open: %s", err.c_str());
    if (!ParseTcp(*sprite, err)) return LT::PushNilError(L, "TCP.Open: %s", err.c_str());
    auto* h = static_cast<TcpHandle*>(lua_newuserdata(L, sizeof(TcpHandle)));
    new (h) TcpHandle();
    h->sprite = std::move(sprite);
    luaL_getmetatable(L, MT_TCP);
    lua_setmetatable(L, -2);
    return 1;
}

int l_Handle_GetInfo(lua_State* L) {
    const auto* s = CheckTcp(L, 1)->sprite.get();
    lua_createtable(L, 0, 7);
    lua_pushstring(L, "SP"); lua_setfield(L, -2, "kind");
    lua_pushinteger(L, s->directions); lua_setfield(L, -2, "directions");
    lua_pushinteger(L, s->framesPerDirection); lua_setfield(L, -2, "framesPerDirection");
    lua_pushinteger(L, static_cast<lua_Integer>(s->offsets.size())); lua_setfield(L, -2, "frameCount");
    lua_pushinteger(L, s->width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, s->height); lua_setfield(L, -2, "height");
    lua_pushstring(L, s->path.c_str()); lua_setfield(L, -2, "path");
    return 1;
}

int l_Handle_DecodeFrame(lua_State* L) {
    const auto* s = CheckTcp(L, 1)->sprite.get();
    const int index = static_cast<int>(luaL_checkinteger(L, 2));
    FrameInfo frame;
    std::string err;
    if (!DecodeFrame(*s, index, frame, err)) return LT::PushNilError(L, "TCP.DecodeFrame: %s", err.c_str());
    PushFrame(L, frame);
    return 1;
}

int l_Handle_DecodeAtlas(lua_State* L) {
    const auto* s = CheckTcp(L, 1)->sprite.get();
    std::vector<FrameInfo> frames(s->offsets.size());
    int cellW = std::max<int>(1, s->width);
    int cellH = std::max<int>(1, s->height);
    std::string err;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!DecodeFrame(*s, static_cast<int>(i), frames[i], err)) return LT::PushNilError(L, "TCP.DecodeAtlas: %s", err.c_str());
        cellW = std::max(cellW, frames[i].width);
        cellH = std::max(cellH, frames[i].height);
    }
    const int atlasW = std::max<int>(1, s->framesPerDirection) * cellW;
    const int atlasH = std::max<int>(1, s->directions) * cellH;
    std::string atlas(static_cast<size_t>(atlasW) * atlasH * 4, '\0');
    for (size_t i = 0; i < frames.size(); ++i) {
        const FrameInfo& f = frames[i];
        const int col = static_cast<int>(i % std::max<int>(1, s->framesPerDirection));
        const int row = static_cast<int>(i / std::max<int>(1, s->framesPerDirection));
        const int dstX = col * cellW;
        const int dstY = row * cellH;
        for (int y = 0; y < f.height; ++y) {
            const size_t src = static_cast<size_t>(y) * f.width * 4;
            const size_t dst = (static_cast<size_t>(dstY + y) * atlasW + dstX) * 4;
            std::memcpy(&atlas[dst], &f.rgba[src], static_cast<size_t>(f.width) * 4);
        }
    }
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, atlasW); lua_setfield(L, -2, "width");
    lua_pushinteger(L, atlasH); lua_setfield(L, -2, "height");
    lua_pushlstring(L, atlas.data(), atlas.size()); lua_setfield(L, -2, "rgba");
    return 1;
}

int l_Handle_Close(lua_State* L) {
    auto* h = static_cast<TcpHandle*>(luaL_checkudata(L, 1, MT_TCP));
    if (h && h->magic == LT_MAGIC_TCP) h->sprite.reset();
    lua_pushboolean(L, 1);
    return 1;
}

int l_Handle_GC(lua_State* L) {
    auto* h = static_cast<TcpHandle*>(luaL_checkudata(L, 1, MT_TCP));
    if (h && h->magic != LT::LT_MAGIC_DEAD) {
        h->sprite.reset();
        h->magic = LT::LT_MAGIC_DEAD;
        h->~TcpHandle();
    }
    return 0;
}

int l_Handle_Tostring(lua_State* L) {
    auto* h = static_cast<TcpHandle*>(luaL_checkudata(L, 1, MT_TCP));
    lua_pushfstring(L, "Light.Plugins.TCP.Handle: %p", h);
    return 1;
}

const luaL_Reg kTcpFns[] = {
    {"Open", l_TCP_Open},
    {nullptr, nullptr}
};

const luaL_Reg kHandleFns[] = {
    {"GetInfo", l_Handle_GetInfo},
    {"DecodeFrame", l_Handle_DecodeFrame},
    {"DecodeAtlas", l_Handle_DecodeAtlas},
    {"Close", l_Handle_Close},
    {"__gc", l_Handle_GC},
    {"__tostring", l_Handle_Tostring},
    {nullptr, nullptr}
};

} // namespace

extern "C" LIGHT_API int luaopen_Light_Plugins_TCP(lua_State* L) {
    if (luaL_newmetatable(L, MT_TCP)) {
        luaL_setfuncs(L, kHandleFns, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    LT::Resource::RegisterPluginsSubmodule(L, "TCP", kTcpFns);
    return 1;
}
