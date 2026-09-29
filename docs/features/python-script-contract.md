# Python 脚本目录与 docstring 契约

返回 [功能总索引](../feature-map.yaml)。

## 覆盖状态

截至 2026-09-28，脚本目录导出、共享 core/cpp 声明解析、Qt5/Tauri2 清单适配，以及 Windows 共用 Python 环境启动入口已对照当前源码核对。脚本元数据来自每个 `.py` 文件的模块 docstring 声明块；前端消费同一解析结果，不执行 Python 来发现脚本。发行 zip 不携带 Python 安装目录；本专题不固定每次发行的体积或目标机环境准备耗时。

## 入口与关键实现

- [docs/SCRIPT_SPEC.md](../SCRIPT_SPEC.md)：约定元数据、参数、宿主和命令行映射；脚本作者与启动器共同消费。
- [core/python/CMakeLists.txt](../../core/python/CMakeLists.txt)：导出脚本目录变量，不生成构建目标。
- [core/python/runtime/python-launcher.ps1](../../core/python/runtime/python-launcher.ps1)：Qt5/Tauri2 共用的 Windows 环境发现、依赖准备与 Python 子进程启动入口。
- [core/python/runtime/runtime-check.py](../../core/python/runtime/runtime-check.py)：仅用标准库检查 CPython 基础条件和 `@requires` 导入/版本。
- [core/python/scripts/](../../core/python/scripts/)：当前脚本实体所在目录，按目录路径组织分组。
- [core/cpp/include/go/manifest/catalog.hpp](../../core/cpp/include/go/manifest/catalog.hpp)：两个前端共用的声明解析与 JSON 清单接口。
- [ScriptManifest.cpp](../../ui/qt5/src/core/ScriptManifest.cpp)：把共享 JSON 解析结果映射成 Qt 模型。
- [ScriptRegistry.cpp](../../ui/qt5/src/core/ScriptRegistry.cpp)：递归发现 `.py` 文件、解析清单并按组组织。
- [ui/tauri2/src-tauri/src/manifest.rs](../../ui/tauri2/src-tauri/src/manifest.rs)：调用共享 `go_script_catalog` CLI 并将 JSON 映射成 IPC 清单。
- [Qt5 启动器专题](qt5-launcher.md)：注册模型如何连接 QML 展示和运行器。

## 调用关系

Qt5 启动时由 `main.cpp` 解析脚本目录并调用 `ScriptListModel::load()`；模型委托 `ScriptRegistry::scan()` 读取各脚本，再由 `ScriptManifest::fromFile()` 调用共享 core/cpp 解析库。Windows Python 宿主把脚本路径、脚本根目录、参数与共享清单中全部原始 `@requires`（含 `?`）以 JSON 通过 stdin 交给共用 PowerShell 启动器；启动器仅规范化这些结构化依赖并准备环境，不再扫描源码。Tauri 2 运行时由 Rust `manifest::load_scripts()` 调用同库构建的 CLI 读取同一目录，供 WebView `list_scripts` command 使用；Windows Python 宿主通过 `paths.rs` 与 `runner.rs` 调用同一启动器，详情见 [Qt5 启动器专题](qt5-launcher.md) 与 [Tauri 2 专题](tauri2-launcher.md)。


## 验证入口

仓库约定检查见 [tools/check_repo.py](../../tools/check_repo.py)，Windows 环境准备测试入口见 [core/python/runtime/tests/runtime-bootstrap.tests.ps1](../../core/python/runtime/tests/runtime-bootstrap.tests.ps1)，Tauri slim zip 的文件与 CRC 检查入口见 [tools/verify_slim_zip.ps1](../../tools/verify_slim_zip.ps1)，EXR 脚本测试见 [tests/test_img_exr2png_large.py](../../tests/test_img_exr2png_large.py)。这些路径是验证入口，不代表本轮已运行或通过。
