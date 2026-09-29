# 打包与发行（output/ 发行装配）

> Qt5 构建后，POST_BUILD 的 bundle 管线把产物装配成
> `output/x64-<前端>/<Debug|Release>/`（qt5 即
> `output/x64-qt5/Release/`）——发行文件整体拷走或压 zip 分发，不依赖开发机的
> 绝对路径（构建期的 build/ 目录才依赖本机 Qt）。Windows 首次运行 Python 脚本时
> 会准备目标机 Python 环境并联网安装所需依赖，见下文。
> 管线脚本位于 `tools/bundle.cmake`；Qt5 的 POST_BUILD 与 Tauri 发行入口
> `tools/package_tauri2.ps1` 共用，入参契约见 `docs/ARCHITECTURE.md`「关键契约」。
>
> 本文按当前 monorepo 的 Qt5/Tauri2 共用装配脚本更新；Windows Python 环境和
> Real-ESRGAN 改为目标机按需准备，不沿用旧仓库的内嵌运行时描述。

## 目录布局（Windows，qt5）

```
output/x64-qt5/Release/
├── GeneralOperationsLauncher.exe   # 图标来自 ui/qt5/src/assets/app.ico（ui/qt5/tools/gen_icon.py 生成）
├── qt.conf                  # Prefix=. —— 相对路径，指向自身
├── Qt5*.dll / lib*.dll      # windeployqt 装配的 Qt + MinGW 运行时
├── platforms/ 及各 QML 模块目录  # 平台插件 + QML 模块（VirtualKeyboard 已剪除）
├── scripts/                 # Python 脚本（来自 GO_CORE_SCRIPTS_DIR = core/python/scripts）
├── tools/go_pmx2glb/        # 后端 exe（core/cpp 构建产物收集，见下文专节）
├── python-launcher.ps1      # 共用 Python 环境启动器
├── runtime-check.py         # CPython / pip / 依赖探针
└── requirements.txt         # 由各脚本 @requires 行自动汇总生成，目标机安装
```

Windows Qt5 与 Tauri 2 的便携 zip 都不内嵌 Python，也不预置 Real-ESRGAN；
Python 启动脚本和探针只负责在目标机准备运行环境。Qt5 包另带 Qt 运行时文件；
Tauri 包不带 Qt 文件。`tools/go_pmx2glb/` 是本仓编译出的原生后端，仍随包分发。

Linux 的 Qt5 布局仍由 `run.sh` 在首次运行时创建 `runtime/venv` 并逐包安装
`requirements.txt`；Qt 走系统包（见下）。

