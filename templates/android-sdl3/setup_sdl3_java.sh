#!/bin/bash
# 从 SDL3 源码中提取 Java 文件到 Android 模板项目
# 用法: ./setup_sdl3_java.sh [SDL3_SOURCE_DIR]
#
# SDL3 源码位置:
#   - 手动克隆: ./SDL3
#   - FetchContent: ChocoLight/build/*/sdl3-src
#   - CI: 由 CI 脚本传入

set -e

SDL3_DIR="${1:-}"

# 自动搜索 FetchContent 缓存
if [ -z "$SDL3_DIR" ]; then
    SDL3_DIR=$(find "$(dirname "$0")/../../ChocoLight/build" -path "*/_deps/sdl3-src" -type d 2>/dev/null | head -1)
fi
if [ -z "$SDL3_DIR" ]; then
    echo "Usage: $0 <SDL3_SOURCE_DIR>"
    echo "  or run CMake configure first to populate FetchContent cache"
    exit 1
fi

SRC="$SDL3_DIR/android-project/app/src/main/java/org/libsdl/app"
DST="$(dirname "$0")/app/src/main/java/org/libsdl/app"

if [ ! -d "$SRC" ]; then
    echo "ERROR: SDL3 Java sources not found at: $SRC"
    exit 1
fi

mkdir -p "$DST"
cp -v "$SRC"/*.java "$DST/"

echo "SDL3 Java sources copied to $DST"
ls -la "$DST"
