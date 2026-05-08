/**
 * @file light_dialog.cpp
 * @brief Light.Dialog 模块 - 文件 / 目录选择对话框 (基于 SDL_dialog)
 *
 * 平台覆盖:
 *   Windows  - 原生 IFileDialog
 *   macOS    - NSOpenPanel / NSSavePanel
 *   Linux    - XDG Desktop Portal (需要 DBus + 桌面环境; KDE/GNOME 都支持)
 *   Android  - Storage Access Framework (返回 content:// URI, 用 SDL_IOFromFile 打开)
 *   iOS      - UIDocumentPickerViewController
 *   Web      - 不支持 (浏览器 sandbox 内有自己的 input file 流程)
 *
 * 异步语义: ShowXXX 立即返回 request_id, 用户操作完后:
 *   - 若注册了 Lua callback: 在 PollEvents() 主线程调用中触发
 *   - 也可主动 PollResult(request_id) 取出结果
 *
 * 跨线程安全: SDL3 在 Linux 上经常在 DBus 线程触发 native callback.
 *   trampoline 只把结果 push 进 g_results 队列 (mutex 保护), 所有 lua_pcall
 *   都在 PollEvents 主线程调用中执行.
 *
 * Lua API (6 fn):
 *   Light.Dialog.ShowOpenFile(opts, [fn])   -> request_id, err
 *   Light.Dialog.ShowSaveFile(opts, [fn])   -> request_id, err
 *   Light.Dialog.ShowOpenFolder(opts, [fn]) -> request_id, err
 *   Light.Dialog.PollEvents()               -> dispatched_count
 *     主循环调用. 派发所有完成的 dialog 给注册的 Lua callback.
 *     Linux 上额外触发 SDL_PumpEvents (XDG Portal 依赖事件循环).
 *   Light.Dialog.PollResult(request_id)     -> result_table, 或 nil
 *     主动轮询. 取走结果. 若结果未就绪返回 nil.
 *   Light.Dialog.IsSupported()              -> bool
 *     该平台是否支持文件对话框.
 *
 * opts:
 *   filters         array of {name, pattern}, optional
 *   default_location string, optional
 *   allow_many       bool, default false (ShowSaveFile 忽略)
 *   title            string, optional (only via SDL_ShowFileDialogWithProperties)
 *
 * result_table:
 *   { state="ok"|"cancelled"|"error", files={paths...}, filter=int_idx, err=str_or_nil }
 *
 * 注: ChocoLight 当前不向 Lua 暴露 SDL_Window*, 所以所有 dialog 都是 non-modal.
 *     未来若 Light.UI.Window 暴露 native handle, 可在 opts 加 window=handle.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <unordered_map>
#include <deque>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdint>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// 内部状态
// ============================================================

struct DialogRequest {
    int        id;
    int        lua_ref;   // function ref in LUA_REGISTRYINDEX, 或 LUA_NOREF (轮询模式)
    lua_State* L;
    // filter / default_location 的存活必须覆盖整个 SDL dialog 生命周期.
    // 存于 unique_ptr 包裹的 DialogRequest 里, heap 地址不变.
    std::vector<SDL_DialogFileFilter> filter_storage;
    std::vector<std::string>          filter_strings;  // name + pattern 的 string 池
    std::string                       default_location_storage;
};

struct DialogResult {
    int                       id;
    std::string               state;   // "ok" / "cancelled" / "error"
    std::vector<std::string>  files;
    int                       filter;  // -1 = no filter
    std::string               err;     // when state == "error"
};

// 用 unique_ptr 避免 unordered_map rehash 时 DialogRequest 被 move 到新地址,
// 从而使传给 SDL3 的 filter 数组指针 / default_location c_str() 失效.
static std::unordered_map<int, std::unique_ptr<DialogRequest>> g_pending;
static std::deque<DialogResult>                                g_results;
static std::mutex g_dialog_mutex;
static std::atomic<int> g_next_id{1};

// 跨线程 callback - 只把结果入队, 不调 lua
static void SDLCALL DialogTrampoline(void* userdata,
                                     const char * const *filelist,
                                     int filter) {
    int req_id = (int)(intptr_t)userdata;

    DialogResult r;
    r.id     = req_id;
    r.filter = filter;

    if (filelist == nullptr) {
        // 错误
        r.state = "error";
        const char* e = SDL_GetError();
        r.err = e ? e : "unknown dialog error";
    } else if (filelist[0] == nullptr) {
        // 用户取消
        r.state = "cancelled";
    } else {
        r.state = "ok";
        for (int i = 0; filelist[i] != nullptr; ++i) {
            r.files.emplace_back(filelist[i]);
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_dialog_mutex);
        g_results.push_back(std::move(r));
        // 不在 callback 里删 g_pending entry (会 race), 由 PollEvents 删
    }
}

// 工具: 平台是否支持 dialog
static bool DialogSupported() {
    // SDL3 dummy backend (Web/某些移动) 上 ShowXXX 也会立即 callback error.
    // 这里粗略判断: 仅 Web/无 video subsystem 时不支持.
    const char* platform = SDL_GetPlatform();
    if (!platform) return true;  // 默认支持
    // SDL3 platform 名: Windows / macOS / Linux / Android / iOS / Emscripten
    if (std::strcmp(platform, "Emscripten") == 0) return false;
    return true;
}

// 工具: 解析 Lua opts.filters 表 -> 写入 req->filter_strings + filter_storage.
// 返回 true 表示 OK; 失败时 push err 到 stack 以供调用者返回.
static bool ParseFilters(lua_State* L, int opts_idx, DialogRequest* req) {
    lua_getfield(L, opts_idx, "filters");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "opts.filters must be a table");
        return false;
    }

    int nf = (int)lua_objlen(L, -1);
    if (nf == 0) { lua_pop(L, 1); return true; }

    // 预留 string 池: 每个 filter 占 2 个 slot. reserve 避免 push_back 中途扩容.
    req->filter_strings.reserve((size_t)nf * 2);
    req->filter_storage.reserve((size_t)nf);

    for (int i = 1; i <= nf; ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 2);
            lua_pushfstring(L, "opts.filters[%d] must be a table {name, pattern}", i);
            return false;
        }

        lua_getfield(L, -1, "name");
        const char* name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        req->filter_strings.emplace_back(name);
        lua_pop(L, 1);

        lua_getfield(L, -1, "pattern");
        const char* pat = lua_isstring(L, -1) ? lua_tostring(L, -1) : "*";
        req->filter_strings.emplace_back(pat);
        lua_pop(L, 1);

        lua_pop(L, 1);  // pop filter table
    }
    lua_pop(L, 1);  // pop filters table

    // 现在 string 池稳定, 构 SDL_DialogFileFilter 数组 (不会再扩容因为 reserve 足量)
    for (int i = 0; i < nf; ++i) {
        SDL_DialogFileFilter f;
        f.name    = req->filter_strings[(size_t)i * 2 + 0].c_str();
        f.pattern = req->filter_strings[(size_t)i * 2 + 1].c_str();
        req->filter_storage.push_back(f);
    }
    return true;
}

// 工具: 从 opts 表读 string 字段 (返回 nullptr 表示未设)
static const char* GetOptString(lua_State* L, int opts_idx, const char* key) {
    lua_getfield(L, opts_idx, key);
    const char* v = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
    lua_pop(L, 1);
    return v;
}

static bool GetOptBool(lua_State* L, int opts_idx, const char* key, bool dflt) {
    lua_getfield(L, opts_idx, key);
    bool v = lua_isnil(L, -1) ? dflt : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return v;
}

// ============================================================
// 公共: 启动一个 dialog 请求 (open / save / folder)
// 返回 request_id; 失败 push (nil + err)
// ============================================================
enum class DialogKind { Open, Save, Folder };

static int LaunchDialog(lua_State* L, DialogKind kind) {
    if (!DialogSupported()) {
        lua_pushnil(L);
        lua_pushstring(L, "file dialog not supported on this platform");
        return 2;
    }

    int opts_idx = 1;
    int cb_idx   = 2;

    // opts 必须为 table 或 nil
    if (!lua_isnil(L, opts_idx) && !lua_istable(L, opts_idx) && !lua_isnone(L, opts_idx)) {
        lua_pushnil(L);
        lua_pushstring(L, "opts must be a table or nil");
        return 2;
    }

    // callback 可选 (不传 / nil / function)
    int lua_ref = LUA_NOREF;
    int cb_type = lua_type(L, cb_idx);
    if (cb_type == LUA_TFUNCTION) {
        lua_pushvalue(L, cb_idx);
        lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else if (cb_type != LUA_TNONE && cb_type != LUA_TNIL) {
        lua_pushnil(L);
        lua_pushstring(L, "callback must be a function or nil");
        return 2;
    }

    int req_id = g_next_id.fetch_add(1);

    // 创建 request 直接 emplace 到 map (unique_ptr 包裹, rehash 不动 heap 实例)
    DialogRequest* req_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_dialog_mutex);
        auto up = std::unique_ptr<DialogRequest>(new DialogRequest());
        up->id      = req_id;
        up->lua_ref = lua_ref;
        up->L       = L;
        req_ptr = up.get();
        g_pending.emplace(req_id, std::move(up));
    }

    // 解析 opts 填 req_ptr
    bool allow_many = false;

    if (lua_istable(L, opts_idx)) {
        if (kind != DialogKind::Folder) {
            if (!ParseFilters(L, opts_idx, req_ptr)) {
                // 失败: ParseFilters 已在 stack top push 了 err
                {
                    std::lock_guard<std::mutex> lk(g_dialog_mutex);
                    g_pending.erase(req_id);
                }
                if (lua_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lua_ref);
                // 让 nil 在 err 之前 (返回格式 nil + err)
                lua_pushnil(L);
                lua_insert(L, -2);
                return 2;
            }
        }
        const char* loc = GetOptString(L, opts_idx, "default_location");
        if (loc) req_ptr->default_location_storage = loc;
        allow_many = GetOptBool(L, opts_idx, "allow_many", false);
    }

    // 从 req_ptr 中取出稳定的 filter 和 location 指针
    const SDL_DialogFileFilter* filters_ptr = req_ptr->filter_storage.empty()
        ? nullptr : req_ptr->filter_storage.data();
    int nfilters = (int)req_ptr->filter_storage.size();
    const char* loc_cstr = req_ptr->default_location_storage.empty()
        ? nullptr : req_ptr->default_location_storage.c_str();

    void* userdata = (void*)(intptr_t)req_id;

    // 调用 SDL3 (异步, 立即返回). filters_ptr / loc_cstr 需保活到 callback 触发.
    switch (kind) {
        case DialogKind::Open:
            SDL_ShowOpenFileDialog(DialogTrampoline, userdata,
                                   /*window*/ nullptr,
                                   filters_ptr, nfilters,
                                   loc_cstr, allow_many);
            break;
        case DialogKind::Save:
            // ShowSaveFile 不支持 allow_many
            SDL_ShowSaveFileDialog(DialogTrampoline, userdata,
                                   /*window*/ nullptr,
                                   filters_ptr, nfilters,
                                   loc_cstr);
            break;
        case DialogKind::Folder:
            SDL_ShowOpenFolderDialog(DialogTrampoline, userdata,
                                     /*window*/ nullptr,
                                     loc_cstr, allow_many);
            break;
    }

    lua_pushinteger(L, req_id);
    lua_pushnil(L);
    return 2;
}