Tauri 2 的 Windows 便携包位于 `output/x64-tauri2/Release/`，入口是
`general-operations-tauri.exe`；`scripts/`、`requirements.txt`、
`python-launcher.ps1`、`runtime-check.py`、`tools/go_pmx2glb/` 与
`tools/go_script_catalog/` 随包分发。Tauri 调用后者读取与 Qt5 静态链接库相同的
脚本声明解析结果。该包不包含 Python 安装目录或 Real-ESRGAN 目录，Qt DLL、
QML 模块及 `qt.conf` 也不进入此包。
发行脚本在压缩前用随包 `go_script_catalog --check` 校验所有脚本声明；无效声明会
直接中断发包。zip 校验器还会核对 CLI、脚本与后端文件及归档 CRC。
便携 zip 不运行 WebView2 安装器，目标机须已有 WebView2 Runtime；Windows 10
（1803 起）及 Windows 11 通常随系统提供，缺失时按
[Tauri 的 Windows 前置要求](https://v2.tauri.app/start/prerequisites/)安装。

GitHub Release 由 [Tauri 2 发行工作流](../.github/workflows/release-tauri2.yml)
在 `main` 更新后于 Windows x64 构建，并只上传根版本对应的
`GeneralOperations-<版本>_x64-tauri2-release.zip`。Release 标签为 `v<版本>`；
每次发版须先递增根 `CMakeLists.txt` 的 patch 版本。工作流与本地构建共用
`tools/package_tauri2.ps1` 和 slim zip 校验，不上传 Qt5、Python 安装目录或
Real-ESRGAN 下载缓存；GitHub 内置 `GITHUB_TOKEN` 完成发布，无需个人访问令牌。

## bundle 管线入参（monorepo 的变化点）

前端调用共用 bundle 脚本时，各资源通过显式参数传入——monorepo 里脚本、QML、
Windows Python runtime 工具和后端产物分处不同目录，不从前端源码树推断路径：

- **`BUNDLE_SCRIPTS`** = `GO_CORE_SCRIPTS_DIR`（即 `core/python/scripts`）：
  脚本拷贝源，进入装配目录 `scripts/`。
- **`BUNDLE_CATALOG`** = `go_script_catalog` 的构建产物：打包期从共享 JSON 清单
  汇总 `@requires` 生成 `requirements.txt`，不由 CMake 扫描 Python 源码。
- **`BUNDLE_QMLDIR`** = 前端 QML 源目录：windeployqt 的 `--qmldir`。
- **`BUNDLE_RUNTIME`** = `GO_PYTHON_RUNTIME_DIR`（即 `core/python/runtime`）：
  Windows 下把 `python-launcher.ps1` 与 `runtime-check.py` 拷到发行 exe 同级；
  不会把 Python 安装器或 Python runtime 放进 zip。
- **`BUNDLE_BACKENDS`** = 后端可执行文件的构建产物（`$<TARGET_FILE:go_pmx2glb>`，
  可为空）：`@host exe` 脚本（如 `geo.pmx2glb.py`）的执行体，收进装配目录
  `tools/<exe 名去扩展名>/`（见下文专节）。
- **`BUNDLE_TOOLS`** = 共用 core 工具的构建产物（Tauri2 为
  `go_script_catalog`）：同样收进 `tools/<名>/`，用于读取脚本清单，不作为
  `@host exe` 执行体。
- **`BUNDLE_UI_KIND`** = `qt5` 或 `tauri2`：只决定是否运行 windeployqt；
  core 资产在两个包里由同一段装配逻辑处理。

其余参数（`BUNDLE_EXE` / `BUNDLE_OUT` / `BUNDLE_CONF` / `BUNDLE_WIN` /
`BUNDLE_MINGW` / `BUNDLE_WDT` / `BUNDLE_QT_BIN`）语义不变。

## Windows Python 环境准备

`python-launcher.ps1` 与 `runtime-check.py` 是 Qt5/Tauri 2 共用的运行期启动链，
随发行包放在 exe 同级。首次执行 Python 宿主脚本时，前端把共享 catalog 中所有
脚本的 `@requires` 原样汇成请求数组；启动器只规范化该数组，不扫描 Python 源码。
随后按以下顺序准备环境；运行状态位于
`%LOCALAPPDATA%\GeneralOperations\runtime`，也可用 `GO_RUNTIME_HOME` 改写：

1. 发现 PATH、`py -0p`、Python 注册表与 `GO_PYTHON` 指定的候选解释器，逐个验证
   必须是 64 位 CPython，且 `pip` 可运行。
2. 用 `runtime-check.py` 导入并核对所有声明依赖（有 `==` 版本约束时一并核对）。
   已满足则复用；缺失时先让 pip 以 `--dry-run --only-binary=:all:` 检查全部
   依赖都有兼容 wheel，再安装并复核。系统 Python 的直接安装使用 `--user`。
3. 系统解释器无法安装或验证依赖时，尝试以该解释器创建独立 venv 并在其中安装。
4. 如果没有合适的系统解释器或其 venv 也不能满足依赖，则从官方 Python 3.13.15
   下载 x64 安装器，校验固定 SHA-256，再安装到用户 runtime；在其 venv 中通过在线
   pip 检查 wheel、安装并复核全部 `@requires`。

目标机首次准备环境需要联网；系统候选可以立即复用，官方安装器和 Python 包只在
不适合的候选无法满足依赖时下载。发行 zip 本身不含 Python 解释器或 site-packages。
启动 Python 子进程时设置 UTF-8 环境并移除 `PYTHONHOME`、`PYTHONPATH` 与
`PYTHONNOUSERSITE`，避免调用进程的 Python 环境变量改变探测结果。

Tauri 2 打开窗口读取脚本清单时不启动上述 Python 检查。首次运行由共享
`go_script_catalog` 解析脚本后，Tauri 在 exe 同级生成 `script-catalog-cache.json`；
后续按脚本与解析器内容指纹及文件元数据判断是否复用。该文件是目标机运行时缓存，发行 zip
不预置；缓存缺失或失效时自动重新解析。

## Real-ESRGAN 懒加载（Windows）

发行 zip 不预置 `tools/realesrgan/`。`img.RealESRGAN_Upscale.py` 首次运行时先找
旧发行包中的工具，再找 `%LOCALAPPDATA%\GeneralOperations\runtime\tools\realesrgan/`
（或 `GO_RUNTIME_HOME` 指定位置）；都没有时才从上游固定 URL 下载官方
v0.2.5.0 Windows zip，并先校验固定 SHA-256 再安全解压到用户缓存。
缓存后的工具供后续调用复用；下载、校验或 Vulkan 执行不可用时，脚本用已声明依赖
Pillow 的 LANCZOS 缩放和适度锐化输出非 AI 保底结果。该路径需要联网获取首份工具包，
失败时仍可使用 Pillow 路径。

## 后端可执行文件（@host exe，构建产物收集）

- **来源**：本仓 core/cpp 构建的后端 exe（当前为 `go_pmx2glb`，PMX→GLB
  转换，对应脚本 `Geometry/Format Convert/geo.pmx2glb.py` 的 `@host exe` /
  `@exe go_pmx2glb` 声明）。Real-ESRGAN 不在 `tools/` 中随包预置，由其脚本首用时
  懒下载到用户 runtime 缓存；本节说明的后端则是随构建一起编译的本仓产物 ——
  POST_BUILD 经 `BUNDLE_BACKENDS` 把 `$<TARGET_FILE:go_pmx2glb>` 收进
  `tools/go_pmx2glb/`（目录名 = exe 去扩展名）。
- **启动器定位**：发行形态到 exe 同级 `tools/<@exe 名>/` 找执行体；开发期
  build/ 的裸 exe 走编译期注入的 `GO_DEV_BACKEND_DIR`（后端产物目录），
  不需要先跑一次装配。
- **失败如何暴露**：前端 CMakeLists 已 `add_dependencies` 保证先建后端再
  装配，正常流程收到的必然是新鲜产物；target 被改名/删除会在 configure 阶段
  被 genex 直接拦下，产物意外缺失时 bundle 收集段 `FATAL_ERROR` 中断构建 ——
  不会静默产出一个缺后端的发行包。

## 使用

（全部从仓库根执行）

```
cmake --build --preset qt5-mingw-release        # 构建并装配 output/x64-qt5/Release
cmake --build --preset qt5-mingw-release --target release_zip   # 压成发行 zip
cmake --build --preset qt5-mingw-debug          # output/x64-qt5/Debug 同理
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package_tauri2.ps1
                                                 # output/x64-tauri2/Release + zip
```

发行 zip：`output/GeneralOperations-<版本>_x64-qt5-<release|debug>.zip`
（版本取根 `project(... VERSION)`，命名规则 `GO_ZIP_FILE` 属于根契约，见
`docs/ARCHITECTURE.md`「关键契约」）。Windows 目标机解压后运行；首次运行 Python
脚本时需要完成上文的环境准备；
Linux 解压后 `./run.sh`。

`qt5-mingw-deploy` build preset 是 release 构建外加 `deploy` 目标
（windeployqt 收集 Qt 运行时与 QML 模块）的一条龙入口。

## 依赖声明

`requirements.txt` 由共享 catalog 的 `@requires` 输出自动汇总，作为包内可读清单；
Windows 前端在运行时从已解析清单传入全部依赖，启动器统一预检/安装，要求 pip 可取得 wheel（不从发行 zip
携带 site-packages）。Linux `run.sh` 则在首次运行时逐包安装，每个包失败只影响
它对应的脚本。`?` 会从清单中的包名移除；当前 Windows 启动器仍会把该依赖纳入
wheel 预检、安装和复核。如果可选包不能安装，环境准备会在脚本启动前失败，所以
`?` 只供脚本执行后的能力降级逻辑使用，不会跳过 Windows 的运行环境预检。

## Linux 目标机系统依赖

Qt 运行时不打包（跨发行版打包 Qt 是 linuxdeployqt/AppImage 的领域），
用系统包安装运行时：

```
sudo apt install libqt5quick5 libqt5qml5 libqt5qmlmodels5 \
     qml-module-qtquick2 qml-module-qtgraphicaleffects \
     python3 python3-venv
```

## 注意

- 目标目录里有正在运行的实例时重跑构建，文件占用会导致装配失败——
  先退出旧实例再构建。
- Windows 首次启动需要联网安装依赖；若系统 Python/venv 和官方用户 runtime
  都无法满足声明依赖，启动器会报告环境准备失败并且不启动脚本。
- 发行装配只认 `output/x64-<前端>/<Config>/` 这一个命名：换前端时目录名跟着
  `GO_BUNDLE_DIR` 走，不要手工搬动产物。

## 产物审计与剪除（bundle 管线末尾）

windeployqt 只增不删，会捎带用不到的行李。bundle 管线末尾按 objdump 导入
闭包审计结论统一剪除（每次装配自动执行，必须放在最后——exe 每次重链
windeployqt 都会把文件灌回来，末尾统一清一遍才保证终态干净）：

- 剪：`Qt5RemoteObjects.dll` + `QtQml/RemoteObjects/`、`virtualkeyboard/` +
  `platforminputcontexts/`（死插件）、`qmltooling/`（调试器）、`bearer/`、
  `imageformats/` 除 `qico`（QIcon 读 .ico 必需）
- 留：`Qt5Network.dll`（Qt5Qml/Qt5Quick 静态导入，剪=启动失败）、
  `libEGL/libGLESv2/D3Dcompiler_47`（ANGLE 链）、`opengl32sw.dll`
  （软件渲染兜底；项目约束"兼容性 > 体积"）、`imageformats/qico.dll`
- 中文 IME 不受影响：qwindows.dll 内建 IMM32 路径，与被剪的
  platforminputcontexts 插件无关

加新剪除项前先跑导入闭包分析（`objdump -p <dll>`）。发行 zip 体积随依赖和前端变化，
不作为固定契约值写入本文。
