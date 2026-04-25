<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 21:18:33
 * @LastEditTime: 2026-04-25 23:06:14
-->
# TODO — SDL3 迁移待办事项

## 已完成

### 1. CI 全平台编译 ✅
- [x] 6 个平台全部编译通过 (Windows/Linux/macOS/Web/Android/iOS)
- [x] M5 阶段修复了 10 个构建问题 (详见 ACCEPTANCE)
- [x] 子模块注册: Android/iOS main 注册全部 26 个 `luaopen_*`

### 2. Android SDL3 Java 源码集成 ✅
- [x] CI 中 `setup_sdl3_java.sh` 正常工作
- [x] SDLActivity.java 从 FetchContent 缓存动态提取

## 高优先级

### 3. Android 真机测试
- [x] APK 安装到模拟器 (MuMu x86_64)
- [x] SDL3 初始化成功, `libchocolight.so` 加载正常
- [ ] **Lua 引擎运行时验证** — 子模块已注册, 等待新 APK 确认窗口创建
- [ ] 确认 GLES3 渲染正常 (矩形/圆形绘制)
- [ ] 确认 SDL3 事件循环 (触摸) 正常

### 4. iOS 真机测试
- [x] CI 编译通过 (Xcode 16.4, arm64)
- [ ] Xcode 真机 Build & Run 验证
- [ ] 确认 GLES3 上下文和引擎初始化

## 中优先级

### 5. Android 视频后端 (MediaPlayer + JNI)
- [ ] 实现 `video_backend_mediaPlayer.cpp` 或通过 JNI 调用 Java MediaPlayer
- [ ] 将视频帧上传到 GLES3 纹理 (SurfaceTexture → GL_TEXTURE_EXTERNAL_OES)
- **当前**: `CreateVideoBackend()` 返回 nullptr, 视频 Lua API 返回空

### 6. iOS 视频后端 (AVPlayer)
- [ ] 实现 `video_backend_avplayer.mm` 完整版
- [ ] AVPlayer + AVPlayerItemVideoOutput + CVPixelBuffer
- [ ] CVOpenGLESTextureCacheCreateTextureFromImage 上传 GL
- **当前**: 存根文件已创建, 返回 nullptr

### 7. 旧模板清理
- [ ] `templates/android/` (旧 Lua-only 模板) 考虑移到 `templates/legacy/`
- [ ] `templates/ios/` (旧 Lua-only 模板) 考虑移到 `templates/legacy/`
- **注意**: 旧模板仍可用于纯 Lua 脚本执行 (不含引擎), 保留可能有价值

## 低优先级

### 8. SDL3 版本更新
- [ ] 当前锁定 `release-3.2.0`, 后续可升级到更新版本
- [ ] 升级时需要测试所有平台

### 9. Emscripten 网络模块
- [ ] 当前网络模块在 Web/Android/iOS 为空存根
- [ ] Web 可通过 Emscripten fetch API 实现 HTTP
- [ ] Android/iOS 可通过原生 HTTP 库实现

### 10. 反调试模块移动端适配
- [ ] `light_antidebug.cpp` 当前仅 Windows 有效
- [ ] Android: 可添加 `ptrace` 检测 + `TracerPid` 检查
- [ ] iOS: 可添加 `sysctl` 反调试检测

## 缺少的配置

| 项目 | 说明 | 操作指引 |
|------|------|---------|
| Android NDK | CI 使用 `android-actions/setup-android@v3` 自动配置 | 本地需安装 Android Studio + NDK 27 |
| Xcode | iOS 构建需要 macOS + Xcode | CI 使用 `macos-latest` runner |
| Emscripten | Web 构建需要 emsdk | CI 使用 `mymindstorm/setup-emsdk@v14` |
| .env | 无 API Key 需求 | 当前项目无外部 API 依赖 |
