#include "resource_package.h"

#include "stb_image.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t LT_MAGIC_MAP = LT::Magic4('M', 'A', 'P', 'H');
constexpr const char* MT_MAP = "Light.Plugins.Map.Handle";
constexpr int TILE_W = 320;
constexpr int TILE_H = 240;

struct ImageRGBA {
    int width = 0;
    int height = 0;
    std::string rgba;
};

struct MapFile {
    std::string path;
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t columns = 0;
    uint32_t rows = 0;
    uint32_t tileCount = 0;
    uint32_t maskOffset = 0;
    std::vector<uint32_t> offsets;
};

struct MapHandle {
    uint32_t magic = LT_MAGIC_MAP;
    std::unique_ptr<MapFile> map;
};

uint16_t U16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint16_t U16BE(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t FourCC(const char s[5]) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
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

uint32_t CeilDiv(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

std::vector<uint8_t> FixJpegYunfeng(const uint8_t* src, size_t size) {
    if (size < 4 || src[0] != 0xFF || src[1] != 0xD8 || src[2] != 0xFF || src[3] != 0xA0) {
        return std::vector<uint8_t>(src, src + size);
    }
    std::vector<uint8_t> out;
    out.reserve(size + 1024);
    size_t i = 0;
    size_t temp = 0;
    while (temp < size && i < size && src[i] == 0xFF) {
        ++i;
        ++temp;
        out.push_back(0xFF);
        if (i >= size) break;
        const uint8_t marker = src[i];
        if (marker == 0xD8) {
            out.push_back(0xD8);
            ++i;
            ++temp;
            continue;
        }
        if (marker == 0xA0) {
            ++i;
            ++temp;
            out.pop_back();
            continue;
        }
        if (marker == 0xC0 || marker == 0xC4 || marker == 0xDB) {
            out.push_back(marker);
            ++i;
            ++temp;
            if (i + 2 > size) break;
            const uint16_t segLen = U16BE(src + i);
            for (uint16_t n = 0; n < segLen && i < size; ++n, ++i, ++temp) out.push_back(src[i]);
            continue;
        }
        if (marker == 0xDA) {
            out.push_back(0xDA);
            out.push_back(0x00);
            out.push_back(0x0C);
            ++i;
            ++temp;
            if (i + 2 > size) break;
            const uint16_t segLen = U16BE(src + i);
            i += 2;
            ++temp;
            for (uint16_t n = 2; n < segLen && i < size; ++n, ++i, ++temp) out.push_back(src[i]);
            out.push_back(0x00);
            out.push_back(0x3F);
            out.push_back(0x00);
            const size_t scanEnd = size >= 2 ? size - 2 : size;
            while (i < scanEnd) {
                if (src[i] == 0xFF) {
                    out.push_back(0xFF);
                    out.push_back(0x00);
                } else {
                    out.push_back(src[i]);
                }
                ++i;
            }
            out.push_back(0xFF);
            out.push_back(0xD9);
            break;
        }
        if (marker == 0xD9) {
            out.push_back(0xD9);
            ++i;
            ++temp;
            break;
        }
        break;
    }
    return out;
}

bool DecodeJpegToRgba(const uint8_t* bytes, size_t size, ImageRGBA& out) {
    std::vector<uint8_t> fixed = FixJpegYunfeng(bytes, size);
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(fixed.data(), static_cast<int>(fixed.size()), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.assign(reinterpret_cast<const char*>(pixels), static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

void Paste(ImageRGBA& dst, const ImageRGBA& src, int dstX, int dstY) {
    if (dst.width <= 0 || dst.height <= 0 || src.width <= 0 || src.height <= 0) return;
    for (int y = 0; y < src.height; ++y) {
        const int yy = dstY + y;
        if (yy < 0 || yy >= dst.height) continue;
        const int copyX0 = std::max(0, dstX);
        const int copyX1 = std::min(dst.width, dstX + src.width);
        if (copyX1 <= copyX0) continue;
        const size_t srcOff = (static_cast<size_t>(y) * src.width + (copyX0 - dstX)) * 4;
        const size_t dstOff = (static_cast<size_t>(yy) * dst.width + copyX0) * 4;
        std::memcpy(&dst.rgba[dstOff], &src.rgba[srcOff], static_cast<size_t>(copyX1 - copyX0) * 4);
    }
}

bool DecodeGmxJpegTile(const uint8_t* bytes, size_t size, ImageRGBA& out) {
    if (size < 26) return false;
    const uint16_t commonSize = U16(bytes);
    const size_t commonStart = 26;
    const size_t commonEnd = commonStart + commonSize;
    if (commonSize == 0 || commonEnd > size) return false;
    out.width = TILE_W;
    out.height = TILE_H;
    out.rgba.assign(static_cast<size_t>(TILE_W) * TILE_H * 4, '\0');
    size_t partPos = commonEnd;
    bool any = false;
    int idx = 0;
    for (int y = 0; y < TILE_H; y += 80) {
        for (int x = 0; x < TILE_W; x += 80) {
            if (idx >= 12) return any;
            const uint16_t partSize = U16(bytes + 2 + idx * 2);
            ++idx;
            if (partSize == 0) continue;
            const size_t partEnd = partPos + partSize;
            if (partEnd > size) return any;
            std::vector<uint8_t> joined;
            joined.reserve(commonSize + partSize);
            joined.insert(joined.end(), bytes + commonStart, bytes + commonEnd);
            joined.insert(joined.end(), bytes + partPos, bytes + partEnd);
            partPos = partEnd;
            ImageRGBA part;
            if (DecodeJpegToRgba(joined.data(), joined.size(), part)) {
                Paste(out, part, x, y);
                any = true;
            }
        }
    }
    return any;
}

bool ParseMap(MapFile& map, std::string& err) {
    const auto& d = map.data;
    if (d.size() < 16) {
        err = "map file too short";
        return false;
    }
    const bool isM10 = std::memcmp(d.data(), "0.1M", 4) == 0 || std::memcmp(d.data(), "M1.0", 4) == 0;
    if (!isM10) {
        err = "unsupported map flag";
        return false;
    }
    map.width = U32(d.data() + 4);
    map.height = U32(d.data() + 8);
    if (map.width == 0 || map.height == 0 || map.width > 65536 || map.height > 65536) {
        err = "invalid map dimensions";
        return false;
    }
    map.columns = CeilDiv(map.width, TILE_W);
    map.rows = CeilDiv(map.height, TILE_H);
    map.tileCount = map.columns * map.rows;
    const size_t listStart = 12;
    const size_t listBytes = static_cast<size_t>(map.tileCount) * 4;
    if (map.tileCount == 0 || listStart + listBytes + 4 > d.size()) {
        err = "truncated map tile list";
        return false;
    }
    map.offsets.resize(map.tileCount);
    for (uint32_t i = 0; i < map.tileCount; ++i) map.offsets[i] = U32(d.data() + listStart + static_cast<size_t>(i) * 4);
    map.maskOffset = U32(d.data() + listStart + listBytes);
    return true;
}

bool TileRange(const MapFile& map, uint32_t tile, uint32_t& begin, uint32_t& end) {
    if (tile >= map.offsets.size() || map.offsets[tile] == 0 || map.offsets[tile] >= map.data.size()) return false;
    begin = map.offsets[tile];
    uint32_t minNext = static_cast<uint32_t>(map.data.size());
    uint32_t minTile = static_cast<uint32_t>(map.data.size());
    for (uint32_t off : map.offsets) {
        if (off != 0) {
            minTile = std::min(minTile, off);
            if (off > begin) minNext = std::min(minNext, off);
        }
    }
    const uint32_t tileDataEnd = (map.maskOffset > begin && map.maskOffset < minNext) ? map.maskOffset : static_cast<uint32_t>(map.data.size());
    end = minNext == static_cast<uint32_t>(map.data.size()) ? tileDataEnd : minNext;
    if (end <= begin || end > map.data.size()) return false;
    (void)minTile;
    return true;
}

bool DecodeTile(const MapFile& map, uint32_t tile, ImageRGBA& out) {
    uint32_t begin = 0;
    uint32_t end = 0;
    if (!TileRange(map, tile, begin, end)) return false;
    const auto& d = map.data;
    size_t cur = begin;
    if (cur + 4 > end) return false;
    const uint32_t maskCount = U32(d.data() + cur);
    cur += 4;
    const size_t maskBytes = static_cast<size_t>(maskCount) * 4;
    if (cur + maskBytes > end) return false;
    cur += maskBytes;
    const uint32_t jpg2 = FourCC("JPG2");
    const uint32_t jpeg = FourCC("JPEG");
    const uint32_t endTag = FourCC("END ");
    const uint8_t* selected = nullptr;
    size_t selectedSize = 0;
    uint32_t selectedTag = 0;
    while (cur + 8 <= end) {
        const uint32_t tag = U32(d.data() + cur);
        const uint32_t size = U32(d.data() + cur + 4);
        cur += 8;
        if (tag == 0 || tag == endTag) break;
        if (cur + size > end) break;
        if (tag == jpg2 || tag == jpeg) {
            if (!selected || tag == jpg2) {
                selected = d.data() + cur;
                selectedSize = size;
                selectedTag = tag;
                if (tag == jpg2) break;
            }
        }
        cur += size;
    }
    if (!selected || selectedSize == 0) return false;
    if (selectedTag == jpeg) return DecodeGmxJpegTile(selected, selectedSize, out);
    return DecodeJpegToRgba(selected, selectedSize, out);
}

void PasteScaled(std::string& preview, int previewW, int previewH, const ImageRGBA& tile, int mapX, int mapY, double scale) {
    const int x0 = std::max(0, static_cast<int>(std::floor(mapX * scale)));
    const int y0 = std::max(0, static_cast<int>(std::floor(mapY * scale)));
    const int x1 = std::min(previewW, static_cast<int>(std::ceil((mapX + tile.width) * scale)));
    const int y1 = std::min(previewH, static_cast<int>(std::ceil((mapY + tile.height) * scale)));
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; ++y) {
        const int srcY = std::clamp(static_cast<int>((y / scale) - mapY), 0, tile.height - 1);
        for (int x = x0; x < x1; ++x) {
            const int srcX = std::clamp(static_cast<int>((x / scale) - mapX), 0, tile.width - 1);
            const size_t src = (static_cast<size_t>(srcY) * tile.width + srcX) * 4;
            const size_t dst = (static_cast<size_t>(y) * previewW + x) * 4;
            preview[dst] = tile.rgba[src];
            preview[dst + 1] = tile.rgba[src + 1];
            preview[dst + 2] = tile.rgba[src + 2];
            preview[dst + 3] = tile.rgba[src + 3];
        }
    }
}

MapHandle* CheckMap(lua_State* L, int idx) {
    auto* h = static_cast<MapHandle*>(luaL_checkudata(L, idx, MT_MAP));
    if (!h || h->magic != LT_MAGIC_MAP || !h->map) luaL_error(L, "Light.Plugins.Map: invalid or closed handle");
    return h;
}

int l_Map_Open(lua_State* L) {
    const char* path = LT::CheckStringStrict(L, 1);
    std::string err;
    auto map = std::make_unique<MapFile>();
    map->path = path;
    if (!ReadFile(path, map->data, err)) return LT::PushNilError(L, "Map.Open: %s", err.c_str());
    if (!ParseMap(*map, err)) return LT::PushNilError(L, "Map.Open: %s", err.c_str());
    auto* h = static_cast<MapHandle*>(lua_newuserdata(L, sizeof(MapHandle)));
    new (h) MapHandle();
    h->map = std::move(map);
    luaL_getmetatable(L, MT_MAP);
    lua_setmetatable(L, -2);
    return 1;
}

int l_Handle_GetInfo(lua_State* L) {
    const auto* m = CheckMap(L, 1)->map.get();
    lua_createtable(L, 0, 9);
    lua_pushstring(L, "M1.0"); lua_setfield(L, -2, "kind");
    lua_pushinteger(L, m->width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, m->height); lua_setfield(L, -2, "height");
    lua_pushinteger(L, TILE_W); lua_setfield(L, -2, "tileWidth");
    lua_pushinteger(L, TILE_H); lua_setfield(L, -2, "tileHeight");
    lua_pushinteger(L, m->columns); lua_setfield(L, -2, "columns");
    lua_pushinteger(L, m->rows); lua_setfield(L, -2, "rows");
    lua_pushinteger(L, m->tileCount); lua_setfield(L, -2, "tileCount");
    lua_pushstring(L, m->path.c_str()); lua_setfield(L, -2, "path");
    return 1;
}

int l_Handle_DecodePreview(lua_State* L) {
    const auto* m = CheckMap(L, 1)->map.get();
    int maxSide = static_cast<int>(luaL_optinteger(L, 2, 2048));
    if (maxSide <= 0) maxSide = 2048;
    const double scale = std::min(1.0, static_cast<double>(maxSide) / static_cast<double>(std::max(m->width, m->height)));
    const int previewW = std::max(1, static_cast<int>(std::floor(m->width * scale + 0.5)));
    const int previewH = std::max(1, static_cast<int>(std::floor(m->height * scale + 0.5)));
    std::string rgba(static_cast<size_t>(previewW) * previewH * 4, '\0');
    int decoded = 0;
    for (uint32_t i = 0; i < m->tileCount; ++i) {
        ImageRGBA tile;
        if (!DecodeTile(*m, i, tile)) continue;
        ++decoded;
        const int col = static_cast<int>(i % m->columns);
        const int row = static_cast<int>(i / m->columns);
        PasteScaled(rgba, previewW, previewH, tile, col * TILE_W, row * TILE_H, scale);
    }
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, previewW); lua_setfield(L, -2, "width");
    lua_pushinteger(L, previewH); lua_setfield(L, -2, "height");
    lua_pushinteger(L, decoded); lua_setfield(L, -2, "decodedTiles");
    lua_pushinteger(L, m->tileCount); lua_setfield(L, -2, "totalTiles");
    lua_pushlstring(L, rgba.data(), rgba.size()); lua_setfield(L, -2, "rgba");
    return 1;
}

int l_Handle_Close(lua_State* L) {
    auto* h = static_cast<MapHandle*>(luaL_checkudata(L, 1, MT_MAP));
    if (h && h->magic == LT_MAGIC_MAP) h->map.reset();
    lua_pushboolean(L, 1);
    return 1;
}

int l_Handle_GC(lua_State* L) {
    auto* h = static_cast<MapHandle*>(luaL_checkudata(L, 1, MT_MAP));
    if (h && h->magic != LT::LT_MAGIC_DEAD) {
        h->map.reset();
        h->magic = LT::LT_MAGIC_DEAD;
        h->~MapHandle();
    }
    return 0;
}

int l_Handle_Tostring(lua_State* L) {
    auto* h = static_cast<MapHandle*>(luaL_checkudata(L, 1, MT_MAP));
    lua_pushfstring(L, "Light.Plugins.Map.Handle: %p", h);
    return 1;
}

const luaL_Reg kMapFns[] = {
    {"Open", l_Map_Open},
    {nullptr, nullptr}
};

const luaL_Reg kHandleFns[] = {
    {"GetInfo", l_Handle_GetInfo},
    {"DecodePreview", l_Handle_DecodePreview},
    {"Close", l_Handle_Close},
    {"__gc", l_Handle_GC},
    {"__tostring", l_Handle_Tostring},
    {nullptr, nullptr}
};

} // namespace

extern "C" LIGHT_API int luaopen_Light_Plugins_Map(lua_State* L) {
    if (luaL_newmetatable(L, MT_MAP)) {
        luaL_setfuncs(L, kHandleFns, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    LT::Resource::RegisterPluginsSubmodule(L, "Map", kMapFns);
    return 1;
}
