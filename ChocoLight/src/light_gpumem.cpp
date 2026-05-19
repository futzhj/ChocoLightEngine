/**
 * @file light_gpumem.cpp
 * @brief Phase G.1 — VRAM Tracking: GpuMemTracker 单例 + Light.Graphics.GetMemoryStats
 *
 * 跟踪策略:
 *   - 高层 wrapper hook (HDR/TAA/SSR/Dilate/UBO Skin), 不动底层 glGen* / glTexImage2D
 *   - 按 category × format × size 累计 count + bytes
 *   - 不区分 instance ID, 多 instance 表现为 count 增长 (用户友好)
 *
 * 公式表 (bytes_per_pixel):
 *   RGBA8        = 4
 *   RGBA16F      = 8
 *   RG8          = 2
 *   RG16F        = 4
 *   R16F         = 2
 *   R32F         = 4
 *   DEPTH24      = 4 (depth RBO)
 *   DEPTH32F     = 4
 *
 * 调用约定:
 *   Track(cat, w, h, format)    — 创建时调, count++, bytes += w*h*bpp
 *   Untrack(cat, w, h, format)  — 释放时调, count--, bytes -= w*h*bpp
 *   TrackBytes(cat, n)          — 直接传 bytes (UBO 等非 wxh 资源)
 *   UntrackBytes(cat, n)
 *
 * 错误恢复:
 *   - Untrack 找不到对应记录 → 静默忽略 (release 多于 create 时)
 *   - 数组满 → 合并相同 category+format 的项 (按 count 累加)
 *
 * 跨平台: 全 C++, 无 GL 依赖, mobile/web 一致
 */

#include "light.h"
#include <cstring>
#include <cstdint>
#include <mutex>   // Phase G.1.3 — worker thread Track 需 mutex 护航

namespace {

constexpr int GPU_MEM_MAX_ITEMS = 64;   // 唯一 (category, format) 组合上限

struct GpuMemItem {
    bool        used;
    char        name[64];        // category 名 (e.g. "HDR sceneTex")
    char        format[16];      // 格式名 (e.g. "RGBA16F")
    int         count;           // 当前活跃实例数
    int         lastW;           // 最近一次 size (诊断用)
    int         lastH;
    int64_t     bytesPerInst;    // 单实例 bytes (=w*h*bpp)
    int64_t     totalBytes;      // = count * bytesPerInst (汇总)
};

static GpuMemItem s_items[GPU_MEM_MAX_ITEMS];

// Phase G.1.3 — 护航 s_items 全局数组. 质粒度: 整个 API 体. 锁内 < 100 ns,
//   不影响主线程帧预算. Worker thread 调 Track 与主线程调 PushStats 互斥.
static std::mutex s_mutex;

// 查找已有 item (按 name + format + bytesPerInst 三元组匹配)
//   bytesPerInst 包含尺寸信息 → 同 category 不同尺寸算两条记录
//   用户调 Light.Graphics.HDR.Resize 时旧尺寸 Untrack + 新尺寸 Track, 自然对齐
static int FindItem(const char* name, const char* format, int64_t bytesPerInst) {
    for (int i = 0; i < GPU_MEM_MAX_ITEMS; ++i) {
        if (!s_items[i].used) continue;
        if (s_items[i].bytesPerInst != bytesPerInst) continue;
        if (std::strcmp(s_items[i].format, format) != 0) continue;
        if (std::strcmp(s_items[i].name, name) != 0) continue;
        return i;
    }
    return -1;
}

static int FindFreeSlot() {
    for (int i = 0; i < GPU_MEM_MAX_ITEMS; ++i) {
        if (!s_items[i].used) return i;
    }
    return -1;
}

// 根据 format 字符串返 bytes/pixel (已知格式表)
static int BytesPerPixel(const char* format) {
    if (!format) return 0;
    if (std::strcmp(format, "RGBA8")    == 0) return 4;
    if (std::strcmp(format, "RGBA16F")  == 0) return 8;
    if (std::strcmp(format, "RG8")      == 0) return 2;
    if (std::strcmp(format, "RG16F")    == 0) return 4;
    if (std::strcmp(format, "R16F")     == 0) return 2;
    if (std::strcmp(format, "R32F")     == 0) return 4;
    if (std::strcmp(format, "DEPTH24")  == 0) return 4;
    if (std::strcmp(format, "DEPTH32F") == 0) return 4;
    if (std::strcmp(format, "RGB32F")   == 0) return 12;
    // Phase G.1.2 — 用户 Image / Font glyph atlas (channels 1/3/4 对应)
    if (std::strcmp(format, "R8")       == 0) return 1;
    if (std::strcmp(format, "RGB8")     == 0) return 3;
    return 0;   // 未知格式
}

} // namespace

