/**
 * @file light_lighting2d.cpp
 * @brief Phase E.1.3 — Light.Lighting2D C++ 模块实现
 *
 * 维护 16 个 light slot + ambient + enabled 开关的单例状态.
 * E.1.4 会在本文件末尾追加 Lua binding (AddPointLight / AddSpotLight / ...).
 * E.1.5 会把 UploadToShader 里的 no-op 替换为对 RenderBackend 虚接口的真正调用.
 *
 * 设计约束:
 *   - POD State + 文件内 static 单例 (不分配堆内存, 不加锁, 单线程使用)
 *   - id 空间 1..MAX_LIGHTS (0 保留给"失败" 返回值), 内部 idx = id - 1
 *   - Add 挑第一个 type==INACTIVE 的 slot, 不保证 id 连续 (Remove 后的 slot 被复用)
 *   - Update 用"全字段覆盖"语义; Lua 层 (E.1.4) 自行处理部分字段合并
 */

#include "light_lighting2d.h"

namespace Lighting2D {

// ==================== 单例状态 ====================
// 文件内 static, 默认初始化等价于 State{}:
//   enabled=true, 所有 lights[i].type=INACTIVE, active_count=0, ambient={0,0,0}
static State g_state;

State* GetState() {
    return &g_state;
}

// ==================== Enabled / Ambient ====================

void SetEnabled(bool v) {
    g_state.enabled = v;
}

bool IsEnabled() {
    return g_state.enabled;
}

void SetAmbient(float r, float g, float b) {
    g_state.ambient[0] = r;
    g_state.ambient[1] = g;
    g_state.ambient[2] = b;
}

void GetAmbient(float& r, float& g, float& b) {
    r = g_state.ambient[0];
    g = g_state.ambient[1];
    b = g_state.ambient[2];
}

// ==================== Add / Update / Remove / Clear ====================

int Add(const Light& l) {
    // 非法 type (INACTIVE) 直接拒绝, 否则 active_count 会与 slot.type 不一致
    if (l.type == TYPE_INACTIVE) return 0;

    // 扫描第一个空闲 slot (O(MAX_LIGHTS) = O(16), 可接受)
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (g_state.lights[i].type == TYPE_INACTIVE) {
            g_state.lights[i] = l;
            ++g_state.active_count;
            return i + 1;  // id = index + 1 (1..16)
        }
    }
    // 16 个 slot 全满
    return 0;
}

bool Update(int id, const Light& fields) {
    // 越界 (id=0 保留 / id>16)
    if (id < 1 || id > MAX_LIGHTS) return false;
    const int idx = id - 1;

    // slot 当前已是 INACTIVE (被 Remove/Clear 过)
    if (g_state.lights[idx].type == TYPE_INACTIVE) return false;

    // fields.type 若为 INACTIVE, 语义上等价于 Remove(id); 但为了明确职责边界,
    // Update 不应承担 Remove 语义, 直接拒绝让调用方显式调 Remove.
    if (fields.type == TYPE_INACTIVE) return false;

    g_state.lights[idx] = fields;
    return true;
}

void Remove(int id) {
    // 幂等: 越界 / 已 INACTIVE 时均 no-op
    if (id < 1 || id > MAX_LIGHTS) return;
    const int idx = id - 1;
    if (g_state.lights[idx].type == TYPE_INACTIVE) return;

    g_state.lights[idx].type = TYPE_INACTIVE;
    --g_state.active_count;
}

void Clear() {
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        g_state.lights[i].type = TYPE_INACTIVE;
    }
    g_state.active_count = 0;
    // ambient 与 enabled 按设计保留 (符合 Lua ClearLights 语义: 只清 light, 不动全局配置)
}

// ==================== 查询 ====================

int GetCount() {
    return g_state.active_count;
}

int GetMax() {
    return MAX_LIGHTS;
}

// ==================== 后端上传 (E.1.3 占位) ====================
//
// E.1.5 实施时替换为:
//   if (!backend || !programId) return;
//   backend->UploadLighting2D(g_state.active_count, types, pos, dir, color,
//                              ranges, intensities, innerCoss, outerCoss,
//                              g_state.ambient);
// 具体 RenderBackend::UploadLighting2D 虚接口签名由 E.1.5 联合敲定.
void UploadToShader(RenderBackend* backend, uint32_t programId) {
    (void)backend;
    (void)programId;
    // 占位: 保持符号存在, 链接期 E.1.5 之前的代码不会 crash
}

}  // namespace Lighting2D
