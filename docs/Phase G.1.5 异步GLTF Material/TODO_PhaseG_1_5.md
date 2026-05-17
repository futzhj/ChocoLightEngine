# Phase G.1.5 — 待办事项 (TODO)

> **创建日期**：2026-05-18
> **状态**：Phase G.1.5 主体已交付。本文记录未来可继续优化的方向。

---

## 一. 已完成 (本期)

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

---

## 二. 可选优化 (后续候选)

### 2.1 性能优化

#### T1. Worker Thread Pool 并行解码 image

- **现状**: 单 worker thread 串行 stbi_load_from_memory ×N (N 最多 5)
- **场景**: 5 张 1024×1024 PNG 总解码 ~50-100ms
- **方案**: thread pool (~4 worker), 并行解码不同 slot 的 image
- **位置**: `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:DecodeGLTF_` 内 5 次 DecodeMaterialImage_ 改并发
- **优先级**: 中 (主线程已不卡顿, 仅减少 worker 总用时)
- **预估**: 4-6h

#### T2. Mipmap 自动生成

- **现状**: worker glTexImage2D 后不调 glGenerateMipmap; 与同步 LoadGLTFImage 行为一致
- **场景**: PBR rendering 需要 mipmap 抗锯齿
- **方案**: WorkerUploadMesh_ 内串入 glGenerateMipmap; fallback 路径在 g_render->CreateTexture 后调
- **位置**: `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:WorkerUploadMesh_`
- **风险**: glGenerateMipmap 可能在某些 GLES 驱动下慢; 需 backend 选项控制
- **优先级**: 高 (PBR 视觉质量)
- **预估**: 2-4h

#### T3. cgltf Texture Sampler 支持

- **现状**: 忽略 cgltf sampler, 用默认 LINEAR + CLAMP
- **场景**: glTF 文件可能指定 NEAREST / REPEAT / MIRROR 等模式
- **方案**: 解析 `image->texture->sampler` (mag/min/wrapS/wrapT), 写入 MaterialImageJob 的 sampler 字段, worker glTexParameteri 用此值
- **位置**: `@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h:MaterialImageJob` 加 sampler 字段
- **优先级**: 中 (大多数 glTF 用默认值 OK)
- **预估**: 3-4h

### 2.2 功能扩展

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

### 2.3 测试 / 验证

#### T6. 大规模真机性能测试

- **现状**: 本地用 fixture 1×1 PNG 测试 (P95 < 1ms 不可测)
- **场景**: 真实游戏资源 1024×1024 - 4096×4096 PNG, 5 类 texture 全设
- **方案**: 加 `samples/perf_async_gltf/` benchmark sample, 类似 `perf_async_loader/` 但用 GLB fixture
- **位置**: `@e:/jinyiNew/Light/samples/perf_async_gltf/main.lua` (新建)
- **优先级**: 中 (验证设计性能模型)
- **预估**: 4-6h (含数据生成 + 报告输出)

#### T7. Worker Thread 失败注入测试

- **现状**: image 失败兜底 (slot=0 + log) 路径未明确测试
- **场景**: glGen 返 0, glTexImage2D 报错
- **方案**: 用 broken GLB fixture (含损坏 PNG) 验证 worker fallback 路径
- **位置**: `@e:/jinyiNew/Light/scripts/smoke/asset_loader_async_gltf.lua` 加 Case 9
- **优先级**: 低 (代码已有兜底, 但行为路径未端到端验证)
- **预估**: 2h (生成损坏 PNG fixture + smoke case)

---

## 三. 文档 / 工具

### D1. API_REFERENCE.md 更新 Mesh.LoadGLTFAsync

- **现状**: API_REFERENCE.md 可能未涵盖 G.1.5 新 with_material 参数
- **位置**: `@e:/jinyiNew/Light/docs/API_REFERENCE.md` 的 `Light.Graphics.Mesh` 章节
- **优先级**: 高
- **预估**: 30 分钟

### D2. Sample 演示

- **现状**: smoke 验证功能, 但用户层无完整 demo 参考
- **方案**: `samples/demo_gltf_async/` 加载真实带贴图模型 (Khronos sample) + 显示 mesh + material (PBR) + HUD 实时帧时间
- **优先级**: 中
- **预估**: 4-6h

---

## 四. 配置 / 环境

| 项 | 是否需要 | 说明 |
|----|----|----|
| Python 3.x | ✅ 已用 | dev/gen_test_glb.py 使用纯标准库 (struct + zlib + json), 任何 Python 3.x OK |
| .glb 入仓体积 | ✅ 已确认 | test_box_textured.glb 1192 bytes, 不影响 repo 体积 |
| 新依赖 | ❌ 无 | 复用现有 cgltf + stb_image + glad |
| CI runner GPU | ⚠️ 待验证 | windows-latest 软件 GL 应支持 1×1 PNG 上传, 待 CI watch 确认 |

---

## 五. 推荐处理顺序

| 优先级 | 任务 | 收益 |
|----|----|----|
| P0 | D1 API_REFERENCE 更新 | 必做, 用户文档同步 |
| P1 | T2 Mipmap 自动生成 | PBR 视觉质量直接提升 |
| P2 | D2 Sample 演示 | 接入参考, 提升采用率 |
| P3 | T3 Texture Sampler 支持 | 完整性 |
| P4 | T6 真机性能测试 | 验证设计性能模型 |
| P5 | T1 Worker Thread Pool | 进一步优化 worker 总用时 |
| P6 | T5 Image Cache | 内存优化 |
| P7 | T7 失败注入测试 | 代码已防御, 验证补充 |
| 待定 | T4 KTX/DDS | 移动端优化, 暂不紧急 |
