# ui/qt6.8lts —— Qt 6.8 LTS 前端（占位）

当前状态：**仅保留位置，尚未实现。** `-DGO_FRONTEND=qt6.8lts` configure 时会
在 `CMakeLists.txt` 处直接 FATAL_ERROR（根 CMakeLists 的 qt6.8lts 分支会
add_subdirectory 进来，报错就在那时触发）。当前可用前端：qt5。

## 规划要点（接入时展开为正式 README）

- **沿用同一脚本契约与 bundle 管线。** 消费 `GO_CORE_SCRIPTS_DIR` 与
  `docs/SCRIPT_SPEC.md`，装配复用 `tools/bundle.cmake`（`GO_BUNDLE_DIR` /
  `GO_ZIP_FILE` 等根契约照旧，产物落 `output/x64-qt6.8lts/<配置>/`）——
  换前端只换 Qt 层，脚本与打包契约不动。
- **QML 从 ui/qt5 演进而非重写。** 视觉与交互已在第二代定稿（设计来源见
  `ui/qt5/README.md`），迁移是增量替换渲染层，不推倒重来。
- **preset 命名模板已预留：`qt68lts-mingw-debug` 这类**（约定
  `<前端><工具链>-<配置>`）。接入时按 docs/ARCHITECTURE.md「新增一个前端」
  的五件套补齐：根 CMakeLists 分支已就位，剩余是 preset 组、tasks.json
  任务组与文档索引。
- **目录名里的点号不影响 CMake 路径。** `qt6.8lts` 作为目录名/路径段完全
  合法（CMake 对路径段不做语义解析）；preset 名里刻意去掉点写成 `qt68lts`，
  是命令行高频输入与 build/<preset> 目录名的可读性取舍，不是限制。

Qt6 迁移的技术要点（import 不带版本号、qt_add_qml_module、Quick Controls2
样式机制变化、windeployqt6）已预记在 `CMakeLists.txt` 头注释里。
