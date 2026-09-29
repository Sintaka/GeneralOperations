# Tauri 2 WebView 启动器与 Rust 运行器

返回 [功能总索引](../feature-map.yaml)。

## 覆盖状态

截至 2026-09-28，TypeScript UI 到 Tauri command、Rust 脚本清单和子进程运行主链已对照源码核对。Windows Python 宿主已接到与 Qt5 共用的运行时启动器；发行 zip 由共用 bundle 装配脚本放入启动器和依赖清单，不放 Python 安装目录或 Real-ESRGAN。当前源码有专门的 bootstrap 与 slim zip 验证入口；本文不声称它们在本轮已运行。

## 入口与关键实现

- [ui/tauri2/src/main.ts](../../ui/tauri2/src/main.ts)：WebView 单页工作台，绘制窗口标题栏并调用 Tauri Window API，载入脚本、显示参数与输入、处理文件选择/拖放，调用命令并接收运行事件。
- [ui/tauri2/src/types.ts](../../ui/tauri2/src/types.ts)：脚本、参数与运行事件的 TypeScript 数据形状。
- [ui/tauri2/src/styles.css](../../ui/tauri2/src/styles.css)：工作台布局、组件与自绘标题栏样式。
- [ui/tauri2/src-tauri/tauri.conf.json](../../ui/tauri2/src-tauri/tauri.conf.json)：主窗口关闭系统装饰并保留尺寸约束。
- [ui/tauri2/src-tauri/capabilities/default.json](../../ui/tauri2/src-tauri/capabilities/default.json)：仅向主窗口授予最小化、最大化/还原、关闭和拖动权限。
- [ui/tauri2/src-tauri/src/main.rs](../../ui/tauri2/src-tauri/src/main.rs)：初始化 Tauri、解析脚本目录、管理共享状态并注册 `list_scripts`、`run_script`、`stop_script` commands。
- [commands.rs](../../ui/tauri2/src-tauri/src/commands.rs)：把 WebView commands 转到脚本清单和运行器。
- [manifest.rs](../../ui/tauri2/src-tauri/src/manifest.rs)：调用随包的 `go_script_catalog`，反序列化共享 JSON 清单供 UI 使用；声明提取与校验由 core/cpp 负责。
- [core/cpp/include/go/manifest/catalog.hpp](../../core/cpp/include/go/manifest/catalog.hpp)：与 Qt5 共用的声明解析接口，允许模块 docstring 前的 shebang、编码注释、空行和整行注释。
- [model.rs](../../ui/tauri2/src-tauri/src/model.rs)：Rust 序列化脚本、参数、运行输出和完成状态模型。
- [runner.rs](../../ui/tauri2/src-tauri/src/runner.rs)：校验脚本/输入/破坏性确认，选择宿主并启动子进程；stdout/stderr 逐行发为 `run-output`，结束状态发为 `run-finished`。
- [arguments.rs](../../ui/tauri2/src-tauri/src/arguments.rs)：验证输入类型/扩展名并按契约生成参数数组。
- [paths.rs](../../ui/tauri2/src-tauri/src/paths.rs)：定位发行包或开发期脚本目录、共用 Python launcher、Blender 和 `@host exe` 后端。
- [ui/tauri2/scripts/build-env.mjs](../../ui/tauri2/scripts/build-env.mjs)：把 Cargo target/home 与 npm cache 固定到仓库根 `build/`，供 dev/build/test 入口共用。
- [core/python/runtime/python-launcher.ps1](../../core/python/runtime/python-launcher.ps1)：Qt5/Tauri2 共用的 Windows Python 环境准备与脚本启动入口。
- [状态与运行期路径](monorepo-layout.md)：共用 core、根 CMake 发行路径和打包脚本的边界。
- [Python 脚本契约专题](python-script-contract.md)：字段格式及脚本元数据的共同事实来源。

## 调用关系

启动时 `main.rs` 调 `paths::resolve_scripts_dir()` 并建立 `AppState`；Web UI 并行注册运行事件、拖放事件并用 `invoke("list_scripts")` 读取清单，注册失败会结束加载态并显示错误。Rust `manifest.rs` 优先读取 exe 同级的 `script-catalog-cache.json`；脚本目录、共享解析器内容/元数据或 `.py` 文件内容/元数据变化，以及缓存缺失/损坏时，调用同级 `tools/go_script_catalog/`（Debug 可回退构建目录）的 CLI 重建缓存。缓存记录绝对目录和工具路径，不在 Rust 再实现 docstring 字段规则。运行时 UI 把脚本 ID、输入、参数和破坏性确认状态送入 `run_script`；`runner.rs` 在真正执行前从共享 catalog 重新读取清单，不把可编辑的界面缓存当作执行凭据，然后调用 arguments 与 paths 模块，启动共用 PowerShell Python launcher、Blender 或声明的 exe。Python 启动请求以 JSON 经 stdin 传入，并包含共享 catalog 中全部原始 `@requires`；启动器只规范化该数组，探测系统 CPython 与 pip，必要时在用户 runtime 准备 venv 或官方 Python。运行器把输出和退出状态发回 Tauri 事件，UI 监听并更新活动记录；停止按钮通过 `stop_script` 按运行 ID 取消任务。

## 验证入口

- [ui/tauri2/src-tauri/tests/core_catalog.rs](../../ui/tauri2/src-tauri/tests/core_catalog.rs)：读取仓库 `core/python/scripts/` 并核对 PMX→GLB 声明的集成测试源码。
- [ui/tauri2/src-tauri/src/arguments.rs](../../ui/tauri2/src-tauri/src/arguments.rs)：包含参数与输入校验单元测试。
- [ui/tauri2/package.json](../../ui/tauri2/package.json)：`npm run check`、`npm test` 和 `npm run build` 入口。
- [tools/package_tauri2.ps1](../../tools/package_tauri2.ps1)：根版本派生的便携包构建与装配入口。
- [core/python/runtime/tests/runtime-bootstrap.tests.ps1](../../core/python/runtime/tests/runtime-bootstrap.tests.ps1)：覆盖需求扫描、系统 Python、venv 和哈希校验后官方 Python 回退的测试入口。
- [tools/verify_slim_zip.ps1](../../tools/verify_slim_zip.ps1)：核对 Tauri zip 中的脚本、共享 catalog CLI、Python 启动工具、后端和 CRC，并拒绝 Python 安装目录及 Real-ESRGAN 目录。
