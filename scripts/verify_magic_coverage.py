#!/usr/bin/env python3
"""
Phase G.1.7 P3.4 — Magic Constant Coverage Verification
========================================================

静态扫描工具, 验证 G.1.7 双层防御策略中的 magic 常量覆盖完整性.

用法:
    python scripts/verify_magic_coverage.py
    python scripts/verify_magic_coverage.py --strict   # 警告也视为错误

检查项:
    1. helpers.h 中声明的所有 LT_MAGIC_* 常量
    2. 每个常量在 ChocoLight/src/*.cpp 中至少被引用 N 次 (>=2: 设置 + 校验)
    3. 报告未使用 / 使用过少的常量

集成: CI Linux 任务 sanity check (Python 跨平台)
"""

import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple


# ==================== 配置 ====================

# 仓库根目录 (相对脚本位置)
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
HELPERS_HEADER = REPO_ROOT / "ChocoLight" / "src" / "light_lua_helpers.h"
SOURCE_DIR = REPO_ROOT / "ChocoLight" / "src"

# 每个 magic 常量的最小预期引用次数:
#   1 次 = helpers.h 自身定义
#   2 次 = 在 .cpp 中赋值 (struct.magic = LT_MAGIC_FOO)
#   3 次 = 在 .cpp 中校验 (if (magic != LT_MAGIC_FOO))
#   ≥ 4 次 = 多个文件协作 (如 cross-module 共享 struct)
MIN_REFS_PER_MAGIC = 2

# 白名单: 允许 ORPHAN 状态的 magic 常量 (helpers.h 预留, 后续阶段实现 / 不改造)
# 修改前请务必更新 docs/AUDIT_TypeSafety.md 反映原因.
ALLOWED_ORPHANS = {
    # G.1.7.1 预留 — 这些模块当前无 ctx struct 或后续阶段改造
    "LT_MAGIC_SPRITE":   "Light.Graphics.SpriteAnimation — 预留, 当前无 ctx struct",
    "LT_MAGIC_LIGHT2D":  "Light.Lighting2D — 预留, 当前用 handle id 模式",
    "LT_MAGIC_CAMERA":   "Light.Camera — 预留, 当前无 ctx struct",
    "LT_MAGIC_CURSOR":   "Light.Cursor — 预留, 当前用 handle id 模式",
    # ECS 设计决策 (见 AUDIT §3.2): 用 entityId 而非指针, 不需要 magic
    "LT_MAGIC_ECS_ENT":  "ECS — 用 uint64_t entityId 模式, 无 type confusion 风险",
    # SDL3 系统模块 — 多数为 handle id / 函数库, 不持 ctx struct
    "LT_MAGIC_PROCESS":  "Light.Process — SDL_Process handle, 函数库模式",
    "LT_MAGIC_TRAY":     "Light.Tray — SDL_Tray handle, 函数库模式",
    "LT_MAGIC_HIDDEV":   "Light.Hidapi — handle id, 函数库模式",
    "LT_MAGIC_SENSOR":   "Light.Sensor — SDL_Sensor handle, 函数库模式",
    "LT_MAGIC_HAPTIC":   "Light.Haptic — SDL_Haptic handle, 函数库模式",
    "LT_MAGIC_LOADSO":   "Light.LoadSO — SDL dlopen handle, 函数库模式",
}

# magic 常量正则: 匹配 helpers.h 中的 `constexpr uint32_t LT_MAGIC_XXX = ...;`
MAGIC_DEF_RE = re.compile(r"constexpr\s+uint32_t\s+(LT_MAGIC_\w+)\s*=")


# ==================== 工具函数 ====================

def parse_magic_constants(header_path: Path) -> List[str]:
    """从 helpers.h 解析所有 LT_MAGIC_* 常量名"""
    if not header_path.exists():
        print(f"[ERROR] helpers header not found: {header_path}", file=sys.stderr)
        sys.exit(2)

    text = header_path.read_text(encoding="utf-8", errors="replace")
    names = MAGIC_DEF_RE.findall(text)
    if not names:
        print(f"[ERROR] no LT_MAGIC_* constants found in {header_path}", file=sys.stderr)
        sys.exit(2)
    # 去重并保持声明顺序
    seen = set()
    ordered: List[str] = []
    for n in names:
        if n not in seen:
            seen.add(n)
            ordered.append(n)
    return ordered


