# Tauri 2 + WebView2 前端

Tauri 前端消费 `core/python/scripts/` 中的 Python 模块 docstring 声明，不维护脚本副本或手工清单。Rust 调用 `core/cpp` 的共享 catalog CLI 取得 JSON 清单，再通过 IPC 提供脚本树、参数和宿主信息；Rust 使用参数数组启动 Python、Blender 或 `@host exe` 后端。

## 构建

需要 Node.js/npm、Rust stable MSVC 工具链、WebView2 Evergreen Runtime、CMake、Ninja 和 C++ 编译器。初次安装和本地运行：

```powershell
npm ci
npm run dev
```

发行构建入口固定为 `general-operations-tauri.exe`：

```powershell
npm run build
```

`npm run build` 先从根 `CMakeLists.txt` 读取产品版本、生成被忽略的 `src-tauri/Cargo.toml` 和 Windows 图标，再用根 CMake 构建 `go_pmx2glb` 与 `go_script_catalog`，最后执行 `tauri build --no-bundle`。它不会生成安装器。Cargo target、Cargo home、npm cache 和 Vite dist 分别位于仓库根 `build/tauri2-cargo-target/`、`build/tauri2-cargo-home/`、`build/tauri2-npm-cache/`、`build/tauri2-ui-dist/`；`node_modules/` 仍留在此前端目录供本地依赖解析。主仓库发行编排从 `build/tauri2-cargo-target/release/general-operations-tauri.exe` 取可执行文件，并按统一便携布局装配：

```text
general-operations-tauri.exe
scripts/                         # core/python/scripts 的原样副本
python-launcher.ps1               # 共用 Windows Python wrapper
runtime-check.py                  # wrapper 使用的运行时检查模块
tools/go_pmx2glb/go_pmx2glb.exe  # 唯一的 @host exe 后端
tools/go_script_catalog/go_script_catalog.exe  # 与 Qt5 共用解析库的 catalog CLI
```

Cargo 要求 SemVer 格式；同步脚本将 CMake 的三位 patch（例如 `0.2.009`）规范化为 `0.2.9` 写入生成清单。源码只跟踪 `src-tauri/Cargo.toml.in`，产品版本仍由根 CMake 单点管理。版本同步可单独运行：

```powershell
npm run sync-version
```

检查与测试：`npm run check`、`npm test`。`npm test` 会先构建共享 catalog CLI，再确认 EXR2PNG、PMX 与其它脚本声明从仓库目录正确读取。

## 运行契约

- 脚本树、名称、描述、参数、文件类型和宿主均由共享 catalog CLI 从模块 docstring 解析；Rust 只反序列化 JSON，不执行 Python 来发现元数据，也不复制字段校验规则。
- 参数按 `SCRIPT_SPEC.md` 映射为 `--kebab-case value`；true 布尔参数传 `--flag`，false 不传；文件路径位于 `--` 后。
- `@host python` 通过发行包 exe 同级的 `python-launcher.ps1` 启动；Debug 构建在缺少同级 wrapper 时回退到 `core/python/runtime/python-launcher.ps1`。wrapper 接管 Python 选择、依赖安装、下载 Python 3.13、venv 和脚本执行。Rust 将脚本绝对路径、契约参数数组和 `scriptsDir` 以 UTF-8 JSON 从 stdin 传入。`@host blender` 使用声明的 Blender 完整路径；`@host exe` 仅解析 `@exe` 命名的随包后端。
- Rust 将 catalog 中全部脚本的原始 `@requires` 作为请求字段交给 Python wrapper；wrapper 负责规范化、依赖检查和环境准备，不读取源码扫描声明。Python 子进程移除继承的 `PYTHONPATH`。
- 脚本路径先取当前可执行文件同级 `scripts/`。仅 Debug 构建在该目录不存在时回退到仓库 `core/python/scripts/`；Release 不搜索开发树。
- Release 缺少同级 `scripts/` 时窗口仍会打开，脚本目录错误会显示在界面日志中，不会伪装成空清单。
- 开发期 C++ 后端由根 CMake 构建到 `build/tauri2-core-debug/core/cpp/`；可用 `GO_DEV_BACKEND_DIR` 覆盖。发行期只从 exe 同级 `tools/go_pmx2glb/` 解析。
- stdout/stderr 通过 Tauri 事件逐行回传；取消会终止 Windows 进程树，并在运行记录和状态中显示“已取消”。

## 架构说明

Qt5 静态链接 `go_script_manifest_core`，Tauri2 调用同库构建的 `go_script_catalog`。两端运行时读取当前脚本目录，共用 docstring 提取、字段校验和 JSON 序列化规则；Tauri2 发行包在压缩前用随包 CLI 检查全部声明。加入或修改脚本后无需维护第二份清单。
