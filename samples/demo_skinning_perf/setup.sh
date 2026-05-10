#!/usr/bin/env bash
# samples/demo_skinning_perf/setup.sh
# Phase AW.x: 一键下载 Khronos RiggedSimple.glb 作为默认测试资产
#
# Usage:
#   ./setup.sh           # 默认下载
#   ./setup.sh -f        # 强制重新下载
#
# Asset source: KhronosGroup/glTF-Sample-Models (CC0 / Royalty-Free)

set -e

FORCE=0
if [ "$1" = "-f" ] || [ "$1" = "--force" ]; then
    FORCE=1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASSETS_DIR="$SCRIPT_DIR/assets"
TARGET="$ASSETS_DIR/character.glb"

if [ -f "$TARGET" ] && [ "$FORCE" = "0" ]; then
    echo "Asset already exists: $TARGET"
    echo "Use -f to re-download."
    exit 0
fi

mkdir -p "$ASSETS_DIR"

URL='https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/master/2.0/RiggedSimple/glTF-Binary/RiggedSimple.glb'
echo "Downloading from $URL ..."

if command -v curl >/dev/null 2>&1; then
    if ! curl -fL -o "$TARGET" "$URL"; then
        echo "curl download failed" >&2
        exit 1
    fi
elif command -v wget >/dev/null 2>&1; then
    if ! wget -O "$TARGET" "$URL"; then
        echo "wget download failed" >&2
        exit 1
    fi
else
    echo "Error: neither curl nor wget is available." >&2
    echo ""
    echo "Manually download any glTF 2.0 skinned mesh to:"
    echo "  $TARGET"
    echo ""
    echo "Recommended public assets:"
    echo "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedSimple"
    echo "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedFigure"
    echo "  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/CesiumMan"
    exit 1
fi

# Cross-platform file size (Linux GNU stat vs macOS BSD stat)
SIZE=$(stat -c%s "$TARGET" 2>/dev/null || stat -f%z "$TARGET" 2>/dev/null || echo "?")
echo "Downloaded $SIZE bytes -> $TARGET"
echo ""
echo "Now run:"
echo "  ../../Light-0.2.3/<platform>/light samples/demo_skinning_perf/main.lua"
