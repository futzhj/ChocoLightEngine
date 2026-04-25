<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 21:18:33
 * @LastEditTime: 2026-04-25 21:55:27
-->
# TODO — SDL3 迁移待办事项

## 高优先级

### 1. CI 全平台验证
- [ ] 确认 GitHub Actions 6 个 job 全部通过
- [ ] 如有失败, 根据日志修复编译错误
- **操作**: 访问 https://github.com/futzhj/ChocoLightEngine/actions 查看最新构建结果

### 2. Android SDL3 Java 源码集成
- [ ] CI 中 SDL3 FetchContent 下载后, 需要将 Java 文件复制到模板项目
- [ ] `setup_sdl3_java.sh` 脚本需要在 CI 环境中验证
- [ ] SDLActivity.java 等文件不在 git 中, 每次构建动态获取
- **操作**: 首次本地构建时运行 `bash templates/android-sdl3/setup_sdl3_java.sh`

### 3. Android 真机测试
- [ ] 在 Android 设备/模拟器上验证 APK 安装和运行
- [ ] 确认 GLES3 上下文创建成功
- [ ] 确认 SDL3 事件循环 (触摸) 正常
- **操作**: `cd templates/android-sdl3 && ./gradlew installDebug`

### 4. iOS 模拟器/真机测试
- [ ] Xcode 打开 `templates/ios-sdl3/build/ChocoLightIOS.xcodeproj`
- [ ] 选择 iPhone 模拟器, Build & Run
- [ ] 确认 GLES3 上下文和引擎初始化日志
- **操作**: `cd templates/ios-sdl3 && cmake -B build -GXcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64`

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
