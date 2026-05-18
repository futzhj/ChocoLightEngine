/**
 * @file   light_asset.cpp
 * @brief  Phase G.1.6 — Light.AssetLoader.Preload 异步预加载 manifest
 *
 * 设计目标:
 *   - 一次提交 N 个异步资源加载 (Image / Sound / LUT / Font / GLTF Mesh) 任务
 *   - 全部完成 (含 Error) 后触发用户 totalCb(succ, fail, errors)
 *   - 提供 BatchHandle userdata 供 :GetProgress / :IsDone / :Cancel
 *
 * 实现策略:
 *   - 完全在 Lua binding 层聚合现有 6 个 AssetLoader::LoadXxxAsync C++ 入口
 *   - 不修改 asset_loader.h / asset_loader.cpp / light_ui.cpp
 *   - 复用现有 RegisterCallback dispatcher 机制
 *   - 每个 sub-future 用唯一 Lua registry ref 持 BatchHandle 强引用
 *     (luaL_ref 同一 userdata 多次返回不同 ref id, 各 ref 独立 unref)
 *
 * 主线程独占语义:
 *   - dispatcher 仅由 AssetLoader::Tick 在主线程调
 *   - Lua API (Preload / GetProgress / IsDone / Cancel) 也在主线程
 *   - BatchHandleUd 字段无锁 (单线程访问)
 *
 * 详见 docs/Phase G.1.6 Async Preload Manifest/{ALIGNMENT,DESIGN}_PhaseG_1_6.md
 *
 * 作者: ChocoLight Engine
 * 版本: Phase G.1.6
 */

#include "light.h"
#include "asset_loader.h"

#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

// ==================== BatchHandle 数据结构 ====================

/// 单个失败条目: 路径 + 错误信息
struct BatchErrorEntry {
    std::string path;
    std::string err;
};

/// BatchHandle userdata 内嵌结构
/// 主线程独占, 无锁; 析构由 __gc 走显式 placement-delete 路径
///
/// futurePaths[i] 与 futures[i] 并行: dispatcher 通过 raw ptr 线性扫描
/// 反查路径用于 errors 报告 (N <= ~1000 时 O(N) 单次成本可忽略)
struct BatchHandleUd {
    int                                                    total      = 0;
    int                                                    remaining  = 0;
    int                                                    succ       = 0;
    std::vector<BatchErrorEntry>                           errors;
    int                                                    totalCbRef = -1;
    bool                                                   cancelled  = false;
    lua_State*                                             L          = nullptr;
    /// 持子 future 强引用, 防用户提前丢弃 handle 导致 future 析构时 dispatcher 已失效
    std::vector<std::shared_ptr<AssetLoader::FutureState>> futures;
    /// 与 futures 并行, 用于 dispatcher 报告失败路径
    std::vector<std::string>                               futurePaths;
};

// 元表名 (Lua 注册表 key)
constexpr const char* kBatchHandleMT = "Light.AssetLoader._BatchHandle";

// ==================== 辅助 ====================

/// 取 BatchHandle userdata; 类型不符返 nullptr (luaL_testudata)
BatchHandleUd* GetBatchUd_(lua_State* L, int idx) {
    void* p = luaL_testudata(L, idx, kBatchHandleMT);
    return p ? static_cast<BatchHandleUd*>(p) : nullptr;
}

/// 严格版本: 类型不符直接 luaL_typerror
BatchHandleUd* CheckBatchUd_(lua_State* L, int idx) {
    void* p = luaL_checkudata(L, idx, kBatchHandleMT);
    return static_cast<BatchHandleUd*>(p);
}

// ==================== 总 cb 触发 ====================

