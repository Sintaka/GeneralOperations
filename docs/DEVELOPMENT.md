# 开发指南（monorepo 级）

> 前端专属的约定与设计来源在 `ui/qt5/README.md` 和
> `ui/tauri2/README.md`；脚本侧契约在
> `docs/SCRIPT_SPEC.md`；本文只写跨目录成立的约定。每轮收尾流程
> （清理 / 静默检查 / 版本 bump / 提交）见 `AGENTS.md`，此处不重复。

## 这个项目要解决什么

血统见 `docs/ARCHITECTURE.md`「血统」。源项目（第一代拖拽脚本集）有两个真实痛点：

1. **没有统一入口。** 每个脚本各自配一个 .bat 重复实现拖拽适配：可执行程序
   定位 + 存在性校验、`%~dp0` 定位配套脚本、"有拖拽参数 vs 无参数走交互输入"
   双入口、多文件取参——同样几块样板在每个 bat 里抄一遍，写法还互相不一致。
2. **直接用系统 Python。** 版本和依赖受系统状态影响，装个别的东西就可能把
   脚本搞坏。

启动器把这两件事收口：左侧 Outliner 选脚本、右侧调参数、拖文件进去执行；
Python 环境由启动器托管。monorepo 化在此基础上再加一层目标：**前端可替换**
（qt5 / tauri2 已接线，qt6.8lts 保留槽位），内核（core/）无条件一起构建、不参与选择——产物
与发行命名只区分前端，靠根 CMake 的 GO_* 契约单点收口，扩展步骤见
`docs/ARCHITECTURE.md` 的「新增一个前端」「新增一个后端」。

## 约束优先级

**兼容性 > UI > 体积。** 顺序是硬的，冲突时按这个顺序让步。

这决定了很多看起来"保守"的选择。例如：毛玻璃不去调 DWM 私有 API 做桌面透视
（跨 Windows 版本行为不一致），改用窗口内模糊、纯 Qt 实现；windeployqt 装配
刻意**不加** `--no-opengl-sw`，保留软件光栅 `opengl32sw.dll` 作为驱动不给力时的
兜底。Windows Python 启动先验证系统 CPython 与依赖，无法满足时准备用户 runtime
中的官方 Python 3.13.15 环境（见 `docs/RELEASE.md`）。

## 工具链

| 项 | 值 | 说明 |
|---|---|---|
| Qt | `D:/Lib/Qt/5.15.2/mingw81_64` | 默认 kit |
| 编译器 | GCC 8.1（Qt 自带 `Tools/mingw810_64`） | 与该 Qt kit 配套构建，ABI 必然匹配 |
| 生成器 | Ninja（Qt 自带 `Tools/Ninja`） | 全部 preset 均为 Ninja 单配置 |
| preset | 仓库根 `CMakePresets.json` | preset 收敛到 monorepo 根；子目录不设自己的 preset |

**默认 MinGW。** preset 里编译器/ninja 全是绝对路径，VSCode 里**不需要选
kit、不依赖 PATH**，打开就能配。MSVC preset 保留为备选（产物小约 10MB），
但必须在 VSCode 里选 MSVC v142/v143 amd64 kit 注入编译器环境——Ninja 不像
VS 生成器会自己找；kit 选错会在 configure 阶段就报找不到编译器。

**Qt5 前端不能用 `D:/Lib/mingw1520_64` 独立 GCC 15.2**——它与 Qt 自带的
GCC 8.1 不是同一套 C++ 运行时。Tauri 发行入口单独构建零 Qt 的 core/cpp，
可以用独立 GCC，并静态链接 C++ 运行时。

Tauri2 的 Cargo target、Cargo 依赖缓存、npm 缓存与 Vite 输出统一放在根 `build/tauri2-*`；
`ui/tauri2/node_modules/` 是供 TypeScript/Vite 解析的本地依赖，不参与发行 zip。

## 构建与常用命令（全部从仓库根执行）

```
cmake --preset qt5-mingw-debug              # 配置（默认 preset）
cmake --build --preset qt5-mingw-debug      # 构建；POST_BUILD 自动装配 output/x64-qt5/Debug
build/qt5-mingw-debug/GeneralOperationsLauncher.exe    # 运行（VS Code 任务"运行 (qt5-debug)"）

cmake --build --preset qt5-mingw-release --target release_zip    # 发行 zip（构建 + 装配 + 压包）
cmake --build --preset qt5-mingw-debug --target go_core_cpp_tests  # 只编 C++ 内核自测
ctest --test-dir build/qt5-mingw-debug --output-on-failure         # 跑上面编出的测试
python tools/check_repo.py                # 仓库卫生静默检查（每轮收尾必跑）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package_tauri2.ps1
                                          # Tauri 2 + 同一 core 的 Windows 便携 zip
```

对应的 VS Code 任务在 `.vscode/tasks.json`（命名 `(qt5-debug)` / `(qt5-release)`
对应 preset）；发行装配布局与 zip 命名见 `docs/RELEASE.md`。

## 仓库结构与依赖方向

```
ui/     前端层：qt5、tauri2；qt6.8lts（占位）
core/   内核层：python（脚本/契约卡完整）；cpp（共享声明解析 + 纯函数 + PMX→GLB 后端 + 测试）
docs/   ARCHITECTURE / DEVELOPMENT / RELEASE / SCRIPT_SPEC / 两个 map / pitfalls
tools/  共用 bundle、Tauri 发行入口、check_repo.py 静默检查
```

依赖方向 **ui → core 单向**，core 对 ui 与 Qt 零感知——规则、原因与执行机制
见 `docs/ARCHITECTURE.md`（分层总览 / 依赖方向铁律 / 关键契约），此处不重复。

## 增量切片状态

开发方式是每次只做一个能跑起来的纵切片（不摊平做完某一层），状态以代码为准。
现状：切片 1–4（构建骨架 / 脚本清单 / 进程托管 / 参数面板）已随 qt5 前端
落地；Windows Python 运行时由 Qt5 与 Tauri2 共用的 `python-launcher.ps1` 在
目标机按需准备，发行 zip 不带 Python 安装目录或 Real-ESRGAN 工具（见
`docs/RELEASE.md`）。脚本执行队列仍未做；发行体积和首次准备耗时随前端、依赖和目标机变化，本指南不固定记录具体数值。

## 代码约定

**通用条目（任何目录都适用）：**

- 注释写**为什么**，不写**做了什么**。代码本身说得清做了什么。
- 检查一律写成脚本跑（新增检查项直接往 `tools/check_repo.py` 里加），禁止
  靠人眼逐字核对。
- `.cpp` 里的类不要加 `Q_OBJECT`，除非它真的需要自己的信号槽——加了就必须
  在文件末尾 `#include "xxx.moc"`，忘了是一串莫名其妙的链接错误；需要信号槽
  就老老实实拆出 `.h`。
- MSVC 必须 `/utf-8`，否则源码里的中文字面量会按 GBK 解读、运行时乱码
  （GCC 默认就是 UTF-8，不需要对应物）。新增 C++ 目标时沿用这一做法。
- 文本源文件一律 UTF-8 无 BOM（`tools/check_repo.py` 会查）。

**脚本侧**（`core/python/scripts/`）的契约与硬规则见 `docs/SCRIPT_SPEC.md`
（无 Qt、无阻塞调用、stdout 一行一条进度、退出码准确）。

**前端专属**（视觉常量从 Theme 取、状态过渡用 `Behavior`、改了 QML 就跑
QML 检查脚本、设计来源等）只在此提一句，详见 `ui/qt5/README.md`。
