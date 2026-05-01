#!/usr/bin/env python3
"""
ChocoLight Lua API 文档生成器

扫描 ChocoLight/src/*.cpp 中的 /// @lua_api 注释块,
按模块归类生成 Markdown 文档。

用法:
    python tools/gen_api_docs.py
    python tools/gen_api_docs.py --output docs/API_REFERENCE.md

注释格式约定:
    /// @lua_api Light.Module.Function
    /// @brief 简短描述
    /// @param name type 描述
    /// @param name? type 可选参数
    /// @return type 描述
    /// @example
    /// local x = Light.Module.Function(...)
    static int l_Function(lua_State* L) { ... }
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

# ============================================================
# 注释块解析
# ============================================================

# /// @key value  风格的单行注释
TAG_RE = re.compile(r'^\s*///\s*@(\w+)(?:\s+(.*))?$')
# /// 普通描述行 (无 @tag)
TEXT_RE = re.compile(r'^\s*///\s?(.*)$')
# 普通注释结束: 非 /// 开头的行


class ApiEntry:
    """单个 API 入口"""

    def __init__(self):
        self.name = ""        # Light.Module.Function
        self.brief = ""       # 简短描述
        self.params = []      # [(name, type, desc, optional)]
        self.returns = []     # [(type, desc)]
        self.example = []     # 示例代码行
        self.notes = []       # 其他说明
        self.source_file = "" # 源文件 (相对路径)
        self.source_line = 0  # 源代码行号

    def module(self) -> str:
        """提取模块名: Light.Graphics.SetColor → Light.Graphics"""
        parts = self.name.rsplit('.', 1)
        return parts[0] if len(parts) == 2 else self.name

    def func_name(self) -> str:
        """提取函数名: Light.Graphics.SetColor → SetColor"""
        return self.name.rsplit('.', 1)[-1]


def parse_file(path: Path):
    """解析单个 cpp 文件, 返回 ApiEntry 列表"""
    entries = []
    text = path.read_text(encoding='utf-8', errors='replace')
    lines = text.split('\n')

    i = 0
    while i < len(lines):
        line = lines[i]
        m = TAG_RE.match(line)
        if m and m.group(1) == 'lua_api':
            # 找到一个 API 入口起始
            entry = ApiEntry()
            entry.name = (m.group(2) or '').strip()
            entry.source_file = str(path.name)
            entry.source_line = i + 1
            i += 1
            # 解析后续 /// 行直到非 /// 行
            in_example = False
            while i < len(lines):
                row = lines[i]
                tm = TAG_RE.match(row)
                if tm:
                    tag = tm.group(1)
                    val = (tm.group(2) or '').strip()
                    in_example = False
                    if tag == 'brief':
                        entry.brief = val
                    elif tag == 'param':
                        entry.params.append(parse_param(val))
                    elif tag == 'return':
                        entry.returns.append(parse_return(val))
                    elif tag == 'example':
                        in_example = True
                        if val:
                            entry.example.append(val)
                    elif tag == 'note' or tag == 'lua_note':
                        entry.notes.append(val)
                    elif tag == 'lua_api':
                        # 下一个 API, 回退一行
                        break
                    i += 1
                    continue
                tx = TEXT_RE.match(row)
                if tx:
                    body = tx.group(1)
                    if in_example:
                        entry.example.append(body)
                    elif body.strip():
                        # 接续 brief / 注释体
                        if entry.brief and not body.strip().startswith('@'):
                            entry.notes.append(body)
                        elif not entry.brief:
                            entry.brief = body.strip()
                    i += 1
                    continue
                # 非 /// 行, 注释块结束
                break
            entries.append(entry)
            continue
        i += 1
    return entries


def parse_param(val: str):
    """
    解析 @param 行:
        name type 描述
        name? type 描述
    返回 (name, type, desc, optional)
    """
    parts = val.split(None, 2)
    if len(parts) < 2:
        return (val.strip(), '', '', False)
    name = parts[0]
    optional = name.endswith('?')
    if optional:
        name = name[:-1]
    type_ = parts[1] if len(parts) > 1 else ''
    desc = parts[2] if len(parts) > 2 else ''
    return (name, type_, desc, optional)


def parse_return(val: str):
    """
    解析 @return 行:
        type 描述
    返回 (type, desc)
    """
    parts = val.split(None, 1)
    if len(parts) == 1:
        return (parts[0], '')
    return (parts[0], parts[1])


# ============================================================
# Markdown 输出
# ============================================================

def render_entry(entry: ApiEntry) -> str:
    """渲染单个 API 为 Markdown 块"""
    out = []
    out.append(f"### `{entry.name}`")
    out.append('')
    if entry.brief:
        out.append(f"**{entry.brief}**")
        out.append('')

    # 参数
    if entry.params:
        out.append("**参数:**")
        out.append('')
        out.append('| 名称 | 类型 | 描述 | 必需 |')
        out.append('|------|------|------|------|')
        for name, type_, desc, optional in entry.params:
            req = '否' if optional else '是'
            out.append(f'| `{name}` | `{type_}` | {desc} | {req} |')
        out.append('')

    # 返回值
    if entry.returns:
        out.append("**返回:**")
        out.append('')
        for type_, desc in entry.returns:
            out.append(f"- `{type_}` — {desc}" if desc else f"- `{type_}`")
        out.append('')

    # 示例
    if entry.example:
        # 去除前导空格
        cleaned = []
        for line in entry.example:
            cleaned.append(line)
        out.append('**示例:**')
        out.append('')
        out.append('```lua')
        out.extend(cleaned)
        out.append('```')
        out.append('')

    # 注释
    note_text = [n for n in entry.notes if n.strip()]
    if note_text:
        out.append('**说明:**')
        out.append('')
        for n in note_text:
            out.append(f"- {n}")
        out.append('')

    # 源文件位置
    out.append(f"<sub>📄 `{entry.source_file}:{entry.source_line}`</sub>")
    out.append('')
    return '\n'.join(out)


def render_module(module: str, entries: list) -> str:
    """渲染单个模块的所有 API"""
    out = []
    out.append(f"## {module}")
    out.append('')
    out.append(f"> 共 {len(entries)} 个 API")
    out.append('')
    # 函数索引
    out.append('**函数列表:**')
    out.append('')
    for e in entries:
        anchor = e.name.lower().replace('.', '').replace(' ', '-')
        out.append(f"- [`{e.func_name()}`](#{anchor}) — {e.brief or '(无描述)'}")
    out.append('')
    out.append('---')
    out.append('')
    # 详细
    for e in entries:
        out.append(render_entry(e))
        out.append('---')
        out.append('')
    return '\n'.join(out)


def render_doc(modules: dict, total: int) -> str:
    """渲染整个文档"""
    out = []
    out.append('# ChocoLight Lua API 参考文档')
    out.append('')
    out.append('> 自动从源代码 `/// @lua_api` 注释生成')
    out.append('')
    out.append(f"**API 总数**: {total}")
    out.append(f"**模块数**: {len(modules)}")
    out.append('')

    # 总目录
    out.append('## 模块目录')
    out.append('')
    for module in sorted(modules.keys()):
        anchor = module.lower().replace('.', '').replace(' ', '-')
        out.append(f"- [{module}](#{anchor}) ({len(modules[module])} 个 API)")
    out.append('')
    out.append('---')
    out.append('')

    # 各模块详情
    for module in sorted(modules.keys()):
        entries = sorted(modules[module], key=lambda e: e.func_name())
        out.append(render_module(module, entries))

    return '\n'.join(out)


# ============================================================
# 主入口
# ============================================================

def main():
    ap = argparse.ArgumentParser(description='Generate ChocoLight Lua API reference')
    ap.add_argument('--src',    default='ChocoLight/src',
                    help='源码目录 (默认 ChocoLight/src)')
    ap.add_argument('--output', default='docs/API_REFERENCE.md',
                    help='输出 Markdown 路径')
    args = ap.parse_args()

    # 找到工程根 (含 ChocoLight/ 的目录)
    root = Path(__file__).resolve().parent.parent
    src_dir = root / args.src
    out_path = root / args.output

    if not src_dir.is_dir():
        print(f"[ERROR] 源码目录不存在: {src_dir}", file=sys.stderr)
        sys.exit(1)

    # 扫描所有 .cpp 文件
    modules = defaultdict(list)
    total = 0
    for cpp in sorted(src_dir.glob('*.cpp')):
        entries = parse_file(cpp)
        if entries:
            print(f"  {cpp.name}: {len(entries)} APIs")
        for e in entries:
            modules[e.module()].append(e)
            total += 1

    if total == 0:
        print("[WARN] 未找到任何 @lua_api 注释")
        sys.exit(0)

    # 输出
    doc = render_doc(modules, total)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(doc, encoding='utf-8')

    print()
    print(f"[OK] {total} APIs 文档生成: {out_path}")
    print(f"     模块数: {len(modules)}")


if __name__ == '__main__':
    main()