/// 触发用户 totalCb(succ, fail, errors); 防重复触发 (totalCbRef 用后置 -1)
/// 总 cb 内异常不向上抛 (pcall 捕获, LOG_WARN)
static void FireTotalCb_(BatchHandleUd* ud) {
    if (!ud || !ud->L || ud->totalCbRef < 0) return;

    lua_State* L = ud->L;

    // 取 cb 函数到栈顶
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->totalCbRef);
    if (!lua_isfunction(L, -1)) {
        // cb 已被 GC 或不是函数 (理论不可能, 防御)
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, ud->totalCbRef);
        ud->totalCbRef = -1;
        return;
    }

    // arg1: succ
    lua_pushinteger(L, ud->succ);
    // arg2: fail
    lua_pushinteger(L, static_cast<int>(ud->errors.size()));
    // arg3: errors table { {path=..., err=...}, ... }
    const int errCount = static_cast<int>(ud->errors.size());
    lua_createtable(L, errCount, 0);
    for (int i = 0; i < errCount; ++i) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, ud->errors[i].path.c_str());
        lua_setfield(L, -2, "path");
        lua_pushstring(L, ud->errors[i].err.c_str());
        lua_setfield(L, -2, "err");
        lua_rawseti(L, -2, i + 1);
    }

    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* errStr = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "AssetLoader.Preload total cb error: %s",
                errStr ? errStr : "(none)");
        lua_pop(L, 1);
    }

    // 释放 cb ref + 防重复触发
    luaL_unref(L, LUA_REGISTRYINDEX, ud->totalCbRef);
    ud->totalCbRef = -1;
}

// ==================== 子 future dispatcher ====================

/// AssetLoader::Tick 在主线程调; state 此时已 Ready/Error
/// batchUdRef 是 luaL_ref(BatchHandle ud) 的唯一 ref id
///
/// 行为:
///   1. lua_rawgeti 拉 BatchHandle ud
///   2. 更新 succ / errors / remaining
///   3. remaining==0 && !cancelled → FireTotalCb_
///
/// Tick 调完此函数后会 luaL_unref(batchUdRef), 释放对应 1 个 ud 强引用
static void BatchSubItemDispatcher_(void* L_, AssetLoader::FutureState* state, int batchUdRef) {
    lua_State* L = static_cast<lua_State*>(L_);
    if (!L || !state || batchUdRef < 0) return;

    // 拉 BatchHandle ud (栈顶)
    lua_rawgeti(L, LUA_REGISTRYINDEX, batchUdRef);
    BatchHandleUd* ud = GetBatchUd_(L, -1);
    lua_pop(L, 1);
    if (!ud) return;  // 防御: 理论不会发生 (ref 持有强引用)

    if (state->status.load() == static_cast<int>(AssetLoader::FutureStatus::Ready)) {
        ud->succ++;
    } else {
        // 线性扫描反查 path (N <= ~1000 时单次 dispatch O(N) 可接受)
        std::string errPath = "(unknown)";
        for (size_t i = 0; i < ud->futures.size(); ++i) {
            if (ud->futures[i].get() == state) {
                errPath = ud->futurePaths[i];
                break;
            }
        }
        ud->errors.push_back({
            std::move(errPath),
            state->errorMsg.empty() ? "unknown error" : state->errorMsg,
        });
    }
    ud->remaining--;

    if (ud->remaining <= 0 && !ud->cancelled) {
        FireTotalCb_(ud);
    }
}

// ==================== manifest 解析辅助 ====================

