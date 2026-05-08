/**
 * @file light_hidapi.cpp
 * @brief Light.Hidapi 模块 - 原始 HID 设备访问 (基于 SDL_hidapi)
 *
 * 用途: 访问任意 USB/蓝牙 HID 设备 (键盘宏、自定义控制器、硬件传感器、加密狗等).
 *
 * 平台覆盖: Windows / macOS / Linux / Android / iOS (BLE).
 *           Web/Emscripten 上 SDL_hid_init 通常返回非 0 (不支持).
 *
 * 核心设计:
 *   - SDL3 wchar_t* 字符串 (Win UTF-16, Linux/macOS UTF-32) 统一转 UTF-8 给 Lua.
 *   - 所有 read/write 数据用 Lua string (binary-safe) 传递.
 *   - dev / enumeration 节点全部以 lightuserdata 暴露.
 *
 * Lua API (17 fns):
 *   Light.Hidapi.Init() / Exit()                                   -> ok, err
 *   Light.Hidapi.DeviceChangeCount()                               -> int
 *   Light.Hidapi.Enumerate([vid], [pid])                           -> array, err
 *   Light.Hidapi.Open(vid, pid, [serial_utf8]) / OpenPath(path)    -> dev, err
 *   Light.Hidapi.Close(dev)                                        -> ok, err
 *   Light.Hidapi.Write(dev, data) / SendFeatureReport(dev, data)   -> bytes_written, err
 *   Light.Hidapi.Read(dev, max_len) / ReadTimeout(dev, max, ms)    -> data_string, err
 *   Light.Hidapi.GetFeatureReport(dev, max_len) /
 *   Light.Hidapi.GetInputReport(dev, max_len)                      -> data_string, err
 *   Light.Hidapi.SetNonblocking(dev, bool)                         -> ok, err
 *   Light.Hidapi.GetManufacturerString / GetProductString /
 *   Light.Hidapi.GetSerialNumberString(dev)                        -> str, err
 */
#include "light.h"