def count_references(magic_names: List[str], source_dir: Path) -> Dict[str, List[Tuple[str, int]]]:
    """
    扫描 source_dir 下所有 .cpp / .h 文件, 统计每个 magic 名字的出现次数.
    返回: {magic_name: [(filename, count), ...]}
    """
    refs: Dict[str, List[Tuple[str, int]]] = {n: [] for n in magic_names}
    if not source_dir.exists():
        print(f"[ERROR] source dir not found: {source_dir}", file=sys.stderr)
        sys.exit(2)

    # 编译每个 magic 的精确匹配正则 (单词边界, 避免误匹配 LT_MAGIC_DEAD2 这类)
    patterns = {n: re.compile(r"\b" + re.escape(n) + r"\b") for n in magic_names}

    for path in source_dir.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() not in (".cpp", ".h", ".hpp", ".cc"):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        for name, pat in patterns.items():
            cnt = len(pat.findall(text))
            if cnt > 0:
                refs[name].append((rel, cnt))
    return refs


def report(magic_names: List[str], refs: Dict[str, List[Tuple[str, int]]], strict: bool) -> int:
    """
    输出报告. 返回 exit code (0 = 通过, 1 = 有警告或错误).
    """
    print("=" * 72)
    print("Phase G.1.7 P3.4 — Magic Constant Coverage Report")
    print("=" * 72)
    print(f"Total magic constants declared: {len(magic_names)}")
    print(f"Source dir: {SOURCE_DIR}")
    print()

    warnings: List[str] = []
    errors: List[str] = []
    allowed_orphans: List[str] = []

    for name in magic_names:
        usage = refs.get(name, [])
        # 统计文件数 + 总引用 (排除 helpers.h 自身的 1 次声明)
        total_refs = sum(c for _, c in usage)
        # helpers.h 中有 1 次定义, 我们关心其他 .cpp/.h 的引用数
        non_decl_refs = total_refs - 1  # 减去 helpers.h 中的 1 次定义
        files_count = len(usage)

        status = "OK"
        if total_refs == 0:
            status = "MISSING"
            errors.append(f"{name}: not found anywhere (declaration too?)")
        elif non_decl_refs < MIN_REFS_PER_MAGIC - 1:
            # 仅在 helpers.h 中定义, 没有任何 .cpp 引用
            if name in ALLOWED_ORPHANS:
                status = "PLANNED"
                allowed_orphans.append(f"{name}: {ALLOWED_ORPHANS[name]}")
            else:
                status = "ORPHAN"
                warnings.append(f"{name}: declared but not used in any .cpp ({total_refs} ref)")
        elif non_decl_refs == MIN_REFS_PER_MAGIC - 1:
            # 仅 1 次 .cpp 引用 — 可能漏写校验或赋值
            status = "LOW"
            warnings.append(f"{name}: only {non_decl_refs} non-decl ref (expected ≥ {MIN_REFS_PER_MAGIC})")
        # 输出每行
        print(f"  [{status:7}] {name:32} refs={total_refs} files={files_count}")

    print()
    print(f"Summary: {len(magic_names)} constants, "
          f"{len(allowed_orphans)} planned, "
          f"{len(warnings)} warnings, {len(errors)} errors")

    if allowed_orphans:
        print("\n--- Planned (whitelisted) ---")
        for p in allowed_orphans:
            print(f"  - {p}")

    if warnings:
        print("\n--- Warnings ---")
        for w in warnings:
            print(f"  ! {w}")

    if errors:
        print("\n--- Errors ---")
        for e in errors:
            print(f"  X {e}")

    if errors:
        print("\nResult: FAIL (errors present)")
        return 1
    if warnings and strict:
        print("\nResult: FAIL (warnings in --strict mode)")
        return 1
    print("\nResult: PASS")
    return 0


# ==================== main ====================

def main() -> int:
    strict = "--strict" in sys.argv

    print(f"[INFO] Scanning {HELPERS_HEADER}")
    magic_names = parse_magic_constants(HELPERS_HEADER)
    print(f"[INFO] Found {len(magic_names)} LT_MAGIC_* constants")
    print(f"[INFO] Counting refs in {SOURCE_DIR}/**/*.{{cpp,h,hpp}}")

    refs = count_references(magic_names, SOURCE_DIR)
    return report(magic_names, refs, strict)


if __name__ == "__main__":
    sys.exit(main())