/// 注册一个 sub-future 到 batch
/// path 仅用于 errors 记录 (state 本身不带 path 字段)
static void AppendOne_(lua_State* L,
                        BatchHandleUd* ud,
                        int batchUdStackIdx,
                        const std::string& path,
                        std::shared_ptr<AssetLoader::FutureState> state) {
    if (!ud || !state) return;

    ud->total++;
    ud->remaining++;
    ud->futures.push_back(state);
    ud->futurePaths.push_back(path);

    const int status = state->status.load();
    if (status == static_cast<int>(AssetLoader::FutureStatus::Pending)) {
        // 异步路径: 给本 future 一个独立的 ud 引用 ref
        lua_pushvalue(L, batchUdStackIdx);
        const int subRef = luaL_ref(L, LUA_REGISTRYINDEX);
        AssetLoader::RegisterCallback(state,
                                       BatchSubItemDispatcher_,
                                       L,
                                       subRef);
    } else {
        // 立即 Ready / Error: 同步更新 ud, 不走 dispatcher
        if (status == static_cast<int>(AssetLoader::FutureStatus::Ready)) {
            ud->succ++;
        } else {
            ud->errors.push_back({
                path,
                state->errorMsg.empty() ? "unknown error" : state->errorMsg,
            });
        }
        ud->remaining--;
    }
}

/// 校验 manifest[fieldName] 是 array (table); 不是 table 时报错; 缺省时返 false 让调用者跳过
static bool TryGetArrayField_(lua_State* L,
                               int manifestIdx,
                               const char* fieldName,
                               int* outArrayIdx) {
    lua_getfield(L, manifestIdx, fieldName);
    int t = lua_type(L, -1);
    if (t == LUA_TNIL) {
        lua_pop(L, 1);
        return false;
    }
    if (t != LUA_TTABLE) {
        lua_pop(L, 1);
        luaL_error(L, "manifest.%s must be table or nil (got %s)", fieldName, lua_typename(L, t));
        return false;  // unreachable
    }
    *outArrayIdx = lua_gettop(L);  // 数组 table 留在栈上
    return true;
}

/// 处理 string array 类字段 (images / sounds / cubeLUTs / haldLUTs)
/// 调用者: lua array table 已在栈顶; 处理完 pop
static void ProcessStringArray_(lua_State* L,
                                 int arrayIdx,
                                 BatchHandleUd* ud,
                                 int batchUdStackIdx,
                                 const char* fieldName,
                                 std::shared_ptr<AssetLoader::FutureState> (*loadFn)(const char*)) {
    const int n = static_cast<int>(lua_objlen(L, arrayIdx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, arrayIdx, i);
        // 严格 string 类型 (lua_isstring 对 number 也返 true, 此处需要拒绝 number)
        if (lua_type(L, -1) != LUA_TSTRING) {
            int et = lua_type(L, -1);
            lua_pop(L, 1);
            luaL_error(L, "manifest.%s[%d] must be string (got %s)",
                       fieldName, i, lua_typename(L, et));
            return;
        }
        std::string path = lua_tostring(L, -1);
        lua_pop(L, 1);

        auto state = loadFn(path.c_str());
        AppendOne_(L, ud, batchUdStackIdx, path, std::move(state));
    }
    lua_pop(L, 1);  // pop array table
}

/// 处理 fonts array: { {path=..., size=16}, ... }
static void ProcessFontsArray_(lua_State* L,
                                int arrayIdx,
                                BatchHandleUd* ud,
                                int batchUdStackIdx) {
    const int n = static_cast<int>(lua_objlen(L, arrayIdx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, arrayIdx, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "manifest.fonts[%d] must be table {path=..., size=...}", i);
            return;
        }

        lua_getfield(L, -1, "path");
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2);
            luaL_error(L, "manifest.fonts[%d].path must be string", i);
            return;
        }
        std::string path = lua_tostring(L, -1);
        lua_pop(L, 1);

        float size = 16.0f;
        lua_getfield(L, -1, "size");
        if (lua_isnumber(L, -1)) size = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_pop(L, 1);  // pop entry table

        auto state = AssetLoader::LoadFontAsync(path.c_str(), size);
        AppendOne_(L, ud, batchUdStackIdx, path, std::move(state));
    }
    lua_pop(L, 1);  // pop array table
}

