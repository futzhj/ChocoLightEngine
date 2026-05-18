#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase G.1.5 — 生成最小 GLB 测试 fixture

输出: scripts/smoke/assets_g1_5/test_box_textured.glb (~1.2 KB)
内容:
  - 1 mesh / 1 primitive: 4 vertices quad + 6 indices (2 triangles)
  - PBR material (baseColorFactor=[0.8,0.5,0.2,1.0], metallic=0.7, roughness=0.3)
  - 1×1 红色 RGBA PNG 作为 baseColorTexture (embedded buffer_view)

为何用 Python 一次性生成而非 cgltf C++ 工具:
  - 无需新增 C++ 工具链, 仅 Python 标准库 (struct + zlib + json)
  - 一次性脚本; 输出 .glb 入仓后 CI 直接复用, 无需运行 generator
  - 跨平台 (Linux / macOS / Windows 都跑得动)

用法:
  cd e:/jinyiNew/Light
  py dev/gen_test_glb.py

期望输出:
  Generated scripts/smoke/assets_g1_5/test_box_textured.glb (XXXX bytes)
"""

import json
import os
import struct
import zlib
import sys

# ============================================================
# Step 1: 构造 1×1 红色 RGBA PNG (RFC 2083)
# ============================================================
def make_red_1x1_png() -> bytes:
    """生成最小 1×1 红色 (0xFF, 0x00, 0x00, 0xFF) RGBA PNG 字节流."""
    # PNG signature
    sig = b'\x89PNG\r\n\x1a\n'

    def chunk(ctype: bytes, data: bytes) -> bytes:
        # PNG chunk: length(4) + type(4) + data + crc(4 over type+data)
        crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + ctype + data + struct.pack('>I', crc)

    # IHDR: width=1, height=1, bitdepth=8, colortype=6(RGBA), 其余 0
    ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)
    # IDAT: 1 行扫描线, filter=0(None) + 4 字节像素
    raw = b'\x00' + b'\xFF\x00\x00\xFF'
    idat = zlib.compress(raw, level=9)
    # IEND: empty
    iend = b''

    return sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', iend)


# ============================================================
# Step 2: 构造 BIN chunk 数据 (vertex + index + PNG)
# ============================================================
def make_bin_chunk(png_bytes: bytes) -> tuple[bytes, dict]:
    """
    布局:
      [0  ..  48)  4 vec3 positions (quad: -1,-1 -> +1,+1 in XY plane)
      [48 ..  80)  4 vec2 UVs        (0,0 -> 1,1)
      [80 ..  92)  6 uint16 indices  (2 triangles: 0,1,2 + 0,2,3)
      [92 ..  96)  4 byte padding (4-byte align before next bufferView)
      [96 .. 96+N) PNG bytes
      [96+N..) padding to 4-byte align (chunk total length must be 4-byte aligned)

    返回 (bytes, byteOffsets dict).
    """
    parts = []
    offsets = {}

    # 4 个 vec3 positions (12 floats × 4 bytes = 48 bytes)
    offsets['pos_offset'] = 0
    positions = [
        (-1.0, -1.0, 0.0),
        ( 1.0, -1.0, 0.0),
        ( 1.0,  1.0, 0.0),
        (-1.0,  1.0, 0.0),
    ]
    for x, y, z in positions:
        parts.append(struct.pack('<fff', x, y, z))

    # 4 个 vec2 UVs (8 floats × 4 bytes = 32 bytes)
    offsets['uv_offset'] = 48
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    for u, v in uvs:
        parts.append(struct.pack('<ff', u, v))

    # 6 个 uint16 indices (12 bytes)
    offsets['idx_offset'] = 80
    indices = [0, 1, 2, 0, 2, 3]
    for i in indices:
        parts.append(struct.pack('<H', i))

    # 4 字节 padding 到 96 边界 (PNG 起始要 4 字节对齐)
    parts.append(b'\x00' * 4)

    # PNG image
    offsets['png_offset'] = 96
    offsets['png_size']   = len(png_bytes)
    parts.append(png_bytes)

    bin_data = b''.join(parts)

    # GLB chunk 总长度必须 4 字节对齐, 末尾用 0x00 padding
    pad_total = (4 - (len(bin_data) % 4)) % 4
    bin_data += b'\x00' * pad_total

    return bin_data, offsets


# ============================================================
# Step 3: 构造 glTF JSON (引用 BIN chunk 内偏移)
# ============================================================
def make_gltf_json(bin_offsets: dict, bin_total_len: int,
                   with_sampler: bool = False) -> bytes:
    """生成 glTF 2.0 JSON 描述文件 (UTF-8).

    with_sampler=True: 添加 samplers 数组 + texture 引用 sampler 索引.
      Phase G.1.5 T3 测试: 验证 cgltf sampler 透传 (mag/min/wrap_s/wrap_t)
      非默认值: NEAREST + MIRRORED_REPEAT, 与 glTF 默认 LINEAR + REPEAT 区分.
    """
    texture_obj = {"source": 0}
    samplers_arr = []
    if with_sampler:
        # 非默认 sampler — 验证透传效果:
        #   magFilter=NEAREST (9728)  / minFilter=NEAREST (9728)
        #   wrapS=MIRRORED_REPEAT (33648) / wrapT=CLAMP_TO_EDGE (33071)
        # 4 个字段都与 glTF 默认 (LINEAR + REPEAT) 不同, 便于 fixture-level 验证.
        samplers_arr = [{
            "magFilter": 9728,
            "minFilter": 9728,
            "wrapS":     33648,
            "wrapT":     33071,
        }]
        texture_obj["sampler"] = 0
    j = {
        "asset": {"version": "2.0", "generator": "ChocoLight Phase G.1.5 gen_test_glb.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                "indices": 2,
                "material": 0
            }]
        }],
        "materials": [{
            "name": "TestPBR",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.8, 0.5, 0.2, 1.0],
                "metallicFactor":  0.7,
                "roughnessFactor": 0.3,
                "baseColorTexture": {"index": 0}
            }
        }],
        "textures": [texture_obj],
        "images": [{"bufferView": 3, "mimeType": "image/png"}],
        "accessors": [
            {  # POSITION (vec3 float)
                "bufferView": 0, "byteOffset": 0,
                "componentType": 5126, "count": 4, "type": "VEC3",
                "min": [-1.0, -1.0, 0.0], "max": [1.0, 1.0, 0.0]
            },
            {  # TEXCOORD_0 (vec2 float)
                "bufferView": 1, "byteOffset": 0,
                "componentType": 5126, "count": 4, "type": "VEC2"
            },
            {  # INDICES (uint16 scalar)
                "bufferView": 2, "byteOffset": 0,
                "componentType": 5123, "count": 6, "type": "SCALAR"
            }
        ],
        "bufferViews": [
            {  # 0: positions
                "buffer": 0, "byteOffset": bin_offsets['pos_offset'],
                "byteLength": 48
            },
            {  # 1: UVs
                "buffer": 0, "byteOffset": bin_offsets['uv_offset'],
                "byteLength": 32
            },
            {  # 2: indices
                "buffer": 0, "byteOffset": bin_offsets['idx_offset'],
                "byteLength": 12
            },
            {  # 3: PNG
                "buffer": 0, "byteOffset": bin_offsets['png_offset'],
                "byteLength": bin_offsets['png_size']
            }
        ],
        "buffers": [{"byteLength": bin_total_len}]
    }
    if samplers_arr:
        j["samplers"] = samplers_arr
    # 紧凑 JSON (没有空格), 减少文件大小
    s = json.dumps(j, separators=(',', ':'), ensure_ascii=True)
    return s.encode('utf-8')


# ============================================================
# Step 4: 组装 GLB 容器 (header + JSON chunk + BIN chunk)
# ============================================================
def make_glb(json_bytes: bytes, bin_bytes: bytes) -> bytes:
    """GLB 2.0 binary format (https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#binary-gltf-layout)."""
    # JSON chunk: 必须 4 字节对齐, 用 0x20 (space) padding
    pad_json = (4 - (len(json_bytes) % 4)) % 4
    json_bytes += b' ' * pad_json   # 用空格填充, 与官方示例一致 (避免 JSON 解析报错)

    # BIN chunk: 必须 4 字节对齐 (已在 make_bin_chunk 内部对齐)

    # GLB header (12 bytes)
    magic   = 0x46546C67   # "glTF"
    version = 2
    length  = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)

    # JSON chunk header (8 bytes): chunkLength(4) + chunkType(4)
    json_chunk_header = struct.pack('<II', len(json_bytes), 0x4E4F534A)  # "JSON"

    # BIN chunk header (8 bytes): chunkLength(4) + chunkType(4)
    bin_chunk_header  = struct.pack('<II', len(bin_bytes), 0x004E4942)   # "BIN\0"

    return (
        struct.pack('<III', magic, version, length)
        + json_chunk_header + json_bytes
        + bin_chunk_header  + bin_bytes
    )


