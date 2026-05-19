# MAP M1.0 资源解码窗口示例

这个示例演示 `Light.Plugins.Map` 如何加载 M1.0 MAP 文件并在引擎窗口中绘制解码预览图。

## 文件结构

```text
samples/resource_decode_map_m10/
  main.lua      # 窗口示例代码
  setup.ps1     # 从仓库根目录 assets/ 复制素材和 runtime
  run.ps1       # 自动准备后运行示例
  assets/       # 本地素材目录，不提交 git
  runtime/      # 本地引擎运行文件，不提交 git
```

## 运行

```powershell
.\samples\resource_decode_map_m10\setup.ps1
.\samples\resource_decode_map_m10\run.ps1
```

窗口打开后会显示 `assets/1001.map` 解码出的地图预览，按 `ESC` 退出。
