# Phase G.1.5 — 待办事项 (TODO)

> **创建日期**：2026-05-18
> **状态**：Phase G.1.5 收尾完成 (D1 + T2 + D2 + T3 + T6 + T1 + T7)。本文记录后续可继续优化方向。

---

## 一. 已完成 (本期 + 收尾)

### 主体 (Phase G.1.5)
- ✅ Mesh.LoadGLTFAsync 加 `withMaterial` 第三参数, 与同步 `LoadGLTF` 完全对齐
- ✅ 5 类 PBR texture 异步加载 (baseColor / metallicRoughness / normal / emissive / occlusion)
- ✅ 3 image 来源支持 (GLB embedded buffer_view / data: URI base64 / 相对文件路径)
- ✅ Worker shared_ctx 路径直接 GL 上传 (主线程 P95 < 1ms)
- ✅ Fallback 主线程上传路径
- ✅ MaterialDesc 数值字段完整透传 (color / metallic / roughness / emissive / normalScale / occlusionStrength / alphaMode / alphaCutoff / doubleSided / mode)
- ✅ Image 解码失败装饰性兜底 (slot=0 + log warn, mesh 仍 ready)
- ✅ Future:Get() 双值返回 (mesh, material) + Callback cb(mesh, material, err)
- ✅ 6 种 Lua 调用形式 (1-4 参数任意组合: path / primIdx / withMaterial / cb)
- ✅ 真实 .glb fixture (1192 bytes) + Python generator
- ✅ 8 用例 smoke (12 PASS)

### 收尾 (D1 + T2 + D2 + T3 + T6 + T1 + T7)
- ✅ **D1**: `docs/api/Light_Graphics.md` 加入 `Light.Graphics.Mesh.LoadGLTF` + `Light.Graphics.Mesh.LoadGLTFAsync` 完整 API 描述 (灵活签名 / 性能特征 / 路径分发 / 错误处理 / 示例)
- ✅ **T2**: PBR material texture 启用 mipmap (worker 路径 `glGenerateMipmap` + `LINEAR_MIPMAP_LINEAR`; fallback 路径 `RenderBackend::GenerateMipmap2D` 公共虚接口); 同步 `LoadGLTF` 路径同步对齐
- ✅ **D2**: `samples/demo_gltf_async/main.lua` 演示异步加载 fixture + Future poll / Callback 双风格切换 (R / M / F 键控制)
- ✅ **T3**: cgltf Texture Sampler 透传 (mag/min/wrap_s/wrap_t); `MaterialImageJob` 加 4 字段; `RenderBackend::SetTexture2DSampler` 公共虚接口 (default no-op, GL33 override); worker + fallback + 同步路径三路一致; min_filter mipmap-aware (非 mipmap 类型跳过 `glGenerateMipmap`); 默认值符合 glTF 2.0 规范 (mag=LINEAR / min=LINEAR_MIPMAP_LINEAR / wrap=REPEAT); 新 fixture `test_box_sampler.glb` + smoke Case 9 (13 PASS)
- ✅ **T6**: `samples/perf_async_gltf/` 真机性能测试 benchmark (主线程帧时 P50/P95/P99/Max + 平均加载耗时); CLI 可调模型路径 / repeat 次数 / loads_per_frame; 本地基线 (NVIDIA RTX GL 3.3 Core): P50=4.17ms / P95=4.79ms (LPF=5) / 4.37ms (LPF=1), 远低于 60fps budget 16.7ms
- ✅ **T1**: Worker Thread Pool 并行解码 5 类 PBR image (`std::async(launch::async)`); `DecodeMaterialImage_` 重构为返 `MaterialImageJob` 不再写 FutureState; 子 thread `stbi_set_flip_vertically_on_load_thread(0)` 独立状态; 本地基线 P95 略降 (4.79→4.35ms LPF=5 / 4.48→4.31ms repeat=200); 真实大图场景 (1024×1024+ PNG) 预计收益显著 (串行 50ms → 并行 10-15ms)
- ✅ **T7**: 失败注入测试 — `gen_test_glb.py` 加 `make_broken_png()` + `with_broken_png=True` 选项; 新 fixture `test_box_broken.glb` (1180 bytes, GLB 结构合法 + image bytes 损坏); smoke Case 10 验证装饰性兜底 (mesh+material ready / baseColor texId=0 / `stbi decode failed` warn 日志触发); 14 PASS

---

## 二. 可选优化 (后续候选)

### 2.1 功能扩展

#### T4. KTX/DDS 压缩纹理支持

- **现状**: cgltf 默认走 stbi (PNG/JPG/BMP/TGA), 不支持 KTX/DDS
- **场景**: 移动平台 ASTC/ETC2 压缩纹理
- **方案**: 加 cgltf KHR_texture_basisu extension + 引入 basisu / KTX 解码库
- **位置**: 第三方依赖 + asset_loader.cpp DecodeMaterialImage_
- **优先级**: 低 (移动端优化, 桌面用 PNG/JPG 已足够)
- **预估**: 16-24h (新依赖 + 解码 + 平台测试)

#### T5. Embedded Image 缓存 (相同 image 共享 GL texture)

- **现状**: 同 path / 同 base64 的 image 每次都重新解码 + 上传 (浪费)
- **场景**: 1 个 .gltf 多个 primitive 共享同一 baseColor texture
- **方案**: AssetLoader 内 `std::unordered_map<std::string, uint32_t>` 缓存已上传 image, key 是 file path 或 SHA256(buffer_view bytes)
- **位置**: `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp` 加全局 image cache
- **优先级**: 中 (具体场景才显著, 一般用户 1-2 个 mesh 不感知)
- **预估**: 6-8h (含缓存失效 + GC 策略)

---

## 三. 文档 / 工具

> D1 + D2 已完成 (见上一. 收尾区段). 本节保留供未来扩展.

### D3. Khronos sample model 集成 (后续)

- **现状**: demo_gltf_async 用 1×1 fixture, 视觉效果有限
- **方案**: 引入 Khronos GLTF Sample Model (DamagedHelmet / FlightHelmet) — 需 license check + 资源路径管理
- **优先级**: 低
- **预估**: 2-4h

---

## 四. 配置 / 环境

| 项 | 是否需要 | 说明 |
|----|----|----|
| Python 3.x | ✅ 已用 | dev/gen_test_glb.py 使用纯标准库 (struct + zlib + json), 任何 Python 3.x OK |
| .glb 入仓体积 | ✅ 已确认 | test_box_textured.glb 1192 bytes + test_box_sampler.glb 1280 bytes (T3) + test_box_broken.glb 1180 bytes (T7), 共 ~3.6 KB |
| 新依赖 | ❌ 无 | 复用现有 cgltf + stb_image + glad |
| CI runner GPU | ⚠️ 待验证 | windows-latest 软件 GL 应支持 1×1 PNG 上传, 待 CI watch 确认 |

---

## 五. 推荐处理顺序 (剩余)

| 优先级 | 任务 | 收益 |
|----|----|----|
| P0 | D3 Khronos sample model | demo 视觉提升 + perf_async_gltf 真机资源 |
| 待定 | T4 KTX/DDS | 移动端优化, 暂不紧急 |
| 已跳过 | T5 Image Cache | 当前架构下收益近 0 (LoadGLTFAsync 只加载单 primitive, 5 类 PBR texture 语义不同几乎不可能共享); 跨 task 缓存涉及 GL texture refcount, 生命周期管理复杂度高 |