/// 处理 meshes array: { {path=..., primIdx=0, withMaterial=false}, ... }
static void ProcessMeshesArray_(lua_State* L,
                                 int arrayIdx,
                                 BatchHandleUd* ud,
                                 int batchUdStackIdx) {
    const int n = static_cast<int>(lua_objlen(L, arrayIdx));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, arrayIdx, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "manifest.meshes[%d] must be table {path=..., primIdx=..., withMaterial=...}", i);
            return;
        }

        lua_getfield(L, -1, "path");
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2);
            luaL_error(L, "manifest.meshes[%d].path must be string", i);
            return;
        }
        std::string path = lua_tostring(L, -1);
        lua_pop(L, 1);

        int primIdx = 0;
        lua_getfield(L, -1, "primIdx");
        if (lua_isnumber(L, -1)) primIdx = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);

        bool withMaterial = false;
        lua_getfield(L, -1, "withMaterial");
        if (lua_isboolean(L, -1)) withMaterial = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_pop(L, 1);  // pop entry table

        auto state = AssetLoader::LoadGLTFAsync(path.c_str(), primIdx, withMaterial);
        AppendOne_(L, ud, batchUdStackIdx, path, std::move(state));
    }
    lua_pop(L, 1);  // pop array table
}

// ==================== Lua API ====================

/// Light.AssetLoader.Preload(manifest [, totalCb]) -> BatchHandle
///
/// manifest = {
///     images   = { "a.png", ... },              -- string array
///     sounds   = { "s.wav", ... },              -- string array
///     cubeLUTs = { "lut.cube", ... },           -- string array
///     haldLUTs = { "hald.png", ... },           -- string array
///     fonts    = { {path="f.ttf", size=16}, ... },                       -- table array
///     meshes   = { {path="m.glb", primIdx=0, withMaterial=true}, ... },  -- table array
/// }
/// totalCb = function(succ, fail, errors) ... end  -- 可选
static int l_AssetLoader_Preload(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    // 第二参 (totalCb): function 或 nil 或 缺省
    int cbRef = -1;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (!lua_isfunction(L, 2)) {
            return luaL_argerror(L, 2, "totalCb must be function or nil");
        }
        lua_pushvalue(L, 2);
        cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // 创建 BatchHandle userdata + placement-new
    void* mem = lua_newuserdata(L, sizeof(BatchHandleUd));
    BatchHandleUd* ud = new (mem) BatchHandleUd();
    luaL_getmetatable(L, kBatchHandleMT);
    lua_setmetatable(L, -2);
    const int batchUdStackIdx = lua_gettop(L);

    ud->L = L;
    ud->totalCbRef = cbRef;

    // 遍历 6 字段; 每个字段独立处理 (任意字段缺省 → 跳过)
    int arrayIdx = 0;
    if (TryGetArrayField_(L, 1, "images", &arrayIdx)) {
        ProcessStringArray_(L, arrayIdx, ud, batchUdStackIdx,
                             "images", &AssetLoader::LoadImageAsync);
    }
    if (TryGetArrayField_(L, 1, "sounds", &arrayIdx)) {
        ProcessStringArray_(L, arrayIdx, ud, batchUdStackIdx,
                             "sounds", &AssetLoader::LoadSoundAsync);
    }
    if (TryGetArrayField_(L, 1, "cubeLUTs", &arrayIdx)) {
        ProcessStringArray_(L, arrayIdx, ud, batchUdStackIdx,
                             "cubeLUTs", &AssetLoader::LoadCubeLUTAsync);
    }
    if (TryGetArrayField_(L, 1, "haldLUTs", &arrayIdx)) {
        ProcessStringArray_(L, arrayIdx, ud, batchUdStackIdx,
                             "haldLUTs", &AssetLoader::LoadHaldLUTAsync);
    }
    if (TryGetArrayField_(L, 1, "fonts", &arrayIdx)) {
        ProcessFontsArray_(L, arrayIdx, ud, batchUdStackIdx);
    }
    if (TryGetArrayField_(L, 1, "meshes", &arrayIdx)) {
        ProcessMeshesArray_(L, arrayIdx, ud, batchUdStackIdx);
    }

    // 空 manifest 或 全部立即完成 → 主动触发总 cb
    if (ud->remaining <= 0 && !ud->cancelled) {
        FireTotalCb_(ud);
    }

    // BatchHandle ud 在栈顶 (batchUdStackIdx 位置), 返 1
    return 1;
}