#include <SDL3/SDL.h>
#include <wchar.h>
#include <cstring>
#include <cstdint>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// wchar_t (Win=UTF-16, *nix=UTF-32) -> UTF-8 跨平台编码器.
// 不依赖 locale, 不依赖 SDL_iconv (避免编码名称差异).
// ============================================================
static std::string WCharToUtf8(const wchar_t* ws) {
    if (!ws) return std::string();
    std::string out;
    while (*ws) {
        uint32_t cp;
        if (sizeof(wchar_t) == 2) {
            // UTF-16: 处理代理对
            uint32_t hi = (uint16_t)*ws++;
            if (hi >= 0xD800 && hi <= 0xDBFF && *ws) {
                uint32_t lo = (uint16_t)*ws++;
                cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                cp = hi;
            }
        } else {
            // UTF-32: 直接 codepoint
            cp = (uint32_t)*ws++;
        }
        // UTF-8 编码
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

static SDL_hid_device* CheckDev(lua_State* L, int idx) {
    return lua_islightuserdata(L, idx) ? (SDL_hid_device*)lua_touserdata(L, idx) : nullptr;
}

// 通用错误返回: nil + err
static int PushErr(lua_State* L, const char* msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg ? msg : "unknown error");
    return 2;
}

// ==================== Init / Exit / DeviceChangeCount ====================

static int l_Hid_Init(lua_State* L) {
    int rc = SDL_hid_init();
    if (rc != 0) { lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Hid_Exit(lua_State* L) {
    int rc = SDL_hid_exit();
    if (rc != 0) { lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Hid_DeviceChangeCount(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SDL_hid_device_change_count());
    return 1;
}

// ==================== Enumerate ====================

static int l_Hid_Enumerate(lua_State* L) {
    // 默认 (0, 0) 列出全部
    unsigned short vid = (unsigned short)luaL_optinteger(L, 1, 0);
    unsigned short pid = (unsigned short)luaL_optinteger(L, 2, 0);
    SDL_hid_device_info* head = SDL_hid_enumerate(vid, pid);
    // head==NULL 表示 0 个设备 或 错误. 视为空列表 (无 err).
    lua_newtable(L);
    int idx = 1;
    for (SDL_hid_device_info* it = head; it != nullptr; it = it->next) {
        lua_newtable(L);
        lua_pushinteger(L, it->vendor_id);            lua_setfield(L, -2, "vendor_id");
        lua_pushinteger(L, it->product_id);           lua_setfield(L, -2, "product_id");
        lua_pushstring(L, it->path ? it->path : "");  lua_setfield(L, -2, "path");
        std::string s_serial = WCharToUtf8(it->serial_number);
        std::string s_manuf  = WCharToUtf8(it->manufacturer_string);
        std::string s_prod   = WCharToUtf8(it->product_string);
        lua_pushlstring(L, s_serial.data(), s_serial.size()); lua_setfield(L, -2, "serial_number");
        lua_pushlstring(L, s_manuf.data(),  s_manuf.size());  lua_setfield(L, -2, "manufacturer");
        lua_pushlstring(L, s_prod.data(),   s_prod.size());   lua_setfield(L, -2, "product");
        lua_pushinteger(L, it->release_number);     lua_setfield(L, -2, "release");
        lua_pushinteger(L, it->usage_page);         lua_setfield(L, -2, "usage_page");
        lua_pushinteger(L, it->usage);              lua_setfield(L, -2, "usage");
        lua_pushinteger(L, it->interface_number);   lua_setfield(L, -2, "interface_number");
        lua_rawseti(L, -2, idx++);
    }
    if (head) SDL_hid_free_enumeration(head);
    lua_pushnil(L);
    return 2;
}

// ==================== Open / OpenPath / Close ====================

static int l_Hid_Open(lua_State* L) {
    unsigned short vid = (unsigned short)luaL_checkinteger(L, 1);
    unsigned short pid = (unsigned short)luaL_checkinteger(L, 2);
    // serial 可选 (UTF-8 in, 转 wchar 给 SDL3 - 如果只 ASCII 简单转, 否则放弃)
    const char* serial_utf8 = luaL_optstring(L, 3, nullptr);
    SDL_hid_device* dev = nullptr;
    if (serial_utf8 && *serial_utf8) {
        // 简化处理: 仅 ASCII serial. 含非 ASCII 时返回错误.
        size_t n = strlen(serial_utf8);
        for (size_t i = 0; i < n; ++i) {
            if ((unsigned char)serial_utf8[i] >= 0x80) {
                return PushErr(L, "non-ASCII serial currently unsupported; use OpenPath instead");
            }
        }
        // wchar_t 数组 (栈上, n+1 wchar)
        wchar_t* wbuf = (wchar_t*)SDL_malloc((n + 1) * sizeof(wchar_t));
        if (!wbuf) return PushErr(L, "out of memory");
        for (size_t i = 0; i < n; ++i) wbuf[i] = (wchar_t)(unsigned char)serial_utf8[i];
        wbuf[n] = 0;
        dev = SDL_hid_open(vid, pid, wbuf);
        SDL_free(wbuf);
    } else {
        dev = SDL_hid_open(vid, pid, nullptr);
    }
    if (!dev) return PushErr(L, SDL_GetError());
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Hid_OpenPath(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    SDL_hid_device* dev = SDL_hid_open_path(path);
    if (!dev) return PushErr(L, SDL_GetError());
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Hid_Close(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid hid device handle"); return 2; }
    int rc = SDL_hid_close(dev);
    if (rc != 0) { lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ==================== Write / SendFeatureReport ====================

static int l_Hid_Write(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    int n = SDL_hid_write(dev, (const unsigned char*)data, len);
    if (n < 0) return PushErr(L, SDL_GetError());
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

static int l_Hid_SendFeatureReport(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    int n = SDL_hid_send_feature_report(dev, (const unsigned char*)data, len);
    if (n < 0) return PushErr(L, SDL_GetError());
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

// ==================== Read / ReadTimeout / GetFeatureReport / GetInputReport ====================

// 通用读取辅助: 调用 fn(dev, buf, max_len) 返回字节数或负数错误.
// 返回 lua: data_string + nil err, 或 nil + err. 0 字节 (nonblock 无数据) 返回空 string.
static int ReadHelper(lua_State* L,
                      int (*fn)(SDL_hid_device*, unsigned char*, size_t),
                      SDL_hid_device* dev, size_t max_len) {
    if (max_len > 65536) max_len = 65536;  // 安全上限
    std::string buf(max_len, '\0');
    int n = fn(dev, (unsigned char*)buf.data(), max_len);
    if (n < 0) return PushErr(L, SDL_GetError());
    lua_pushlstring(L, buf.data(), (size_t)n);
    lua_pushnil(L);
    return 2;
}

static int l_Hid_Read(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    size_t max_len = (size_t)luaL_checkinteger(L, 2);
    return ReadHelper(L, SDL_hid_read, dev, max_len);
}

static int l_Hid_ReadTimeout(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    size_t max_len = (size_t)luaL_checkinteger(L, 2);
    int ms = (int)luaL_checkinteger(L, 3);
    if (max_len > 65536) max_len = 65536;
    std::string buf(max_len, '\0');
    int n = SDL_hid_read_timeout(dev, (unsigned char*)buf.data(), max_len, ms);
    if (n < 0) return PushErr(L, SDL_GetError());
    lua_pushlstring(L, buf.data(), (size_t)n);
    lua_pushnil(L);
    return 2;
}

static int l_Hid_GetFeatureReport(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    size_t max_len = (size_t)luaL_checkinteger(L, 2);
    return ReadHelper(L, SDL_hid_get_feature_report, dev, max_len);
}

static int l_Hid_GetInputReport(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    size_t max_len = (size_t)luaL_checkinteger(L, 2);
    return ReadHelper(L, SDL_hid_get_input_report, dev, max_len);
}

// ==================== SetNonblocking ====================

static int l_Hid_SetNonblocking(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid hid device handle"); return 2; }
    int nonblock = lua_toboolean(L, 2) ? 1 : 0;
    int rc = SDL_hid_set_nonblocking(dev, nonblock);
    if (rc != 0) { lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ==================== GetXxxString ====================

// 通用字符串查询辅助: 调用 fn(dev, wbuf, maxlen) 取 wchar_t* 后转 utf8.
typedef int (*HidStringFn)(SDL_hid_device*, wchar_t*, size_t);

static int StringHelper(lua_State* L, HidStringFn fn, SDL_hid_device* dev) {
    wchar_t wbuf[256] = {0};
    int rc = fn(dev, wbuf, 256);
    if (rc != 0) return PushErr(L, SDL_GetError());
    std::string s = WCharToUtf8(wbuf);
    lua_pushlstring(L, s.data(), s.size());
    lua_pushnil(L);
    return 2;
}

static int l_Hid_GetManufacturerString(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    return StringHelper(L, SDL_hid_get_manufacturer_string, dev);
}

static int l_Hid_GetProductString(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    return StringHelper(L, SDL_hid_get_product_string, dev);
}

static int l_Hid_GetSerialNumberString(lua_State* L) {
    SDL_hid_device* dev = CheckDev(L, 1);
    if (!dev) return PushErr(L, "invalid hid device handle");
    return StringHelper(L, SDL_hid_get_serial_number_string, dev);
}

// ==================== luaopen_Light_Hidapi ====================

extern "C" LIGHT_API int luaopen_Light_Hidapi(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "Init",                    l_Hid_Init                    },
        { "Exit",                    l_Hid_Exit                    },
        { "DeviceChangeCount",       l_Hid_DeviceChangeCount       },
        { "Enumerate",               l_Hid_Enumerate               },
        { "Open",                    l_Hid_Open                    },
        { "OpenPath",                l_Hid_OpenPath                },
        { "Close",                   l_Hid_Close                   },
        { "Write",                   l_Hid_Write                   },
        { "Read",                    l_Hid_Read                    },
        { "ReadTimeout",             l_Hid_ReadTimeout             },
        { "SetNonblocking",          l_Hid_SetNonblocking          },
        { "SendFeatureReport",       l_Hid_SendFeatureReport       },
        { "GetFeatureReport",        l_Hid_GetFeatureReport        },
        { "GetInputReport",          l_Hid_GetInputReport          },
        { "GetManufacturerString",   l_Hid_GetManufacturerString   },
        { "GetProductString",        l_Hid_GetProductString        },
        { "GetSerialNumberString",   l_Hid_GetSerialNumberString   },
        { nullptr,                   nullptr                       },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
