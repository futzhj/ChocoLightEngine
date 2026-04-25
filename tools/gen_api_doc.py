#!/usr/bin/env python3
"""
gen_api_doc.py — ChocoLight Lua API 文档生成器
扫描 ChocoLight/src/*.cpp 中的 @lua_api 注释块, 按模块分组生成 Markdown 文档
用法: python tools/gen_api_doc.py
"""

import os
import re
import sys
from collections import defaultdict
from pathlib import Path

# ==================== 配置 ====================

SCRIPT_DIR = Path(__file__).parent
PROJECT_ROOT = SCRIPT_DIR.parent
SRC_DIR = PROJECT_ROOT / "ChocoLight" / "src"
OUTPUT_DIR = PROJECT_ROOT / "docs" / "api"

# 注释块正则: 从 "/// @lua_api" 开始, 到下一个非 "///" 行结束
BLOCK_START = re.compile(r"^\s*///\s*@lua_api\s+(.+)$")
COMMENT_LINE = re.compile(r"^\s*///\s?(.*)$")

# 字段解析
TAG_RE = re.compile(r"^@(\w+)\s*(.*)")

# ==================== 解析 ====================

def parse_api_blocks(filepath: Path) -> list:
    """从单个 .cpp 文件中提取所有 @lua_api 注释块"""
    blocks = []
    current_block = None

    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            # 检测块开始
            m = BLOCK_START.match(line)
            if m:
                current_block = {"api_name": m.group(1).strip(), "tags": [], "source": filepath.name}
                blocks.append(current_block)
                continue

            # 在块内收集注释行
            if current_block is not None:
                cm = COMMENT_LINE.match(line)
                if cm:
                    content = cm.group(1)
                    current_block["tags"].append(content)
                else:
                    current_block = None  # 非注释行, 结束当前块

    return blocks


def parse_tags(raw_tags: list) -> dict:
    """将原始注释行解析为结构化字段"""
    result = {
        "brief": "",
        "params": [],
        "returns": "",
        "example": [],
        "notes": [],
        "since": "",
    }
    current_tag = None

    for line in raw_tags:
        tm = TAG_RE.match(line)
        if tm:
            tag_name = tm.group(1)
            tag_val = tm.group(2).strip()

            if tag_name == "brief":
                result["brief"] = tag_val
                current_tag = "brief"
            elif tag_name == "param":
                # @param name type description
                parts = tag_val.split(None, 2)
                param = {
                    "name": parts[0] if len(parts) > 0 else "",
                    "type": parts[1] if len(parts) > 1 else "",
                    "desc": parts[2] if len(parts) > 2 else "",
                }
                result["params"].append(param)
                current_tag = None
            elif tag_name == "return":
                result["returns"] = tag_val
                current_tag = "return"
            elif tag_name == "example":
                current_tag = "example"
            elif tag_name == "note":
                result["notes"].append(tag_val)
                current_tag = "note"
            elif tag_name == "since":
                result["since"] = tag_val
                current_tag = None
            else:
                current_tag = None
        else:
            # 续行 (属于上一个标签)
            if current_tag == "example":
                result["example"].append(line)
            elif current_tag == "brief" and line:
                result["brief"] += " " + line
            elif current_tag == "note" and line:
                result["notes"][-1] += " " + line

    return result

# ==================== 生成 ====================

def generate_module_doc(module_name: str, apis: list) -> str:
    """生成单个模块的 Markdown 文档"""
    lines = [f"# {module_name}\n"]

    for api in apis:
        info = parse_tags(api["tags"])
        lines.append(f"## `{api['api_name']}`\n")

        if info["brief"]:
            lines.append(f"{info['brief']}\n")

        if info["params"]:
            lines.append("### 参数\n")
            lines.append("| 名称 | 类型 | 说明 |")
            lines.append("|------|------|------|")
            for p in info["params"]:
                lines.append(f"| `{p['name']}` | `{p['type']}` | {p['desc']} |")
            lines.append("")

        if info["returns"]:
            lines.append(f"### 返回值\n\n`{info['returns']}`\n")

        if info["example"]:
            lines.append("### 示例\n")
            lines.append("```lua")
            for el in info["example"]:
                lines.append(el)
            lines.append("```\n")

        if info["notes"]:
            lines.append("### 备注\n")
            for n in info["notes"]:
                lines.append(f"- {n}")
            lines.append("")

        if info["since"]:
            lines.append(f"*Since: {info['since']}*\n")

        lines.append("---\n")

    return "\n".join(lines)


def main():
    if not SRC_DIR.exists():
        print(f"Error: source directory not found: {SRC_DIR}", file=sys.stderr)
        sys.exit(1)

    # 收集所有 API 块
    all_apis = []
    for cpp_file in sorted(SRC_DIR.glob("*.cpp")):
        blocks = parse_api_blocks(cpp_file)
        all_apis.extend(blocks)

    if not all_apis:
        print("Warning: no @lua_api blocks found. Add annotations to generate docs.")
        print("See docs/引擎升级/API_ANNOTATION_SPEC.md for annotation format.")
        sys.exit(0)

    # 按模块分组 (取 api_name 的前两段: Light.Graphics.Draw → Light.Graphics)
    modules = defaultdict(list)
    for api in all_apis:
        parts = api["api_name"].rsplit(".", 1)
        module = parts[0] if len(parts) > 1 else "Light"
        modules[module].append(api)

    # 生成输出
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # 生成各模块文档
    index_lines = ["# ChocoLight Lua API 文档\n", "## 模块列表\n"]

    for module_name in sorted(modules.keys()):
        apis = modules[module_name]
        safe_name = module_name.replace(".", "_")
        out_path = OUTPUT_DIR / f"{safe_name}.md"

        doc = generate_module_doc(module_name, apis)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(doc)

        index_lines.append(f"- [{module_name}]({safe_name}.md) ({len(apis)} 个函数)")
        print(f"  Generated: {out_path.relative_to(PROJECT_ROOT)} ({len(apis)} APIs)")

    # 生成索引
    index_path = OUTPUT_DIR / "README.md"
    with open(index_path, "w", encoding="utf-8") as f:
        f.write("\n".join(index_lines) + "\n")

    print(f"\nDone: {len(all_apis)} APIs across {len(modules)} modules → {OUTPUT_DIR.relative_to(PROJECT_ROOT)}/")


if __name__ == "__main__":
    main()
