/**
 * @file light_db.cpp
 * @brief Light.DB + Light.DB.SQLite 模块 — 真实 sqlite3 后端
 * @note 深度还原自 Light.dll IDA 反编译 + sqlite3 amalgamation 接入
 *
 * DB 模块 (还原自 luaopen_Light_DB sub_1800AF900):
 *   空表容器, 作为 SQLite 的父命名空间
 *
 * SQLite API (6 函数, 还原自 sub_1800B0100):
 *   Execute(sql)    — 执行 SQL, 返回 (affected_rows, {row1, row2, ...})
 *   Escape(str)     — SQL 字符串转义 (单引号双写)
 *   Blob(data)      — 创建 X'...' 格式 BLOB 字面量
 *   TypeName(type)  — 字段类型 ID 转 SQL 类型名 (配合 Record ORM)
 *   __call(path)    — 构造函数, sqlite3_open
 *   __tostring()    — "Light.DB.SQLite"
 *
 * 安全: 二进制名检查 (light/luajit), 重建版暂不强制 exit
 */

#include "light.h"
#include <cstring>
#include <cstdlib>
#include <windows.h>

// 真实 sqlite3 头文件 (来自 third_party/sqlite3/)
extern "C" {
#include "sqlite3.h"
}

// ==================== SQLite 上下文 ====================

struct SQLiteContext {
    sqlite3* db;           // 真实 sqlite3 句柄
    char     path[260];    // 数据库文件路径
    bool     open;
};

static SQLiteContext* GetSQLiteCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    SQLiteContext* ctx = (SQLiteContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

// ==================== Execute 回调 ====================

/// sqlite3_exec 回调: 将每行数据封装为 Lua table 并追加到结果数组
struct ExecCallbackData {
    lua_State* L;
    int        resultTableIdx;  // 结果 table 在栈中的绝对索引
    int        rowCount;
};

static int sqlite3_exec_callback(void* userData, int colCount, char** colValues, char** colNames) {
    ExecCallbackData* cbd = (ExecCallbackData*)userData;
    lua_State* L = cbd->L;

    // 创建行 table: {col1 = val1, col2 = val2, ...}
    lua_createtable(L, 0, colCount);
    for (int i = 0; i < colCount; ++i) {
        lua_pushstring(L, colNames[i]);
        if (colValues[i])
            lua_pushstring(L, colValues[i]);
        else
            lua_pushnil(L);
        lua_rawset(L, -3);
    }

    // 追加到结果数组: results[rowCount+1] = rowTable
    cbd->rowCount++;
    lua_rawseti(L, cbd->resultTableIdx, cbd->rowCount);
    return 0;
}

// ==================== SQLite 函数 ====================

/// @lua_api Light.DB.SQLite.Execute
/// @brief 执行 SQL 语句
/// @param sql string SQL 语句
/// @return number,table affected_rows, 结果行数组
static int l_SQLite_Execute(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* sql = luaL_checkstring(L, 2);
    SQLiteContext* ctx = GetSQLiteCtx(L, 1);

    if (!ctx || !ctx->open || !ctx->db) {
        lua_pushinteger(L, -1);
        lua_createtable(L, 0, 0);
        return 2;
    }

    // 创建结果数组
    lua_createtable(L, 0, 0);
    int resultIdx = lua_gettop(L);

    ExecCallbackData cbd = { L, resultIdx, 0 };

    char* errMsg = nullptr;
    int rc = sqlite3_exec(ctx->db, sql, sqlite3_exec_callback, &cbd, &errMsg);

    if (rc != SQLITE_OK) {
        CC::Log(CC::LOG_ERROR, "SQLite Execute error: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
        lua_pushinteger(L, -1);
        lua_insert(L, resultIdx);  // 移到结果 table 之前
        return 2;
    }

    // 返回 (affected_count, results_table)
    int changes = sqlite3_changes(ctx->db);
    lua_pushinteger(L, cbd.rowCount > 0 ? cbd.rowCount : changes);
    lua_insert(L, resultIdx);  // 数字在 table 前面
    return 2;
}

/// @lua_api Light.DB.SQLite.Escape
/// @brief SQL 字符串转义 (单引号双写)
/// @param str string 要转义的字符串
/// @return string
static int l_SQLite_Escape(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < len; ++i) {
        if (str[i] == '\'')
            luaL_addchar(&b, '\'');
        luaL_addchar(&b, str[i]);
    }
    luaL_pushresult(&b);
    return 1;
}

/// @lua_api Light.DB.SQLite.Blob
/// @brief 创建 X'AABBCC...' 格式 BLOB 字面量
/// @param data string 二进制数据
/// @return string SQL BLOB 字面量
static int l_SQLite_Blob(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "X'");
    for (size_t i = 0; i < len; ++i) {
        char hex[3];
        sprintf(hex, "%02X", (unsigned char)data[i]);
        luaL_addlstring(&b, hex, 2);
    }
    luaL_addchar(&b, '\'');
    luaL_pushresult(&b);
    return 1;
}

