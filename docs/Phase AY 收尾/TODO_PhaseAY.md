# TODO — Phase AY 收尾后剩余事项

> **6A 工作流 Stage 6 — Assess §最终 TODO 清单**
> 仅列出 Phase AY 中决策"延后"或仍待用户配置的事项, 已完成项见 `ACCEPTANCE_PhaseAY.md` / `FINAL_PhaseAY.md`.

---

## 一、需用户操作 (无代码工作)

无.

---

## 二、需用户配置才能验证

### 2.1 T04 — Audio FFmpeg fallback 完整实施 (P1, 延后)

**当前状态**: 诊断增强完成, 完整解码循环未实施.

**完整实施需要的输入**:
- FFmpeg DLL 真机环境:
  - `avformat-59.dll` (或对应 lib 在 macOS/Linux)
  - `avcodec-59.dll`
  - `avutil-57.dll`
  - `swresample-4.dll`
  - 放置位置: `light.exe` 同目录 (Windows) / `LD_LIBRARY_PATH` (Linux) / `DYLD_LIBRARY_PATH` (macOS)
- 测试音频文件:
  - 至少 1 个 AAC (.m4a / .aac)
  - 至少 1 个 OGG (.ogg)
  - 至少 1 个 Opus (.opus)
- CI 环境配置:
  - `.github/workflows/build-templates.yml` 增加 step 下载/解压 FFmpeg DLL
  - 添加 audio 解码 smoke

**实施代码框架** (light_av.cpp 中):
```cpp
// l_Audio_Call 当 miniaudio 失败时:
if (LoadFFmpeg() && ctx->codecCtx) {
    // 1. avcodec_open2 打开解码器
    g_ff.avcodec_parameters_to_context(ctx->codecCtx, codecpar);
    g_ff.avcodec_open2(ctx->codecCtx, decoder, nullptr);

    // 2. swr_alloc_set_opts + swr_init (重采样到 S16)
    SwrContext* swr = g_ff.swr_alloc_set_opts(...);
    g_ff.swr_init(swr);

    // 3. while av_read_frame: send_packet -> receive_frame -> swr_convert
    std::vector<int16_t> pcm;
    while (g_ff.av_read_frame(ctx->formatCtx, packet) >= 0) {
        if (packet.stream_index != ctx->streamIdx) continue;
        g_ff.avcodec_send_packet(ctx->codecCtx, packet);
        while (g_ff.avcodec_receive_frame(ctx->codecCtx, frame) == 0) {
            int16_t outBuf[8192];
            int converted = g_ff.swr_convert(swr, &outBuf, 8192, frame->data, frame->nb_samples);
            pcm.insert(pcm.end(), outBuf, outBuf + converted * channels);
        }
    }

    // 4. AudioBackend::LoadPCM
    ctx->audioHandle = AudioBackend::LoadPCM(pcm.data(), pcm.size() / channels,
                                              2 /*S16*/, channels, sampleRate);
}
```

预计工作量: ~200 行 + smoke 测试 + CI step.

**用户操作指引** (临时变通):
```bash
# 用 ffmpeg 命令行先转换为 WAV/MP3/FLAC, miniaudio 即可加载
ffmpeg.exe -i input.aac -f wav output.wav
```

---

## 三、长期演进候选 (无紧迫性)

### 3.1 真正的多 primitive 解析 (Phase AY+ 扩展)

**当前状态**: T06 已加 `pack.meshes` 数组字段, 但仅含 0 或 1 个元素.

**完整实施**:
- 每个 primitive 独立 ExtractSkinMesh + ExtractMorphTargets 调用
- `pack.meshes[i]` 含独立 SkinnedMesh ud, 各持自己的材质槽位
- 渲染端: `Light.Animation.DrawSkinnedMesh(meshes[i], animator, transform, materials[i])`
- 测试资产: 需 multi-material 角色 glTF (业界标准 RiggedSimple_MultiMat 等)

预计工作量: ~120 行 + 新 demo + smoke.

### 3.2 IK / blend tree (动画高级语义)

**当前状态**: 状态机 + crossfade 已能覆盖 80% 用例.

**候选方向**:
- Two-bone IK (foot placement, look-at)
- 1D / 2D blend tree (locomotion 动画混合)
- Animation layer (face / upper body / lower body 分层)

预计工作量: 单独 Phase (类似 Phase AV 规模).

### 3.3 Lua 绑定文档自动化

**当前状态**: 各模块文档分散在 `docs/Phase Ax/*.md`.

**候选方向**:
- 类 LDoc 风格的 `///` 注释自动提取
- 生成 `docs/lua_api/` 单一入口的 API 索引
- VS Code stub 用于 IntelliSense

预计工作量: 工具脚本 + 注释清理 (~300 行 Python + 全模块注释规范化).

---

## 四、Phase AY 流程改进经验

> 仅供下次清理批次参考, 非 TODO 项.

1. **批次划分**有效: 网络 / 动画 / 渲染 / 文档 4 批次让 commit 历史清晰.
2. **本地实跑**比依赖 CI 反馈快得多: T10 在本地跑通 212/212 后才提交, 避免 CI 红色循环.
3. **用户决策点**集中在:
   - 大工作量任务 (T10 实施 vs 跳过 vs 迷你版) → 主动 ask_user_question
   - 改动会影响其他任务 (LoadSkinnedGLTF 多次改动同一函数) → 用同一 commit 减少 diff
4. **延后任务文档化**比试图全部完成更负责任 (T04).

---

## 五、清单总览

| 类别 | 数量 | 说明 |
|------|------|------|
| Phase AY 阶段任务 | 10 实施 + 1 跳过 | 全部完成 |
| 用户操作待办 | 0 | 无 |
| 用户配置待办 | 1 | T04 (需 FFmpeg DLL + 测试音频) |
| 长期演进候选 | 3 | 多 primitive / IK / 文档自动化 |

**Phase AY 收尾完毕, 仓库可进入新阶段或保持当前清理状态等待生产使用.**
