# 打包与发行（output/ 发行装配）

> 每次构建后，POST_BUILD 的 bundle 管线把产物装配成
> `output/x64-<前端>-<后端>/<Debug|Release>/`（默认组合即
> `output/x64-qt5-python/Release/`）——**自包含目录**，整体拷走或压 zip 后即可
> 分发，不依赖开发机的任何绝对路径（构建期的 build/ 目录才依赖本机 Qt）。
> 管线脚本由前端持有（qt5 为 `ui/qt5/cmake/bundle.cmake`，POST_BUILD 以
> `cmake -P` 调用），入参契约见 `docs/ARCHITECTURE.md`「关键契约」。
>
> 本文改写自第二代旧仓库的 docs/RELEASE.md；路径与命名已按 monorepo 契约
> （`GO_BUNDLE_DIR` / `GO_ZIP_FILE`）更新，管线行为细节保持不变。

## 目录布局（Windows，qt5 + python 默认组合）

```
output/x64-qt5-python/Release/
├── GeneralOperationsLauncher.exe   # 图标来自 ui/qt5/src/assets/app.ico（ui/qt5/tools/gen_icon.py 生成）
├── qt.conf                  # Prefix=. —— 相对路径，指向自身
├── Qt5*.dll / lib*.dll      # windeployqt 装配的 Qt + MinGW 运行时
├── platforms/ 及各 QML 模块目录  # 平台插件 + QML 模块（VirtualKeyboard 已剪除）
├── scripts/                 # Python 脚本（来自 GO_CORE_SCRIPTS_DIR = core/python/scripts）
├── requirements.txt         # 由各脚本 @requires 行自动汇总生成
└── python/                  # 内嵌 Python 运行时（见下）
    ├── python.exe  python312.zip  ...
    └── Lib/site-packages/   # requirements.txt 装好的依赖
```

Linux 布局相同（同在 `output/x64-qt5-python/<Config>/`），但 Qt 走系统包，
没有 python/ —— 换成 run.sh + requirements.txt（见下）。

## bundle 管线入参（monorepo 的变化点）

POST_BUILD 调用 bundle 脚本时，第二代仓库的单参数 `BUNDLE_SRC`（源码树根）
被拆成三个显式参数——monorepo 里脚本不再住在前端目录下，三个推断路径各指一处：

- **`BUNDLE_SCRIPTS`** = `GO_CORE_SCRIPTS_DIR`（即 `core/python/scripts`）：
  脚本拷贝（进装配目录 `scripts/`）与 `@requires` 汇总的唯一来源。
- **`BUNDLE_QMLDIR`** = 前端 QML 源目录：windeployqt 的 `--qmldir`。
- **`BUNDLE_CACHE`** = 仓库根 `.cache/`：embeddable Python zip 与 get-pip.py
  的下载缓存（已 gitignore，只下一次）。

其余参数（`BUNDLE_EXE` / `BUNDLE_OUT` / `BUNDLE_CONF` / `BUNDLE_WIN` /
`BUNDLE_MINGW` / `BUNDLE_WDT` / `BUNDLE_QT_BIN`）语义不变。

## Python 运行时（不依赖系统 Python）

- **Windows**：构建期下载官方
  [embeddable 包](https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip)
  （3.12.10，约 11MB，缓存在 `BUNDLE_CACHE`，只下一次），展开到装配目录
  `python/` 并修改 `._pth` 启用 site-packages，再用 get-pip 引导 pip 按要求安装。
  **目标机零部署**：zip 解压即用。requirements.txt 由各脚本的 `@requires` 行
  自动汇总——给脚本加依赖不需要改打包配置，加完依赖重跑一次 bundle 即可重装。
- **Linux**：venv 绑定 glibc 和解释器 ABI，构建期预装没有可移植性，
  所以由 `run.sh` 在**首次运行**时 `python3 -m venv runtime/venv` 并
  安装 requirements.txt（要求目标机有 python3 + venv 模块 + 联网）。
- 两边都失效时，运行器自动回退系统 `python`/`py`（Windows）或
  `python3`（Linux），状态区会给出明确报错文案。

启动器选解释器的顺序：内嵌运行时 → 系统解释器；子进程环境强制
PYTHONUTF8=1 并清除用户 PYTHONPATH，依赖一律以本包 site-packages 为准。

## 使用

（全部从仓库根执行）

```
cmake --build --preset qt5-mingw-release        # 构建并装配 output/x64-qt5-python/Release
cmake --build --preset qt5-mingw-release --target release_zip   # 压成发行 zip
cmake --build --preset qt5-mingw-debug          # output/x64-qt5-python/Debug 同理
```

发行 zip：`output/GeneralOperations-<版本>_x64-qt5-python-<release|debug>.zip`
（版本取根 `project(... VERSION)`，命名规则 `GO_ZIP_FILE` 属于根契约，见
`docs/ARCHITECTURE.md`「关键契约」）。目标机解压到任意目录直接运行（Windows）；
Linux 解压后 `./run.sh`。

`qt5-mingw-deploy` build preset 是 release 构建外加 `deploy` 目标
（windeployqt 收集 Qt 运行时与 QML 模块）的一条龙入口。

## 逐包安装的容错

pip 对清单文件（`-r requirements.txt`）是全有或全无——一个包没有 wheel 会让
整批依赖装不上（实测 PyOpenColorIO 无 cp312 wheel）。所以 Windows bundle 和
Linux run.sh 都是**逐包安装**：单包失败只影响它自己的脚本，其余照常。

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
- pip 安装失败（断网/个别包无 wheel）不阻塞构建：运行器会回退系统
  Python，状态区会显示具体脚本缺什么。
- 发行装配只认 `output/x64-<前端>-<后端>/<Config>/` 这一个命名：换前端/后端
  组合时目录名跟着 `GO_BUNDLE_DIR` 走，不要手工搬动产物。

## 产物审计与剪除（bundle 管线末尾）

windeployqt 只增不删，会捎带用不到的行李。bundle 管线末尾按 objdump 导入
闭包审计结论统一剪除（每次装配自动执行，必须放在最后——exe 每次重链
windeployqt 都会把文件灌回来，末尾统一清一遍才保证终态干净）：

- 剪：`Qt5RemoteObjects.dll` + `QtQml/RemoteObjects/`、`virtualkeyboard/` +
  `platforminputcontexts/`（死插件）、`qmltooling/`（调试器）、`bearer/`、
  `imageformats/` 除 `qico`（QIcon 读 .ico 必需）、Python 的 pip 全套与
  numpy 构建期工具（requirements.txt 变更时重装分支会自动补回 get-pip）
- 留：`Qt5Network.dll`（Qt5Qml/Qt5Quick 静态导入，剪=启动失败）、
  `libEGL/libGLESv2/D3Dcompiler_47`（ANGLE 链）、`opengl32sw.dll`
  （软件渲染兜底；项目约束"兼容性 > 体积"）、`imageformats/qico.dll`
- 中文 IME 不受影响：qwindows.dll 内建 IMM32 路径，与被剪的
  platforminputcontexts 插件无关

合计剪除约 19.7MB。加新剪除项前先跑导入闭包分析（`objdump -p <dll>`）。
