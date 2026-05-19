# MAP 0.1M 资源解码窗口示例

这个示例演示 `Light.Plugins.Map` 如何加载 `mx_map` 下的 MAP 文件并在引擎窗口中绘制解码预览图。

## 文件结构

```text
samples/resource_decode_map_01m/
  main.lua      # 窗口示例代码
  setup.ps1     # 从仓库根目录 assets/mx_map/ 复制素材和 runtime
  run.ps1       # 自动准备后运行示例
  assets/       # 本地素材目录，不提交 git
  runtime/      # 本地引擎运行文件，不提交 git
```

## 运行

```powershell
.\samples\resource_decode_map_01m\setup.ps1
.\samples\resource_decode_map_01m\run.ps1
```

窗口打开后会默认显示 `assets/1001.map`，按 `1` / `2` 切换两个地图预览，按 `ESC` 退出。