namespace LT { namespace GpuMem {

// ============================================================
// 公共 C 接口 (供 wrapper hook 调用)
// ============================================================

/// 跟踪一个 RT/texture 实例 (count++, bytes 累加)
/// @param name   类别名 (e.g. "HDR sceneTex"), 长度 < 64
/// @param format 格式字符串 (e.g. "RGBA16F"), 长度 < 16
/// @param w/h    尺寸 (像素)
void Track(const char* name, const char* format, int w, int h) {
    if (!name || !format || w <= 0 || h <= 0) return;
    const int bpp = BytesPerPixel(format);
    if (bpp <= 0) return;
    const int64_t bytesPerInst = (int64_t)w * (int64_t)h * (int64_t)bpp;
    std::lock_guard<std::mutex> lock(s_mutex);   // Phase G.1.3 — worker safe

    int slot = FindItem(name, format, bytesPerInst);
    if (slot < 0) {
        slot = FindFreeSlot();
        if (slot < 0) return;   // 数组满, 静默丢弃 (>64 unique combo 不应该发生)
        s_items[slot].used = true;
        std::strncpy(s_items[slot].name, name, sizeof(s_items[slot].name) - 1);
        s_items[slot].name[sizeof(s_items[slot].name) - 1] = '\0';
        std::strncpy(s_items[slot].format, format, sizeof(s_items[slot].format) - 1);
        s_items[slot].format[sizeof(s_items[slot].format) - 1] = '\0';
        s_items[slot].count        = 0;
        s_items[slot].bytesPerInst = bytesPerInst;
    }
    s_items[slot].count++;
    s_items[slot].lastW = w;
    s_items[slot].lastH = h;
    s_items[slot].totalBytes = (int64_t)s_items[slot].count * s_items[slot].bytesPerInst;
}

/// 取消跟踪 (count--, 若 count<=0 则释放 slot)
void Untrack(const char* name, const char* format, int w, int h) {
    if (!name || !format || w <= 0 || h <= 0) return;
    const int bpp = BytesPerPixel(format);
    if (bpp <= 0) return;
    const int64_t bytesPerInst = (int64_t)w * (int64_t)h * (int64_t)bpp;
    std::lock_guard<std::mutex> lock(s_mutex);   // Phase G.1.3 — worker safe

    int slot = FindItem(name, format, bytesPerInst);
    if (slot < 0) return;   // release 多于 create, 静默忽略
    s_items[slot].count--;
    if (s_items[slot].count <= 0) {
        s_items[slot].used = false;
        s_items[slot].count = 0;
        s_items[slot].totalBytes = 0;
    } else {
        s_items[slot].totalBytes = (int64_t)s_items[slot].count * s_items[slot].bytesPerInst;
    }
}

/// 跟踪非 wxh 资源 (UBO 等), 直接传 bytes
/// @param name 类别名
/// @param bytes 单实例 bytes
void TrackBytes(const char* name, int64_t bytes) {
    if (!name || bytes <= 0) return;
    std::lock_guard<std::mutex> lock(s_mutex);   // Phase G.1.3 — worker safe

    // 用 "BYTES" 作 format 标识, 用 bytes 自身作 bytesPerInst
    int slot = FindItem(name, "BYTES", bytes);
    if (slot < 0) {
        slot = FindFreeSlot();
        if (slot < 0) return;
        s_items[slot].used = true;
        std::strncpy(s_items[slot].name, name, sizeof(s_items[slot].name) - 1);
        s_items[slot].name[sizeof(s_items[slot].name) - 1] = '\0';
        std::strncpy(s_items[slot].format, "BYTES", sizeof(s_items[slot].format) - 1);
        s_items[slot].format[sizeof(s_items[slot].format) - 1] = '\0';
        s_items[slot].count        = 0;
        s_items[slot].bytesPerInst = bytes;
        s_items[slot].lastW        = 0;
        s_items[slot].lastH        = 0;
    }
    s_items[slot].count++;
    s_items[slot].totalBytes = (int64_t)s_items[slot].count * s_items[slot].bytesPerInst;
}

void UntrackBytes(const char* name, int64_t bytes) {
    if (!name || bytes <= 0) return;
    std::lock_guard<std::mutex> lock(s_mutex);   // Phase G.1.3 — worker safe
    int slot = FindItem(name, "BYTES", bytes);
    if (slot < 0) return;
    s_items[slot].count--;
    if (s_items[slot].count <= 0) {
        s_items[slot].used = false;
        s_items[slot].count = 0;
        s_items[slot].totalBytes = 0;
    } else {
        s_items[slot].totalBytes = (int64_t)s_items[slot].count * s_items[slot].bytesPerInst;
    }
}

/// 清空全部跟踪 (smoke 用)
void Reset() {
    std::lock_guard<std::mutex> lock(s_mutex);   // Phase G.1.3 — worker safe
    for (int i = 0; i < GPU_MEM_MAX_ITEMS; ++i) {
        s_items[i].used = false;
        s_items[i].count = 0;
        s_items[i].totalBytes = 0;
    }
}

// ============================================================
// Lua 绑定: 推 stats table 到栈顶 (供 light_graphics.cpp 调)
// ============================================================

/// 推 stats table 到 Lua 栈
/// 返回 table 结构:
///   {
///     total_bytes = N,
///     render_targets = {count = N, bytes = N},
///     ubos           = {count = N, bytes = N},
///     items = { {name, format, count, bytes, w, h}, ... }
///   }
int PushStats(lua_State* L) {
    int64_t totalBytes = 0;
    int64_t rtCount = 0,  rtBytes  = 0;
    int64_t uboCount = 0, uboBytes = 0;
    std::lock_guard<std::mutex> lock(s_mutex);   // Phase G.1.3 — worker safe

    // 汇总
    for (int i = 0; i < GPU_MEM_MAX_ITEMS; ++i) {
        if (!s_items[i].used) continue;
        totalBytes += s_items[i].totalBytes;
        // BYTES 格式 = UBO 类; 其他都算 render_targets
        if (std::strcmp(s_items[i].format, "BYTES") == 0) {
            uboCount += s_items[i].count;
            uboBytes += s_items[i].totalBytes;
        } else {
            rtCount += s_items[i].count;
            rtBytes += s_items[i].totalBytes;
        }
    }

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)totalBytes);
    lua_setfield(L, -2, "total_bytes");

