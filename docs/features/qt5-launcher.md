# Qt5 启动器与脚本执行

返回 [功能总索引](../feature-map.yaml)。

## 覆盖状态

截至 2026-09-28，Qt5 启动、脚本扫描、QML 模型桥、拖放校验和 QProcess 运行主链路已对照源码核对；未做本轮 GUI 运行验收。Windows Python 宿主经 QProcess 启动共用的 PowerShell 环境准备器；Linux 仍由启动器选择包内 venv 或系统 Python。Qt6.8 是占位前端；Tauri2 的调用链见 [Tauri 2 专题](tauri2-launcher.md)。

## 入口与关键实现

- [ui/qt5/src/main.cpp](../../ui/qt5/src/main.cpp)：解析开发或发行形态的脚本目录，装配模型与运行器并向 QML 注入上下文对象。
- [ScriptRegistry.cpp](../../ui/qt5/src/core/ScriptRegistry.cpp)：递归扫描脚本文件，并将共享 core/cpp 解析结果组织成稳定分组顺序。
- [ScriptListModel.cpp](../../ui/qt5/src/model/ScriptListModel.cpp)：向 QML 暴露脚本信息、校验拖入文件及脚本选择查询。
- [ScriptRunner.cpp](../../ui/qt5/src/model/ScriptRunner.cpp)：依据清单启动 Python、Blender 或声明的 exe，收集输出并处理退出和取消。
- [core/python/runtime/python-launcher.ps1](../../core/python/runtime/python-launcher.ps1)：Windows 下 Qt5 与 Tauri2 共用的 Python 环境发现、依赖准备和脚本启动器。
- [ProcessTreeController.cpp](../../ui/qt5/src/model/ProcessTreeController.cpp)：管理进程启动和取消；Windows 路径使用 Job Object 托管后代进程。
- [DropZone.qml](../../ui/qt5/src/qml/DropZone.qml)：处理拖放事件、显示校验状态，确认破坏性操作后调用运行器。
- [main.qml](../../ui/qt5/src/qml/main.qml)：组合脚本 Outliner、参数编辑器、输出区和窗口布局。
- [脚本契约专题](python-script-contract.md)：脚本元数据与命令行参数格式。

## 主要调用链

`main.cpp` → `ScriptListModel::load()` → `ScriptRegistry::scan()` → `ScriptManifest::fromFile()`；QML 读取模型生成界面。拖放由 `DropZone.qml` 调用 `validateDrop()`，通过后交给 `ScriptRunner::run()`。运行器根据 `@host` 组装参数并以 `QProcess` 启动相应宿主。

## 验证入口

- [ui/qt5/tests/ScriptRunnerTests.cpp](../../ui/qt5/tests/ScriptRunnerTests.cpp)：运行器测试源码，由 Qt5 CMake 注册为 `script_runner`。
- [core/python/runtime/tests/runtime-bootstrap.tests.ps1](../../core/python/runtime/tests/runtime-bootstrap.tests.ps1)：共享 Windows Python 环境准备测试入口。
- [ui/qt5/tools/qmltest/README.md](../../ui/qt5/tools/qmltest/README.md)：QML 解析、主题引用和行为检查工具说明。
- 拖放异常的首轮取证见 [symptom-map.yaml](../symptom-map.yaml)；诊断工具入口为 `GeneralOperationsLauncher.exe --dropdebug`，其日志位于 exe 同级，诊断后按仓库约定清理。
