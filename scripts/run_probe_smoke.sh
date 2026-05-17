#!/usr/bin/env bash
# ============================================================================
# Phase G.1.1 — Shared GL Context probe 验证 wrapper (Linux / Mac)
# ============================================================================
# 用途:
#   本地开发或自托管 GPU CI runner 上验证 AssetLoader Shared GL Context 路径.
#   通过 grep 抓 stdout 关键字判 PASS/FAIL, 绕开 light 进程退出码不稳定的问题.
#
# 退出码:
#   0  - probe 日志命中
#   1  - 日志未命中
#   2  - 用户参数错误
#
# 用法:
#   ./scripts/run_probe_smoke.sh                                  # 默认路径
#   ./scripts/run_probe_smoke.sh path/to/light path/to/probe.lua  # 自定义
#
# CI 接入示例 (自托管 GPU runner):
#   - name: Phase G.1.1 probe smoke
#     run: ./scripts/run_probe_smoke.sh
# ============================================================================

set -euo pipefail

LIGHT_BIN="${1:-lumen-master/build/src/light/light}"
SCRIPT_LUA="${2:-scripts/smoke/asset_loader_async_probe.lua}"
TIMEOUT_SEC="${3:-10}"

# 参数校验
if [ ! -x "$LIGHT_BIN" ]; then
    echo "[FAIL] light binary not found or not executable: $LIGHT_BIN" >&2
    exit 2
fi
if [ ! -f "$SCRIPT_LUA" ]; then
    echo "[FAIL] probe script not found: $SCRIPT_LUA" >&2
    exit 2
fi

echo "[INFO] light    : $LIGHT_BIN"
echo "[INFO] script   : $SCRIPT_LUA"
echo "[INFO] timeout  : ${TIMEOUT_SEC}s"

# 用 timeout 命令限制最长运行时间; macOS 默认无 timeout, 退一步用 gtimeout (coreutils) 或直接跑
TMP_OUT=$(mktemp)
trap 'rm -f "$TMP_OUT"' EXIT

if command -v timeout >/dev/null 2>&1; then
    timeout "${TIMEOUT_SEC}" "$LIGHT_BIN" "$SCRIPT_LUA" > "$TMP_OUT" 2>&1 || true
elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout "${TIMEOUT_SEC}" "$LIGHT_BIN" "$SCRIPT_LUA" > "$TMP_OUT" 2>&1 || true
else
    echo "[WARN] no timeout command available, running without timeout" >&2
    "$LIGHT_BIN" "$SCRIPT_LUA" > "$TMP_OUT" 2>&1 || true
fi

# 抓 probe 关键字
if grep -q "AssetLoader: Shared GL Context enabled" "$TMP_OUT"; then
    echo "[PASS] probe enabled path: Shared GL Context worker upload + fence"
    exit 0
elif grep -q "AssetLoader: fallback to main-thread upload" "$TMP_OUT"; then
    echo "[PASS] probe fallback path: main-thread upload (driver / SDL doesn't support shared ctx)"
    echo "[INFO] 这通常发生在无 GPU / 无 GL 3.3 / 移动平台. 功能仍可用, 仅无 worker 加速收益"
    exit 0
else
    echo "[FAIL] probe key log not found in output" >&2
    echo "[INFO] 完整 stdout/stderr 如下:" >&2
    echo "----- output start -----" >&2
    cat "$TMP_OUT" >&2
    echo "----- output end -----" >&2
    exit 1
fi