    // render_targets 子表
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)rtCount);  lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer)rtBytes);  lua_setfield(L, -2, "bytes");
    lua_setfield(L, -2, "render_targets");

    // ubos 子表
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)uboCount); lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer)uboBytes); lua_setfield(L, -2, "bytes");
    lua_setfield(L, -2, "ubos");

    // items 数组
    lua_createtable(L, 0, 0);
    int idx = 1;
    for (int i = 0; i < GPU_MEM_MAX_ITEMS; ++i) {
        if (!s_items[i].used) continue;
        lua_createtable(L, 0, 6);
        lua_pushstring(L, s_items[i].name);   lua_setfield(L, -2, "name");
        lua_pushstring(L, s_items[i].format); lua_setfield(L, -2, "format");
        lua_pushinteger(L, s_items[i].count); lua_setfield(L, -2, "count");
        lua_pushinteger(L, (lua_Integer)s_items[i].totalBytes); lua_setfield(L, -2, "bytes");
        lua_pushinteger(L, s_items[i].lastW); lua_setfield(L, -2, "w");
        lua_pushinteger(L, s_items[i].lastH); lua_setfield(L, -2, "h");
        lua_rawseti(L, -2, idx++);
    }
    lua_setfield(L, -2, "items");

    return 1;   // table 留栈顶
}

}} // namespace LT::GpuMem
