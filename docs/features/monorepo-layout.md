# Monorepo 分层与前端选择

返回 [功能总索引](../feature-map.yaml)。

## 覆盖状态

截至 2026-09-28，根 CMake 的目录边界、两个 core 挂载、Qt5 与 Tauri2 接入和共用装配入口已对照源码核对。Windows 包装配当前使用共用 Python launcher/runtime-check，且不收 Python 安装目录或 Real-ESRGAN。Qt6.8 仍为配置时明确报错的占位前端。

## 入口与关键实现

- [根 CMakeLists.txt](../../CMakeLists.txt)：声明唯一工程版本与 `GO_FRONTEND`，无条件加入两个 core，再按前端选择 Qt5、Qt6.8 占位目录或 Tauri2 工具链说明分支。
- [core/python/CMakeLists.txt](../../core/python/CMakeLists.txt)：向父目录导出 Python 脚本目录和共享 Windows runtime 工具目录；脚本本身不由此目录构建。
- [core/python/runtime/python-launcher.ps1](../../core/python/runtime/python-launcher.ps1)：Qt5/Tauri2 共用的 Windows Python 环境准备入口。
- [core/cpp/CMakeLists.txt](../../core/cpp/CMakeLists.txt)：定义纯函数库、PMX→GLB 库和命令行程序。
- [ui/qt5/CMakeLists.txt](../../ui/qt5/CMakeLists.txt)：Qt5/QML 前端构建入口。
- [ui/qt6.8lts/CMakeLists.txt](../../ui/qt6.8lts/CMakeLists.txt)：占位拒绝配置；不是可构建前端。
- [tools/bundle.cmake](../../tools/bundle.cmake)：Qt5 POST_BUILD 与 Tauri 发行入口共同使用的装配脚本；按前端类型控制 Qt 文件部署。
- [tools/package_tauri2.ps1](../../tools/package_tauri2.ps1)：读取根 CMake 生成的发行路径，构建 Tauri 和 core 并调用共用装配脚本。
- [.github/workflows/release-tauri2.yml](../../.github/workflows/release-tauri2.yml)：`main` 更新后在 Windows x64 构建并将单个 Tauri 2 slim zip 发布为根版本对应的 GitHub Release。
- [ui/tauri2/README.md](../../ui/tauri2/README.md)：Tauri 前端构建和发行入口说明；源码定位见 [Tauri 2 专题](tauri2-launcher.md)。
- [架构契约与前后端接入步骤](../ARCHITECTURE.md)：分层约束、扩展步骤和根级变量说明。

## 边界与导航

前端从根级契约消费脚本目录和声明；Python 与 C++ 内核不依赖 UI 或 Qt。脚本解析和运行见 [Python 脚本契约专题](python-script-contract.md)、[Qt5 启动器专题](qt5-launcher.md) 与 [Tauri 2 专题](tauri2-launcher.md)；C++ 能力见 [C++ 后端专题](cpp-backends.md)。Qt6.8 是未实现占位。

## 构建与发行路径

根 CMake 单点决定 `GO_BUNDLE_DIR` 与 `GO_ZIP_FILE`；单配置生成器还会把这些值和 `GO_PYTHON_RUNTIME_DIR` 写入构建目录的 `go_release_paths.json`，供独立工具链读取。Qt5 的 POST_BUILD 调用 `tools/bundle.cmake` 并传入 `BUNDLE_RUNTIME`；Tauri 发行脚本读取该 JSON，再调用同一装配脚本。Windows zip 放共享启动脚本和 `requirements.txt`，目标机按需准备 Python 环境。装配细节和发行目录见 [docs/RELEASE.md](../RELEASE.md)。
