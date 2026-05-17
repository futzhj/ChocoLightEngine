# Phase G.1 异步资源加载验收记录

## 本次覆盖范围

- `AssetLoader::PushAsyncFuture` 已实现，统一创建 `Light.Graphics._AsyncFuture` userdata。
- `Future:Get()` 已支持通过 `ResultPusher` 推送资源结果。
- `Light.Graphics.Image.LoadAsync` 继续使用通用 Future helper。
- `Light.Graphics.HDR.LoadCubeLUTAsync` / `LoadHaldLUTAsync` 已接入 Future 与 callback。
- `Light.Graphics.Font.LoadAsync` 已接入 Future 与 callback。
- `Light.Graphics.Mesh.LoadGLTFAsync` 已接入 Future 与 callback。
- `Light.Audio.Sound.LoadAsync` 已接入 Future 与 callback。
- 新增 `scripts/smoke/asset_loader_async.lua`，覆盖 API surface 与缺失资源错误路径。
- Windows runtime smoke workflow 已接入 `asset_loader_async.lua`。

## 验证记录

| 验证项 | 结果 |
| --- | --- |
| `cmake --build build --config Release --target Light` | 通过 |
| `lightc.exe -p scripts/smoke/asset_loader_async.lua` | 通过 |
| `light.exe scripts/smoke/asset_loader_async.lua` | 通过 |

## 已知边界

- glTF 异步路径当前只返回基础 Mesh，不包含 material 与内嵌纹理。
- Mesh / Sound / Font 的 Future result 为所有权转移语义，首次 `Get()` 或 callback 会创建资源 userdata 并接管底层句柄。
- LUT 异步结果返回 LUT texture id，释放仍沿用 `Light.Graphics.HDR.DeleteLUT3D`。
