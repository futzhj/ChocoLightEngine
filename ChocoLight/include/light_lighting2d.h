/**
 * @file light_lighting2d.h
 * @brief Phase E.1.3 — Light.Lighting2D C++ 模块接口
 *
 * 管理 2D forward 多光状态 (16 个 light 数组 + 全局 ambient + enabled 开关),
 * 供 Lua API (E.1.4) / GL33Backend DrawLit2DQuad (E.1.5) / ECS Light2D system (E.1.6) 共享读写.
 *
 * 模块职责边界:
 *   - 本文件: POD struct + C++ API 声明 (无 GL / 无 Lua)
 *   - light_lighting2d.cpp: State 单例 + 所有 C++ API 实现 + (E.1.4 追加) Lua binding
 *   - render_gl33.cpp (E.1.5): 实现 RenderBackend::UploadLighting2D 真正把 state 上传到 programLit2D
 *
 * 线程模型: 单线程 (与 Lua/Render 主循环一致), 不加锁.
 */

#ifndef CHOCO_LIGHT_LIGHTING2D_H
#define CHOCO_LIGHT_LIGHTING2D_H

#include <cstdint>

class RenderBackend;  // 前向声明, 避免把整个 render_backend.h 引入

namespace Lighting2D {

/// 灯光类型常量 (与 VS_LIT2D/FS_LIT2D shader 内 uLightType[i] 语义一致)
enum LightType : int {
    TYPE_INACTIVE = 0,  ///< slot 空闲, shader 侧不参与计算
    TYPE_POINT    = 1,  ///< 点光 (忽略 dir/innerCos/outerCos)
    TYPE_SPOT     = 2,  ///< 聚光 (用 dir + innerCos/outerCos 构造 cone falloff)
};

/// 单 light 数据 (POD, 可 memcpy / 栈分配)
/// 与 shader uniform 布局一一对应 (见 render_gl33.cpp FS_LIT2D_SOURCE)
struct Light {
    int   type      = TYPE_INACTIVE;
    float pos[2]    = { 0.0f, 0.0f };     ///< 世界坐标 (x, y)
    float dir[2]    = { 1.0f, 0.0f };     ///< spot 方向 (需调用方归一化; point 忽略)
    float color[3]  = { 1.0f, 1.0f, 1.0f };
    float range     = 200.0f;             ///< 距离衰减半径 (d > range 时跳过)
    float intensity = 1.0f;               ///< 强度乘子
    float innerCos  = 1.0f;               ///< cos(innerAngle), spot only
    float outerCos  = 1.0f;               ///< cos(outerAngle), spot only
};

/// 模块硬上限 (与 shader uniform 数组大小 `[16]` 严格一致)
constexpr int MAX_LIGHTS = 16;

/// 全局 state (单例, 由 GetState() 访问)
struct State {
    bool     enabled      = true;
    Light    lights[MAX_LIGHTS];
    int      active_count = 0;                          ///< 当前非 inactive 的 slot 数
    float    ambient[3]   = { 0.0f, 0.0f, 0.0f };       ///< 全局环境光 (RGB)
    /// Phase E.2.1: 单调递增版本号; mutator 每次修改后 ++version.
    /// Backend 缓存 lastUploadedVersion, 相等则跳过 uniform 上传.
    /// 初值 1 + Backend 初值 0 保证首次 upload 一定 mismatch.
    /// uint32_t 溢出周期 ~497 天 (假设 1ms/update), 可忽略.
    uint32_t version      = 1;
};

// ==================== 状态访问 ====================

/// 获取单例 state 引用 (E.1.5 GL33Backend 读取 / 本模块内部写入)
State* GetState();

// ==================== C++ API (由 E.1.4 Lua binding + E.1.6 ECS system 共享调用) ====================

/// 全局启用/禁用 Lit 路径 (disable 时 DrawLit* 可退化为 Draw / 或维持 ambient 全黑)
void SetEnabled(bool v);
bool IsEnabled();

/// 环境光设置/查询
void SetAmbient(float r, float g, float b);
void GetAmbient(float& r, float& g, float& b);

/// 添加一个 light 到第一个空闲 slot
/// @param l 调用方构造好的 Light 数据 (必须 type != TYPE_INACTIVE)
/// @return lightId (1..MAX_LIGHTS) 成功; 0 = 已满 / type 非法
int Add(const Light& l);

/// 更新指定 slot 的全部字段 (id 是 Add 返回值)
/// @param fields 新数据 (type != TYPE_INACTIVE)
/// @return true 成功 / false (id 越界或 slot 已被 Remove)
bool Update(int id, const Light& fields);

/// 移除指定 slot (幂等: 对已 Remove 的 id 再调用是 no-op)
void Remove(int id);

/// 清空所有 light (active_count 归 0; ambient 与 enabled 不变)
void Clear();

/// 当前 active light 数 (便于 Lua 侧显示 / 测试断言)
int GetCount();

/// 硬上限常量 (= MAX_LIGHTS)
int GetMax();

/// Phase E.2.1 — 当前 state.version (单调递增, mutator 后 ++).
/// 主要用途: smoke 间接验证 backend dirty bit. Lua 层也暴露为 GetVersion().
uint32_t GetVersion();

// ==================== 后端上传 helper ====================

/// 把当前 state 上传到 programLit2D 的 uniform 数组
/// E.1.3: 仅占位 (no-op, 保持签名稳定, 避免 E.1.5 时改 API)
/// E.1.5: 在 render_gl33.cpp 实现时会调 backend 虚接口完成真正上传
void UploadToShader(RenderBackend* backend, uint32_t programId);

}  // namespace Lighting2D

#endif  // CHOCO_LIGHT_LIGHTING2D_H
