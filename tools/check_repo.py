#!/usr/bin/env python3
"""仓库卫生静默检查：干净时零输出、退出码 0；有问题逐条打印并退出 1。

检查项：
  1. 文本源文件不得带 UTF-8 BOM（MSVC 工具链和部分工具会把 BOM 当内容）。
  2. 文本源文件必须能按 UTF-8 解码（防止 GBK 字面量混入导致乱码/编译错）。
  3. core/python/scripts/ 树下全部 .py 禁止出现 Qt 绑定（PySide/PyQt/qtpy）。
     脚本层是纯后端，沾上 Qt 意味着 Python 环境要跟着 Qt 的版本和许可走，
     破坏"脚本跑在任意无 GUI 部署形态"的前提。这条与打包期 cmake bundle 里的
     拦截是双保险：打包检查只在构建发行包时触发，平时开发随手改脚本就能被
     本检查先拦下。
  4. core/python/scripts/ 下每个脚本的 @group 必须与其相对 scripts 的目录
     路径一致（根下脚本 @group 不含 /）。启动器只透传 group 字符串做
     ListView 分组、不校验名字，分组与目录的一致性全靠这条检查兜底。

用法：python tools/check_repo.py
（AGENTS.md 约定：这类检查一律写脚本跑，不靠模型逐字核对。）
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TEXT_EXTS = {".h", ".cpp", ".qml", ".py", ".md", ".cmake", ".sh",
             ".conf", ".txt", ".in", ".qrc", ".json", ".yaml", ".yml",
             ".rs", ".toml", ".js", ".mjs", ".ts", ".tsx", ".css",
             ".html", ".svg", ".ps1"}
TEXT_NAMES = {"CMakeLists.txt", ".gitignore", "Cargo.lock"}

# 只看源码树；构建产物、发行装配、诊断目录、tauri2 依赖/产物目录跳过。
# .vscode 刻意不在跳过之列：tasks.json 是文本，同样要查 BOM/编码。
SKIP_DIRS = {"build", "output", ".git", ".cache", ".sandbox",
             "__pycache__", "venv",
             "node_modules", "target", "dist", ".cargo-home", ".npm-cache"}

# 与打包期（bundle 管线）同一个正则，两边语义保持一致，改一处必须改另一处。
QT_BINDING_RE = re.compile(r"(PySide[0-9]?|PyQt[56]?|qtpy)")

# docstring 声明块里的分组行（逐行找，取第一个命中；MULTILINE 让 ^$ 逐行生效）。
GROUP_RE = re.compile(r"^@group\s+(.+?)\s*$", re.MULTILINE)

# 脚本实体目录（monorepo 后端契约，见 docs/ARCHITECTURE.md）。
SCRIPTS_DIR = ROOT / "core" / "python" / "scripts"


def check_tasks(problems: list[str]) -> None:
    """VS Code tasks.json 是 JSONC；去掉整行注释后检查结构与 label 唯一性。"""
    path = ROOT / ".vscode" / "tasks.json"
    if not path.is_file():
        problems.append(".vscode/tasks.json: 文件不存在")
        return
    source = path.read_text(encoding="utf-8")
    source = re.sub(r"(?m)^\s*//[^\n]*$", "", source)
    try:
        data = json.loads(source)
    except ValueError as exc:
        problems.append(f".vscode/tasks.json: JSONC 解析失败（{exc}）")
        return
    if not isinstance(data, dict) or not isinstance(data.get("tasks"), list):
        problems.append(".vscode/tasks.json: 根节点或 tasks 列表格式错误")
        return
    if any(not isinstance(task, dict) or not isinstance(task.get("label"), str)
           for task in data["tasks"]):
        problems.append(".vscode/tasks.json: task 缺少字符串 label")
        return
    labels = [task["label"] for task in data["tasks"]]
    if len(labels) != len(set(labels)):
        problems.append(".vscode/tasks.json: 存在重复 task label")


def check_maps(problems: list[str]) -> None:
    """无第三方依赖地检查 JSON 语法的 YAML 1.2 导航及其链接。"""
    maps = {}
    for name in ("feature-map.yaml", "symptom-map.yaml"):
        path = ROOT / "docs" / name
        if not path.is_file():
            problems.append(f"docs/{name}: 导航文件不存在")
            continue
        try:
            maps[name] = json.loads(path.read_text(encoding="utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            problems.append(f"docs/{name}: JSON/YAML 解析失败（{exc}）")
    if len(maps) != 2:
        return

    feature_map = maps["feature-map.yaml"]
    symptom_map = maps["symptom-map.yaml"]
    if not isinstance(feature_map, dict) or not isinstance(feature_map.get("features"), list):
        problems.append("docs/feature-map.yaml: 根节点或 features 列表格式错误")
        return
    if not isinstance(symptom_map, dict) or not isinstance(symptom_map.get("symptoms"), list):
        problems.append("docs/symptom-map.yaml: 根节点或 symptoms 列表格式错误")
        return
    if feature_map.get("path_base") != "repository-root":
        problems.append("docs/feature-map.yaml: path_base 必须为 repository-root")
    if symptom_map.get("path_base") != "repository-root":
        problems.append("docs/symptom-map.yaml: path_base 必须为 repository-root")
    if symptom_map.get("feature_map") != "docs/feature-map.yaml":
        problems.append("docs/symptom-map.yaml: feature_map 指向不正确")
    unmapped = feature_map.get("not_yet_mapped", [])
    if isinstance(unmapped, list) and all(isinstance(value, str) for value in unmapped):
        if len(unmapped) != len(set(unmapped)):
            problems.append("docs/feature-map.yaml: not_yet_mapped 存在重复项")

    feature_ids = set()
    indexed_docs = set()
    for item in feature_map["features"]:
        if not isinstance(item, dict):
            problems.append("docs/feature-map.yaml: features 条目不是对象")
            continue
        feature_id = item.get("id")
        if not isinstance(feature_id, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", feature_id):
            problems.append(f"docs/feature-map.yaml: 非法功能 ID {feature_id!r}")
        elif feature_id in feature_ids:
            problems.append(f"docs/feature-map.yaml: 重复功能 ID {feature_id}")
        feature_ids.add(feature_id)
        doc = item.get("doc")
        if not isinstance(doc, str) or not doc.startswith("docs/features/"):
            problems.append(f"docs/feature-map.yaml: {feature_id} 的 doc 不在 docs/features/")
            continue
        indexed_docs.add(doc)
        entrypoints = item.get("entrypoints")
        if not isinstance(entrypoints, list):
            problems.append(f"docs/feature-map.yaml: {feature_id} 的 entrypoints 不是列表")
            continue
        for rel in [doc, *entrypoints]:
            if not isinstance(rel, str) or not (ROOT / rel).is_file():
                problems.append(f"docs/feature-map.yaml: {feature_id} 引用不存在的文件 {rel!r}")

    for path in sorted((ROOT / "docs" / "features").glob("*.md")):
        rel = path.relative_to(ROOT).as_posix()
        if rel not in indexed_docs:
            problems.append(f"{rel}: 未从 feature-map 索引到达")
        for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", path.read_text(encoding="utf-8")):
            target = target.split("#", 1)[0].strip("<>")
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            resolved = (path.parent / target).resolve()
            if not resolved.is_relative_to(ROOT) or not resolved.exists():
                problems.append(f"{rel}: Markdown 链接失效 {target!r}")

    symptom_ids = set()
    for item in symptom_map["symptoms"]:
        if not isinstance(item, dict):
            problems.append("docs/symptom-map.yaml: symptoms 条目不是对象")
            continue
        symptom_id = item.get("id")
        if not isinstance(symptom_id, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", symptom_id):
            problems.append(f"docs/symptom-map.yaml: 非法症状 ID {symptom_id!r}")
        elif symptom_id in symptom_ids:
            problems.append(f"docs/symptom-map.yaml: 重复症状 ID {symptom_id}")
        symptom_ids.add(symptom_id)
        refs = item.get("feature_ids")
        if not isinstance(refs, list):
            problems.append(f"docs/symptom-map.yaml: {symptom_id} 的 feature_ids 不是列表")
            continue
        for feature_id in refs:
            if feature_id not in feature_ids:
                problems.append(f"docs/symptom-map.yaml: {symptom_id} 引用未知功能 {feature_id!r}")
        if not item.get("first_checks"):
            problems.append(f"docs/symptom-map.yaml: {symptom_id} 缺少首轮取证分流")


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
    for p in sorted(SCRIPTS_DIR.rglob("*.py")):
        text = p.read_text(encoding="utf-8")
        hit = QT_BINDING_RE.search(text)
        if hit:
            problems.append(
                f"{p.relative_to(ROOT)}: 引用了 Qt 绑定（{hit.group(1)}）。"
                "脚本层是纯后端，禁止依赖 Qt —— 否则 Python 环境就要跟着 "
                "Qt 的版本和许可走了（docs/SCRIPT_SPEC.md 硬规则 0）。")


def check_group_dir_consistency(problems: list[str]) -> None:
    """检查项 4：@group 与 scripts 目录结构一致。

    分组名可用 / 表层级（如 Image/Format Convert），约定与脚本相对
    core/python/scripts/ 的目录路径一致；在 scripts 根下的脚本 group 不含 /。
    启动器把 group 当不透明字符串透传给 ListView 分组显示，不做名字校验，
    这条一致性约定靠本检查兜底。
    """
    if not SCRIPTS_DIR.is_dir():
        return
    for p in sorted(SCRIPTS_DIR.rglob("*.py")):
        rel = p.relative_to(SCRIPTS_DIR).as_posix()
        hit = GROUP_RE.search(p.read_text(encoding="utf-8"))
        actual = hit.group(1) if hit else None
        if "/" in rel:
            expected = rel.rsplit("/", 1)[0]
            ok = actual == expected
        else:
            expected = None
            ok = actual is not None and "/" not in actual
        if ok:
            continue
        want = expected if expected is not None \
            else "不含 / 的分组名（脚本在 scripts 根）"
        shown = actual if actual is not None else "(未找到 @group 行)"
        problems.append(
            f"{p.relative_to(ROOT)}: @group 与目录不一致"
            f"（实际 \"{shown}\"，期望 \"{want}\"）。"
            "约定：新脚本放进哪个目录，@group 就写到那个相对路径"
            "（docs/SCRIPT_SPEC.md @group 键）。")


def check_tauri_build_layout(problems: list[str]) -> None:
    """旧的 Tauri 构建/缓存目录不能重新长进 ui/ 源码树。"""
    for rel in (
        "ui/tauri2/src-tauri/target",
        "ui/tauri2/.cargo-home",
        "ui/tauri2/.npm-cache",
        "ui/tauri2/dist",
    ):
        if (ROOT / rel).exists():
            problems.append(f"{rel}: Tauri 生成物应位于仓库根 build/tauri2-*，请检查构建环境路径")


def main() -> int:
    problems = []
    for p in sorted(ROOT.rglob("*")):
        parts = p.relative_to(ROOT).parts
        if (set(parts) & SKIP_DIRS or parts[0] == "runtime"
                or parts[:4] == ("ui", "tauri2", "src-tauri", "gen")):
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
    check_group_dir_consistency(problems)
    check_tauri_build_layout(problems)
    check_tasks(problems)
    check_maps(problems)

    for line in problems:
        print(line)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