# ============================================================
# Main
# ============================================================
def _write_glb(out_path: str, with_sampler: bool) -> int:
    """生成单个 fixture (with_sampler 决定是否含 samplers 字段)."""
    png_bytes              = make_red_1x1_png()
    bin_bytes, bin_offsets = make_bin_chunk(png_bytes)
    json_bytes             = make_gltf_json(bin_offsets, len(bin_bytes),
                                            with_sampler=with_sampler)
    glb_bytes              = make_glb(json_bytes, bin_bytes)
    with open(out_path, 'wb') as f:
        f.write(glb_bytes)
    print(f"Generated {out_path} ({len(glb_bytes)} bytes, with_sampler={with_sampler})")
    return len(glb_bytes)


def main() -> int:
    out_dir = os.path.join('scripts', 'smoke', 'assets_g1_5')
    os.makedirs(out_dir, exist_ok=True)
    # 默认 fixture (无 samplers, 用 glTF 2.0 默认: LINEAR + REPEAT)
    _write_glb(os.path.join(out_dir, 'test_box_textured.glb'), with_sampler=False)
    # Phase G.1.5 T3 fixture (非默认 sampler: NEAREST + MIRRORED_REPEAT/CLAMP_TO_EDGE)
    _write_glb(os.path.join(out_dir, 'test_box_sampler.glb'),  with_sampler=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