// ==================== BatchHandle 方法 ====================

/// :GetProgress() -> done(int), total(int), errors(int)
static int l_BatchHandle_GetProgress(lua_State* L) {
    BatchHandleUd* ud = CheckBatchUd_(L, 1);
    const int done = ud->total - ud->remaining;
    lua_pushinteger(L, done);
    lua_pushinteger(L, ud->total);
    lua_pushinteger(L, static_cast<int>(ud->errors.size()));
    return 3;
}

/// :IsDone() -> boolean
static int l_BatchHandle_IsDone(lua_State* L) {
    BatchHandleUd* ud = CheckBatchUd_(L, 1);
    lua_pushboolean(L, ud->remaining <= 0 ? 1 : 0);
    return 1;
}

/// :Cancel() -> nil
/// advisory: 标记 cancelled; 子 future 仍跑完, 但总 cb 不再触发
static int l_BatchHandle_Cancel(lua_State* L) {
    BatchHandleUd* ud = CheckBatchUd_(L, 1);
    ud->cancelled = true;
    // 释放 totalCbRef 防内存泄漏
    if (ud->totalCbRef >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->totalCbRef);
        ud->totalCbRef = -1;
    }
    return 0;
}

/// __tostring -> "Light.AssetLoader.BatchHandle(d/N[, cancelled])"
static int l_BatchHandle_tostring(lua_State* L) {
    BatchHandleUd* ud = GetBatchUd_(L, 1);
    if (!ud) {
        lua_pushstring(L, "Light.AssetLoader.BatchHandle(invalid)");
        return 1;
    }
    char buf[96];
    const int done = ud->total - ud->remaining;
    snprintf(buf, sizeof(buf),
             "Light.AssetLoader.BatchHandle(%d/%d%s)",
             done,
             ud->total,
             ud->cancelled ? ", cancelled" : "");
    lua_pushstring(L, buf);
    return 1;
}

/// __gc: 显式析构 BatchHandleUd (placement-new 对应)
/// 释放 totalCbRef + futures vector (子 future shared_ptr 引用计数 - 1)
/// 注意: sub-cb 的 luaL_ref 已在 Tick 内逐个 unref, 不需在 gc 处再统一处理
static int l_BatchHandle_gc(lua_State* L) {
    BatchHandleUd* ud = GetBatchUd_(L, 1);
    if (!ud) return 0;
    if (ud->totalCbRef >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->totalCbRef);
        ud->totalCbRef = -1;
    }
    ud->~BatchHandleUd();
    return 0;
}

}  // anonymous namespace

// ==================== 模块注册 ====================

extern "C" LIGHT_API int luaopen_Light_AssetLoader(lua_State* L) {
    // 注册 BatchHandle 元表
    if (luaL_newmetatable(L, kBatchHandleMT)) {
        // __index = methods table
        lua_newtable(L);
        static const luaL_Reg kMethods[] = {
            { "GetProgress", l_BatchHandle_GetProgress },
            { "IsDone",      l_BatchHandle_IsDone      },
            { "Cancel",      l_BatchHandle_Cancel      },
            { nullptr,       nullptr                   },
        };
        luaL_setfuncs(L, kMethods, 0);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, l_BatchHandle_gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, l_BatchHandle_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);

    // 注册 Light.AssetLoader 子表
    static const luaL_Reg kModuleFns[] = {
        { "Preload", l_AssetLoader_Preload },
        { nullptr,   nullptr               },
    };
    LT::RegisterModule(L, "AssetLoader", kModuleFns);
    return 1;
}