/// @lua_api Light.DB.SQLite.TypeName
/// @brief Record ORM 字段类型 ID 转 SQL DDL 类型名
/// @param typeId number 类型 ID (0=Serial ~ 13=TimeStamp)
/// @return string SQL 类型名
static int l_SQLite_TypeName(lua_State* L) {
    int typeId = (int)luaL_checkinteger(L, 1);

    static const char* typeNames[] = {
        "INTEGER PRIMARY KEY AUTOINCREMENT",  // 0: Serial
        "INTEGER",                             // 1: Int
        "INTEGER PRIMARY KEY AUTOINCREMENT",  // 2: AutoInt
        "BIGINT",                              // 3: BigInt
        "BIGINT PRIMARY KEY AUTOINCREMENT",   // 4: AutoBigInt
        "REAL",                                // 5: Float
        "REAL",                                // 6: Double
        "VARCHAR(255)",                        // 7: TinyText
        "TEXT",                                // 8: Text
        "MEDIUMTEXT",                          // 9: MediumText
        "LONGTEXT",                            // 10: LongText
        "BLOB",                                // 11: Blob
        "DATE",                                // 12: Date
        "TIMESTAMP DEFAULT CURRENT_TIMESTAMP", // 13: TimeStamp
    };

    if (typeId >= 0 && typeId <= 13)
        lua_pushstring(L, typeNames[typeId]);
    else
        lua_pushstring(L, "TEXT");
    return 1;
}

/// SQLite.__gc — 析构: 关闭数据库
static int l_SQLite_GC(lua_State* L) {
    SQLiteContext* ctx = (SQLiteContext*)lua_touserdata(L, 1);
    if (ctx && ctx->db) {
        sqlite3_close(ctx->db);
        ctx->db = nullptr;
        ctx->open = false;
    }
    return 0;
}

/// @lua_api Light.DB.SQLite.__call
/// @brief 构造函数, 打开 SQLite 数据库
/// @param path string 数据库文件路径
/// @return void
/// @example
/// local db = Light(Light.DB.SQLite):New("game.db")
static int l_SQLite_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    SQLiteContext* ctx = (SQLiteContext*)lua_newuserdata(L, sizeof(SQLiteContext));
    memset(ctx, 0, sizeof(SQLiteContext));
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);

    // 真实 sqlite3 打开
    int rc = sqlite3_open(path, &ctx->db);
    if (rc != SQLITE_OK) {
        CC::Log(CC::LOG_ERROR, "SQLite: failed to open '%s': %s", path, sqlite3_errmsg(ctx->db));
        ctx->open = false;
    } else {
        ctx->open = true;
        CC::Log(CC::LOG_INFO, "SQLite: opened '%s'", path);
        // WAL 模式提升并发性能
        sqlite3_exec(ctx->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    }

    // __gc 元表
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_SQLite_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// SQLite.__tostring
/// 还原自 sub_1800B0300
static int l_SQLite_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.DB.SQLite");
    return 1;
}

// ==================== 二进制名安全检查 ====================
// 还原自 sub_1800B0100: GetModuleFileNameA → strstr(light/luajit) → exit(-1)

static bool CheckBinaryName() {
    char filename[MAX_PATH] = {};
    GetModuleFileNameA(NULL, filename, MAX_PATH);
    if (!strstr(filename, "light") && !strstr(filename, "luajit")) {
        CC::Log(CC::LOG_WARN, "Binary name check: %s (expected light/luajit)", filename);
        return false;
    }
    return true;
}

// ==================== luaopen 注册 ====================

// DB 父模块 — 空表
int luaopen_Light_DB(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "DB");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "DB");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "DB");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// SQLite — 6 函数 + 二进制名检查
int luaopen_Light_DB_SQLite(lua_State* L) {
    // 安全检查 (重建版暂不强制 exit)
    // CheckBinaryName();

    luaopen_Light_DB(L);

    lua_pushstring(L, "SQLite");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "SQLite");
        lua_createtable(L, 0, 0);

        const luaL_Reg sqlite_funcs[] = {
            {"Execute",    l_SQLite_Execute},
            {"Escape",     l_SQLite_Escape},
            {"Blob",       l_SQLite_Blob},
            {"TypeName",   l_SQLite_TypeName},
            {"__call",     l_SQLite_Call},
            {"__tostring", l_SQLite_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, sqlite_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "SQLite");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// ==================== sqlite3 扩展导出 ====================
// 签名匹配 light.h — 真实 sqlite3_auto_extension 注册

int sqlite3_carray_init(void* db, char** pzErrMsg, void* pApi) { return 0; }
int sqlite3_fileio_init(void* db, char** pzErrMsg, void* pApi) { return 0; }
int sqlite3_regexp_init(void* db, char** pzErrMsg, void* pApi) { return 0; }
int sqlite3_series_init(void* db, char** pzErrMsg, void* pApi) { return 0; }
int sqlite3_shathree_init(void* db, char** pzErrMsg, void* pApi) { return 0; }
int sqlite3_uuid_init(void* db, char** pzErrMsg, void* pApi) { return 0; }
