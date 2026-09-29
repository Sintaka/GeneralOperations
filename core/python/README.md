# core/python —— Python 内核脚本

本目录是 monorepo 的**内核（core）层**：包含纯后端 Python 脚本和前端共用的
Windows Python 启动工具，不含任何前端 UI，也不构建 Python 代码（`.py` 源码即
交付物）。

## 硬性规则：禁止依赖 Qt

本目录脚本**禁止 `import` PyQt / PySide / qtpy 及任何 Qt 绑定**。
Qt 只属于 ui/ 层的启动器前端；脚本与 GUI 框架、Python 环境完全解耦，才能跑在
系统 Python 或目标机准备的运行时、无 GUI 的部署形态里。打包期 CMake 会扫描本目录脚本，发现 Qt 依赖
直接报错拦下。

同理，脚本内不允许出现 `input()`、`os.system('pause')`、tkinter 等阻塞/自绘
调用——脚本由启动器以子进程方式托管（契约详见根目录 `docs/SCRIPT_SPEC.md`）。

## 目录内容

```
core/python/
├── CMakeLists.txt   # 向父作用域导出脚本目录与共用 Windows runtime 工具目录
├── runtime/
│   ├── python-launcher.ps1 # Qt5/Tauri2 共用的 Windows 环境准备与脚本启动器
│   ├── runtime-check.py    # stdlib-only 的 CPython/pip/依赖探针
│   └── tests/              # runtime bootstrap 的 PowerShell 测试
└── scripts/         # 全部内核脚本（启动器递归自动发现，见下文）
    ├── Image/
    │   ├── Format Convert/  img.EXR2PNG_Large.py
    │   ├── ReSize/          img.Resize_jpg.py
    │   └── Edit/            img.FlipImage_Horizontal.py  img.crossSplit.py  img.Zbrush_UDIM_Correction.py
    ├── Geometry/
    │   └── Format Convert/  geo.pmx2fbx.py
    └── os.FlattenFolderHierarchy.py        # 留在根，@group System
```

目录即分组：`@group` 与脚本相对 `scripts/` 的目录路径一致（`/` 分隔层级，
规范见 `docs/SCRIPT_SPEC.md` 的 `@group` 键；一致性由 `tools/check_repo.py`
检查兜底）。启动器递归扫描整棵 `scripts/` 树，子目录层数不限。

## 溯源映射表

脚本源头是 `python/GeneralOperations` 源仓库（拖拽式脚本集），先由 Qt5 启动器
仓库（`qt/GeneralOperations-Qt5/scripts/`）适配成带 docstring 契约头的版本，
再原样迁入本目录。源项目 13 个文件的完整去向如下：

| 源文件（python/GeneralOperations） | 现文件（core/python/scripts） | 说明 |
|---|---|---|
| geo.pmx2fbx.py | Geometry/Format Convert/geo.pmx2fbx.py | 保留；适配为 `@host blender` 特例，Blender 路径改由脚本头 `@blender` 键声明，参数经 `--` 分隔符传入 |
| geo.pmx2fbx_launch.bat | 未迁移 | 单文件 PMX→FBX 拖拽启动器（硬编码 Blender 路径、pause 阻塞、日志重定向），职责由 GUI 启动器接管 |
| geo.pmx2fbx_launch_multi.bat | 未迁移 | 批量 PMX→FBX 拖拽启动器（调试日志 + 逐文件循环调用），职责由 GUI 启动器接管 |
| img.EXR2PNG_SceneLinear_sRGB.Display.py | 合并入 Image/Format Convert/img.EXR2PNG_Large.py | 全分辨率 EXR→PNG（ACES 显示变换）；新版把输出上限做成 `@param target`，target ≥ 原图长边时不缩放，等价原版 |
| img.EXR2PNG_to4K_SceneLinear_sRGB.Display.py | 合并入 Image/Format Convert/img.EXR2PNG_Large.py | 流式降采样 ≤4K 版（8K/16K 大图不爆内存）；即新版默认行为（target 默认 4096） |
| img.FlipImage_Horizontal.py | Image/Edit/img.FlipImage_Horizontal.py | 重写为 argparse + 契约头；Pillow 常规图与 OpenEXR 两条翻转路径保留 |
| img.Resize_to_1k_jpg.py | 合并入 Image/ReSize/img.Resize_jpg.py | 与 2k/4k 版仅 `MAX_PIXELS = 1024` 一行之差；对应 `@param max_pixels` 预设档 1024 |
| img.Resize_to_2k_jpg.py | 合并入 Image/ReSize/img.Resize_jpg.py | 原 `MAX_PIXELS = 2048`；对应预设档 2048 |
| img.Resize_to_4k_jpg.py | 合并入 Image/ReSize/img.Resize_jpg.py | 原 `MAX_PIXELS = 4096`；对应预设档 4096 |
| img.Zbrush_UDIM_Correction.py | Image/Edit/img.Zbrush_UDIM_Correction.py | 重写为 argparse + 契约头，补 `@destructive`（原地翻转 EXR 像素并按行镜像重排 UDIM 编号） |
| img.crossSplit.py | Image/Edit/img.crossSplit.py | 重写为 argparse + 契约头；中心十字切四块逻辑不变 |
| img.x2_0.bat | 未迁移 | Real-ESRGAN 2x 放大启动器（调用外部 realesrgan-ncnn-vulkan.exe），被 GUI 取代，暂无对应脚本 |
| os.FlattenFolderHierarchy.py | os.FlattenFolderHierarchy.py | 重写为 argparse + 契约头；展平深度做成 `@param levels` |

合并结果：13 个源文件 → 7 个内核脚本（5 个重写保留；5 个两两合并——
EXR2PNG×2 → 1 个、Resize×3 → 1 个；3 个 .bat 启动器被 GUI 取代淘汰）。

## 如何新增脚本

1. 把一个 `.py` 放进 `scripts/` 下对应分组的子目录（目录路径即 `@group`，
   `/` 分隔层级；新分组就建新目录）；
2. 在文件开头写好 docstring 契约头（`@name` / `@group` / `@desc` / `@accepts`
   为必填，规范与示例见根目录 `docs/SCRIPT_SPEC.md`；`@group` 写脚本相对
   `scripts/` 的目录路径）；
3. 完成。启动器下次启动自动发现该脚本，C++ 侧与 CMake 侧都不用改——
   `CMakeLists.txt` 只导出目录路径，不枚举文件。

脚本硬规则（无 Qt 依赖、无阻塞调用、stdout 一行一条进度、退出码准确）同样
见 `docs/SCRIPT_SPEC.md` 的"移植时的硬规则"一节。

## Windows Python 运行时工具

Qt5 与 Tauri2 共用 `runtime/python-launcher.ps1` 和 `runtime/runtime-check.py`。
发行 zip 仅携带这两个小型启动工具，不携带 Python 解释器、site-packages 或
Real-ESRGAN；目标机首次运行时由启动器验证可用系统 CPython 和依赖，需要时再在
用户 runtime 准备 Python/venv。Real-ESRGAN 由对应脚本首次使用时下载到同一用户
runtime 缓存，依赖不可用时脚本可使用 Pillow 的非 AI 路径。契约见
[`docs/RELEASE.md`](../../docs/RELEASE.md) 与 [`docs/SCRIPT_SPEC.md`](../../docs/SCRIPT_SPEC.md)。