static int l_Dialog_ShowOpenFile(lua_State* L)   { return LaunchDialog(L, DialogKind::Open); }
static int l_Dialog_ShowSaveFile(lua_State* L)   { return LaunchDialog(L, DialogKind::Save); }
static int l_Dialog_ShowOpenFolder(lua_State* L) { return LaunchDialog(L, DialogKind::Folder); }

// ============================================================
// PollEvents: 主线程派发完成的 dialog
// ============================================================
static int l_Dialog_PollEvents(lua_State* L) {
    // Linux XDG Portal 需要事件循环 pump (SDL3 文档明确要求)
    SDL_PumpEvents();

    // 1. snapshot 完成的 results
    std::deque<DialogResult> ready;
    {
        std::lock_guard<std::mutex> lk(g_dialog_mutex);
        ready.swap(g_results);
    }

    int dispatched = 0;
    for (auto& r : ready) {
        // 2. 找到对应的 request, 取出 lua_ref. 若该 req 是轮询模式则路给 PollResult.
        int lua_ref = LUA_NOREF;
        lua_State* CL = L;
        bool poll_mode = false;
        {
            std::lock_guard<std::mutex> lk(g_dialog_mutex);
            auto it = g_pending.find(r.id);
            if (it != g_pending.end()) {
                lua_ref = it->second->lua_ref;
                if (it->second->L) CL = it->second->L;
                if (lua_ref == LUA_NOREF) {
                    poll_mode = true;
                } else {
                    g_pending.erase(it);
                }
            } else {
                // 不应发生: req 被其他路径提前删了
                continue;
            }
        }

        if (poll_mode) {
            // 到 PollResult 里取
            std::lock_guard<std::mutex> lk(g_dialog_mutex);
            g_results.push_back(std::move(r));
            continue;
        }

        // 3. pcall 注册时的 fn(result_table)
        lua_rawgeti(CL, LUA_REGISTRYINDEX, lua_ref);
        // 构造 result table
        lua_newtable(CL);
        lua_pushstring(CL, r.state.c_str());
        lua_setfield(CL, -2, "state");
        lua_pushinteger(CL, r.filter);
        lua_setfield(CL, -2, "filter");
        if (!r.err.empty()) {
            lua_pushstring(CL, r.err.c_str());
            lua_setfield(CL, -2, "err");
        }
        // files 数组
        lua_newtable(CL);
        for (size_t i = 0; i < r.files.size(); ++i) {
            lua_pushstring(CL, r.files[i].c_str());
            lua_rawseti(CL, -2, (int)(i + 1));
        }
        lua_setfield(CL, -2, "files");

        if (lua_pcall(CL, 1, 0, 0) != 0) {
            const char* e = lua_tostring(CL, -1);
            SDL_Log("[Light.Dialog] callback error: %s", e ? e : "(no message)");
            lua_pop(CL, 1);
        }

        luaL_unref(CL, LUA_REGISTRYINDEX, lua_ref);
        ++dispatched;
    }

    lua_pushinteger(L, dispatched);
    return 1;
}

