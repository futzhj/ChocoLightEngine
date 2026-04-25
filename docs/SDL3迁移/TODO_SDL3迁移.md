<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 21:18:33
 * @LastEditTime: 2026-04-25 23:17:41
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

## 已完成

### 5. Android 视频后端 (MediaPlayer + JNI) ✅
- [x] `video_backend_mediaPlayer.cpp`: JNI MediaPlayer + SurfaceTexture → OES → TEXTURE_2D
- [x] OES 拷贝着色器 (samplerExternalOES → FBO blit)

### 6. iOS 视频后端 (AVPlayer) ✅
- [x] `video_backend_avplayer.mm`: AVPlayer + AVPlayerItemVideoOutput + CVPixelBuffer
- [x] BGRA 像素直接 glTexSubImage2D 上传

### 7. 移动端网络支持 (HTTP/HTTPS/WebSocket) ✅
- [x] `light_platform_net_mobile.cpp`: POSIX 非阻塞 socket + select
- [x] 替代 libuv, 与现有 Lua API 100% 兼容
- [x] `light_network.cpp`: 移动端启用完整网络模块 (Http/HttpServer/Web)

## 中优先级

### 8. 旧模板清理 ✅
- [x] `templates/android/` → `templates/legacy/android/`
- [x] `templates/ios/` → `templates/legacy/ios/`
- 旧模板保留在 legacy/ 中, 可用于纯 Lua 脚本执行

## 低优先级

### 9. SDL3 版本更新
- [ ] 当前锁定 `release-3.2.0`, 后续可升级到更新版本
- [ ] 升级时需要测试所有平台

### 10. Emscripten 网络模块 
- [x] HTTP: emscripten_fetch 操作请求 (GET/POST/PUT/DELETE/HEAD)
- [x] WebSocket: JS WebSocket API (EM_JS 接口)
- [x] HttpServer: 浏览器不支持, 保留空存根
- [x] Lua API 与桌面/移动端 100% 兼容

### 11. 反调试模块移动端适配 ✅
- [x] Android: TracerPid + ptrace自占位 + 时间异常 + 调试器进程名检测
- [x] iOS: sysctl P_TRACED + 时间异常 + DYLD_INSERT_LIBRARIES注入检测
- [x] 策略与 Windows 一致: 渐进式 anomaly factor

## 缺少的配置

| 项目 | 说明 | 操作指引 |
|------|------|---------|
| Android NDK | CI 使用 `android-actions/setup-android@v3` 自动配置 | 本地需安装 Android Studio + NDK 27 |
| Xcode | iOS 构建需要 macOS + Xcode | CI 使用 `macos-latest` runner |
| Emscripten | Web 构建需要 emsdk | CI 使用 `mymindstorm/setup-emsdk@v14` |
| .env | 无 API Key 需求 | 当前项目无外部 API 依赖 |
