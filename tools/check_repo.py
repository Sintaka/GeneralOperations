#!/usr/bin/env python3
"""仓库卫生静默检查：干净时零输出、退出码 0；有问题逐条打印并退出 1。

检查项：
  1. 文本源文件不得带 UTF-8 BOM（MSVC 工具链和部分工具会把 BOM 当内容）。
  2. 文本源文件必须能按 UTF-8 解码（防止 GBK 字面量混入导致乱码/编译错）。
  3. core/python/scripts/*.py 禁止出现 Qt 绑定（PySide/PyQt/qtpy）。
     脚本层是纯后端，沾上 Qt 意味着 Python 环境要跟着 Qt 的版本和许可走，
     破坏"脚本跑在任意无 GUI 部署形态"的前提。这条与打包期 cmake bundle 里的
     拦截是双保险：打包检查只在构建发行包时触发，平时开发随手改脚本就能被
     本检查先拦下。

用法：python tools/check_repo.py
（AGENTS.md 约定：这类检查一律写脚本跑，不靠模型逐字核对。）
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TEXT_EXTS = {".h", ".cpp", ".qml", ".py", ".md", ".cmake", ".sh",
             ".conf", ".txt", ".in", ".qrc", ".json"}
TEXT_NAMES = {"CMakeLists.txt", ".gitignore"}

# 只看源码树；构建产物、发行装配、诊断目录、未来 tauri2 前端的依赖/产物目录跳过。
# .vscode 刻意不在跳过之列：tasks.json 是文本，同样要查 BOM/编码。
SKIP_DIRS = {"build", "output", ".git", ".cache", ".sandbox",
             "__pycache__", "runtime", "venv",
             "node_modules", "target", "dist"}

# 与打包期（bundle 管线）同一个正则，两边语义保持一致，改一处必须改另一处。
QT_BINDING_RE = re.compile(r"(PySide[0-9]?|PyQt[56]?|qtpy)")

# 脚本实体目录（monorepo 后端契约，见 docs/ARCHITECTURE.md）。
SCRIPTS_DIR = ROOT / "core" / "python" / "scripts"


def is_text(p: Path) -> bool:
    if p.name in TEXT_NAMES:
        return True
    return p.suffix.lower() in TEXT_EXTS


def check_qt_bindings(problems: list[str]) -> None:
    """检查项 3：脚本层禁 Qt。

    目录尚不存在时静默跳过 —— monorepo 搭建期该目录由并行任务落盘，
    缺目录不是本检查的职责（打包管线与脚本清单校验会兜底）。
    """
    if not SCRIPTS_DIR.is_dir():
        return
    for p in sorted(SCRIPTS_DIR.glob("*.py")):
        text = p.read_text(encoding="utf-8")
        hit = QT_BINDING_RE.search(text)
        if hit:
            problems.append(
                f"{p.relative_to(ROOT)}: 引用了 Qt 绑定（{hit.group(1)}）。"
                "脚本层是纯后端，禁止依赖 Qt —— 否则 Python 环境就要跟着 "
                "Qt 的版本和许可走了（docs/SCRIPT_SPEC.md 硬规则 0）。")


def main() -> int:
    problems = []
    for p in sorted(ROOT.rglob("*")):
        if set(p.relative_to(ROOT).parts) & SKIP_DIRS:
            continue
        if not p.is_file() or not is_text(p):
            continue
        data = p.read_bytes()
        if data.startswith(b"\xef\xbb\xbf"):
            problems.append(f"{p.relative_to(ROOT)}: 带 UTF-8 BOM")
            continue
        try:
            data.decode("utf-8")
        except UnicodeDecodeError as e:
            problems.append(f"{p.relative_to(ROOT)}: 不是合法 UTF-8（{e}）")

    check_qt_bindings(problems)

    for line in problems:
        print(line)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