// ============================================================
// PollResult: 轮询模式 - 主动取走某 request 的结果
// ============================================================
static int l_Dialog_PollResult(lua_State* L) {
    int req_id = (int)luaL_checkinteger(L, 1);

    DialogResult r;
    bool found = false;
    int pending_ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> lk(g_dialog_mutex);
        for (auto it = g_results.begin(); it != g_results.end(); ++it) {
            if (it->id == req_id) {
                r = std::move(*it);
                g_results.erase(it);
                found = true;
                break;
            }
        }
        if (found) {
            // 同时清掉 pending entry (释放 filter storage)
            auto pit = g_pending.find(req_id);
            if (pit != g_pending.end()) {
                pending_ref = pit->second->lua_ref;
                g_pending.erase(pit);
            }
        }
    }
    if (pending_ref != LUA_NOREF) {
        // 锁外 unref (避免 锁与 Lua GC 交互)
        luaL_unref(L, LUA_REGISTRYINDEX, pending_ref);
    }

    if (!found) {
        lua_pushnil(L);
        return 1;
    }

    // 构造 result table
    lua_newtable(L);
    lua_pushstring(L, r.state.c_str());
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, r.filter);
    lua_setfield(L, -2, "filter");
    if (!r.err.empty()) {
        lua_pushstring(L, r.err.c_str());
        lua_setfield(L, -2, "err");
    }
    lua_newtable(L);
    for (size_t i = 0; i < r.files.size(); ++i) {
        lua_pushstring(L, r.files[i].c_str());
        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setfield(L, -2, "files");
    return 1;
}

static int l_Dialog_IsSupported(lua_State* L) {
    lua_pushboolean(L, DialogSupported() ? 1 : 0);
    return 1;
}

// ==================== luaopen_Light_Dialog ====================
extern "C" LIGHT_API int luaopen_Light_Dialog(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "ShowOpenFile",   l_Dialog_ShowOpenFile   },
        { "ShowSaveFile",   l_Dialog_ShowSaveFile   },
        { "ShowOpenFolder", l_Dialog_ShowOpenFolder },
        { "PollEvents",     l_Dialog_PollEvents     },
        { "PollResult",     l_Dialog_PollResult     },
        { "IsSupported",    l_Dialog_IsSupported    },
        { nullptr,          nullptr                 },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
