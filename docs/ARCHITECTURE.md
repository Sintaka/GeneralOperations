# 架构 —— 分层、契约与扩展点

> AGENTS.md 与根 `CMakeLists.txt` 引用的「关键契约」「新增一个前端」「新增一个
> 后端」三节都在本文。建议阅读顺序：本文 → `docs/SCRIPT_SPEC.md`（脚本契约）
> → 各目录自己的 README。
>
> 本文与代码不一致时，以代码为准，并回来改本文——契约漂移比实现错误更难发现。

## 分层总览

```
        ui/ —— 前端层（可替换的壳；一次只构建一个）
┌─────────────────────┬──────────────────────────┬─────────────────────────┐
│ ui/qt5              │ ui/qt6.8lts（占位）       │ ui/tauri2（占位）        │
│ Qt 5.15 QML 启动器  │ Qt 6.8 LTS QML           │ Tauri2 + WebView2       │
│ target:             │ 规划：                   │ 规划：                  │
│ GeneralOperations-  │ ui/qt6.8lts/README.md    │ ui/tauri2/README.md     │
│ Launcher            │                          │                         │
└─────────────────────┴────────────┬─────────────┴─────────────────────────┘
                                   │ 只消费 GO_* 变量与脚本 docstring 契约
                                   ▼
        契约层（不是一个目录，是四个单点约定）
┌──────────────────────────────────────────────────────────────────────────┐
│ 1. 根 CMakeLists.txt：前端选择、GO_* 变量、输出与 zip 命名；               │
│    全仓库唯一 project()，版本号只此一处                                   │
│ 2. docs/SCRIPT_SPEC.md：脚本 docstring 声明块，前端与打包管线共同消费     │
│ 3. 脚本目录解析链：exe 同级 scripts/ → 开发期 GO_DEV_SCRIPTS_DIR → 报错   │
│ 4. bundle 装配管线：POST_BUILD 装配 output/x64-<前端>/<Config>/           │
│    （管线脚本由前端持有，入参见「关键契约」）                             │
└──────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
        core/ —— 内核层（后端）
┌──────────────────────────────────┬───────────────────────────────────────┐
│ core/python（完整）              │ core/cpp（占位）                      │
│ scripts/ 下 7 个脚本，           │ go_core_cpp（INTERFACE 库）：         │
│ docstring 头即契约（没有枚举清   │   flip_horizontal_rgba8 / udim_mirror │
│ 单，加脚本零 C++/CMake 改动）；  │ go_core_cpp_tests（进默认构建）       │
│ 导出 GO_PYTHON_SCRIPTS_DIR       │                                       │
└──────────────────────────────────┴───────────────────────────────────────┘
```

### 依赖方向铁律

**ui → core 单向；core 对 ui 与 Qt 零感知。** 展开成三条可执行的规则：

1. 前端接触后端只有两个通道：读 `GO_CORE_SCRIPTS_DIR` 里各脚本的 docstring 头
   （契约见 `docs/SCRIPT_SPEC.md`），以及把这些文件原样拷进发行包。不存在
   "前端 import 后端代码"这回事——core/python 的脚本对 C++ 侧完全透明，
   core/cpp 目前只有头文件纯函数。
2. `core/` 禁止 include/import 任何 ui 与 Qt。`core/python/scripts/` 禁止
   Qt 绑定（PySide/PyQt/qtpy），由 `tools/check_repo.py` 与打包期 bundle
   双重拦截；`core/cpp` 零 Qt 头、零第三方依赖。原因：脚本要跑在任意无 GUI
   的部署形态里（内嵌 Python、没装 Qt 的机器），Python 环境一旦沾 Qt，其
   版本与许可就被 Qt 绑架；C++ 内核同理必须能脱离 GUI 单测
   （`go_core_cpp_tests` 就是这个证明）。
3. 执行机制是"目录 + 依赖方向 + 根 CMake 单点"，不引入抽象框架：
   目录约定（ui/ 只放前端、core/ 只放内核）、`add_subdirectory` 的顺序与
   变量流向（先 core 后 ui，变量只从 core 流向 ui）、`tools/check_repo.py`
   静默检查兜底。

## Monorepo 还是子仓库（决策记录）

**决定：单 git 仓库。不用 git subtree、不用 git worktree、也不拆多仓库。**

1. **发布原子性（决定性理由）。** 发行 zip 同时装着前端二进制与内核脚本
   （`GO_ZIP_FILE`，见「关键契约」）。脚本改一行 `@param` 而前端没跟上（或
   反过来），在单仓库里是同一次构建、同一个 zip 的事；拆仓库后它变成"两个
   仓库的版本对齐问题"，发布脚本要跨仓库核对提交，而且出错样式是静默的——
   zip 里的脚本和 exe 各来自一个仓库，谁也不会报错。
2. **没有跨仓库协作需求。** 单人项目。subtree 解决的是"把上游仓库镜像进
   子目录、双方历史都保留、多团队各管一半"；worktree 解决的是"**同一个**
   仓库的多个分支并行签出"——worktree 是同仓库多分支的机制，与"多个项目
   如何布局"是两码事，拿它论证子仓库拆分属于概念错位。这两个工具针对的
   问题在这里都不存在。
3. **边界本来就不靠物理隔离。** 分层边界靠三件事维持（见上一节）：目录、
   依赖方向、根 CMakeLists 单点。这三条在任何仓库布局下都得靠纪律执行；
   仓库物理隔离提供不了额外保证，只会把"加一个前端动五处"（见
   「新增一个前端」）变成"再加仓库登记、CI 对齐、发布联动"。

接受的代价：前端演进与内核脚本的提交共享同一条历史。单人项目里
"一轮改动 = 一个提交"（AGENTS.md 收尾流程）反而贴近真实工作单元，可以接受。

## 关键契约

前端、内核、打包三方共同依赖的约定全部收口在此。改任何一条 = 三方同时动，
所以逐条列出；与根 `CMakeLists.txt` 不一致时以代码为准并回来改本节。

### 变量（GO_*）

| 变量 | 定义处 | 语义 |
|---|---|---|
| `GO_FRONTEND` | 根 CMakeLists（cache） | 前端选择：`qt5`（默认）\| `qt6.8lts`（占位）\| `tauri2`（占位） |
| `GO_PYTHON_SCRIPTS_DIR` | core/python/CMakeLists（PARENT_SCOPE 导出） | "脚本在哪里"是 core/python 的自述，根不写死路径——将来脚本换目录只动 core/python 一处 |
| `GO_CORE_SCRIPTS_DIR` | 根 CMakeLists（当前无条件 = `GO_PYTHON_SCRIPTS_DIR`，即 `core/python/scripts`） | 内核无关的脚本目录。前端与打包管线只认这个名字，不需要知道脚本资产由哪个内核提供；两个内核无条件一起构建，指向仍由根这单点维护 |
| `GO_BUNDLE_DIR` | 根 CMakeLists | 发行装配根：`output/x64-<前端>`；装配落在其 `<Config>` 子目录（Debug/Release）。带前端段：不同前端的产物落到各自子目录互不覆盖；带 x64 段：为将来非 Windows/非 64 位产物留位 |
| `GO_ZIP_FILE` | 根 CMakeLists | 发行 zip 绝对路径（命名见「输出与命名」）；**仅单配置（`CMAKE_BUILD_TYPE` 非空）下定义** |
| `GO_DEV_SCRIPTS_DIR` | 前端 CMakeLists（编译期注入，值取 `GO_CORE_SCRIPTS_DIR`） | 开发期脚本解析路径，见「脚本目录解析链」 |

### 目标（target）

| 目标 | 定义处 | 语义 |
|---|---|---|
| `GeneralOperationsLauncher` | ui/qt5 | qt5 前端可执行（目标名沿用第二代仓库，发行包入口 exe 同名） |
| `deploy` | ui/qt5 | windeployqt 收集 Qt 运行时与 QML 模块（qt5 前端） |
| `release_zip` | 前端（消费根的 `GO_ZIP_FILE`） | 把 `GO_BUNDLE_DIR/<Config>/` 压成发行 zip（`cmake -E tar` 走 libarchive，写 zip 不需要外部工具） |
| `go_core_cpp_tests` | core/cpp | C++ 内核自测可执行，**进默认构建**（不挂 `BUILD_TESTING` 门后）——占位代码一旦开始实现就有测试挂载点，不需要改根。ctest 里对应测试名 `go_core_cpp` |

### 脚本目录解析链（前端运行期）

```
① 发行 exe 同级 scripts/         ← 发行形态；保证测的就是交付物本身
② 开发期 GO_DEV_SCRIPTS_DIR      ← CMake 编译期注入的源码树路径；build/ 里的裸 exe 生成即可跑
③ 都不存在 → 明确报"目录不存在"  ← 不静默回退成空列表：装作"没有脚本"比报错更误人
```

顺序有意**发行优先**：开发期路径只是让 `build/` 下的裸 exe 不经装配就能跑起来
的便利；任何改动最终都要通过发行形态（解析链 ①）验证。

### 输出与命名

| 产物 | 路径 |
|---|---|
| 构建目录 | `build/<presetName>/`（如 `build/qt5-mingw-debug/`） |
| 自包含发行装配 | `output/x64-<前端>/<Config>/`，默认前端即 `output/x64-qt5/Release/` |
| 发行 zip | `output/GeneralOperations-<版本>_x64-<前端>-<release\|debug>.zip`，如 `GeneralOperations-0.2.000_x64-qt5-release.zip` |

- 版本号唯一来源：根 `project(GeneralOperations VERSION 0.2.xxx)`，子项目不得
  声明版本（版本策略见「血统」）。
- zip 名的 `<release|debug>` 是 `CMAKE_BUILD_TYPE` 的小写。多配置生成器一次
  configure 出多个配置、按哪个算 zip 名都不对，所以根 CMakeLists 对空
  `CMAKE_BUILD_TYPE` 守卫：`GO_ZIP_FILE` 不定义、`release_zip` 不可用并显式
  警告。本项目全部 preset 都是 Ninja 单配置，这条守卫只对"有人手改生成器"的
  场景生效——此时明确警告，而不是悄悄产出错名的 zip。

### preset 命名

现有：`qt5-mingw-debug`（默认）\| `qt5-mingw-release` \| `qt5-mingw-deploy` \|
`qt5-msvc-debug` \| `qt5-msvc-release` \| `qt5-msvc-deploy`。

约定：`<前端><工具链>-<配置>`；deploy 是 build preset（复用对应 release 的
configure，附 `--target deploy`）。所有非 hidden 的 configure preset 显式带
`GO_FRONTEND`——这是未来新前端 preset 的模板（qt6.8lts 已预记
`qt68lts-mingw-debug` 这类名字，见 `ui/qt6.8lts/README.md`）。

### bundle 管线参数（前端持有）

bundle 装配脚本由前端持有（约定放在前端目录的 `cmake/` 下，qt5 即
`ui/qt5/cmake/bundle.cmake`，POST_BUILD 以 `cmake -P` 调用），入参分两组：

| 参数 | 传入 | 用途 |
|---|---|---|
| `BUNDLE_SCRIPTS` | `GO_CORE_SCRIPTS_DIR` | 脚本源：拷进装配目录 `scripts/`，并汇总 `@requires` 生成 requirements.txt |
| `BUNDLE_QMLDIR` | 前端 QML 源目录 | windeployqt 的 `--qmldir`，据此决定部署哪些 QML 模块 |
| `BUNDLE_CACHE` | 仓库根 `.cache/` | embeddable Python zip、get-pip.py 与 Real-ESRGAN zip 的下载缓存（只下一次） |
| `BUNDLE_EXE` / `BUNDLE_OUT` / `BUNDLE_CONF` / `BUNDLE_WIN` / `BUNDLE_MINGW` / `BUNDLE_WDT` / `BUNDLE_QT_BIN` | 同第二代仓库 | exe 路径、装配目录、配置、平台/工具链旗标、windeployqt 与 Qt bin 路径，语义不变 |

为什么把第二代仓库的单参数 `BUNDLE_SRC`（源码树根）拆成三个：旧仓里脚本
（`scripts/`）、QML（`src/qml`）、下载缓存（`.cache/`）都住在同一棵源码树下，
一个根参数全推出来；monorepo 里脚本不再住在前端目录下，三者各在一处，推断
失效——拆成显式参数后，前端对"脚本在哪里"零假设，这正是
`GO_CORE_SCRIPTS_DIR` 存在的意义。管线行为细节见 `docs/RELEASE.md`。

## 新增一个前端

以假想的 `foo` 前端为例。**五件套，缺一不可**——三份根文件（CMakeLists、
CMakePresets、tasks.json）互相引用 preset 名，漏一件就会在某一环断链：

1. **建 `ui/foo/` 目录。** CMake 前端放 `CMakeLists.txt` + 前端实现 +
   `README.md`。规划期可以先只放 README 占位（ui/qt6.8lts、ui/tauri2 就是
   这么做的），此时根分支用 `message(FATAL_ERROR ...)` 明确提示"尚未实现"，
   保留目录与名字。前端消费 `GO_CORE_SCRIPTS_DIR` 与 `docs/SCRIPT_SPEC.md`，
   禁止自己再发明脚本发现机制或第二套命名。
2. **根 `CMakeLists.txt` 加分支。** CMake 前端：
   `elseif(GO_FRONTEND STREQUAL "foo")` → `add_subdirectory(ui/foo)`；不走
   CMake 的前端照 tauri2 的样子直接 `FATAL_ERROR` 提示。这是 monorepo 多
   前端的唯一扩展点，"一次只构建一个前端"是有意保持的约束——前端决定整棵
   GUI 目标树（Qt 版本、QML 模块、windeployqt 差异），不存在同一次构建里
   两个前端并存的需求。
3. **`CMakePresets.json` 加 preset 组。** configure preset 继承 hidden
   `base`（+ 该前端的工具链组），显式带 `GO_FRONTEND=foo`；
   build preset 同名对应，deploy 变体复用 release configure 附
   `--target deploy`。命名 `<前端><工具链>-<配置>`。
4. **`.vscode/tasks.json` 加任务组。** 照 qt5 的任务组复制改 preset 名：
   配置 / 生成 / 运行 / 发行 zip。运行任务的 cwd 必须指到 exe 目录——保证
   产物以"最终运行环境"的方式被启动（与双击行为一致，脚本目录解析链依赖它）。
5. **文档索引。** 根 `README.md` 的文档索引加一行 `ui/foo/README.md`；前端
   README 写清定位、构建命令与前端专属约定。

验证清单（全过才算接完）：`cmake --preset foo-<工具链>-debug` configure 通过，
且结尾摘要的 `[GO]` 各值正确 → 构建成功 → 运行 exe 能看到脚本树（解析链 ②
生效）→ `--target release_zip` 产出
`output/GeneralOperations-<版本>_x64-foo-<配置>.zip`，解压后拷到别的
目录也能跑（解析链 ① 生效）。

## 新增一个后端

后端不再是一个可选项：编译与发行不区分后端，`core/` 下所有内核无条件一起
构建、一起进发行——产物目录与 zip 名只带前端段，没有后端段。以假想的
`core/foo` 为例（真 C++ 内核、或未来任何工具链内核），接入只需两步：

1. **建 `core/foo/`。** 内核实现 + `README.md`；若走 CMake，其 `CMakeLists.txt`
   只做两件事：挂 target（命名 `go_<名>_*` 或按需），并向父作用域导出自己的
   资源/脚本目录变量（模式照 `GO_PYTHON_SCRIPTS_DIR`：目录是子目录的自述，
   根不写死）。零 Qt、零 ui 依赖——依赖方向铁律对内核同样成立。
2. **根 `CMakeLists.txt` 加一行。** `add_subdirectory(core/foo)`，不做任何
   条件分支——与前端唯一的结构差异就在这里：前端决定整棵 GUI 目标树，一次
   只构建一个所以走分支；内核彼此正交，无条件全部一起构建。

`GO_CORE_SCRIPTS_DIR` 的指向仍由根单点维护（当前指向 `GO_PYTHON_SCRIPTS_DIR`）：
前端与打包管线只认这个名字，新内核若也提供脚本资产，改根这一处指向即可，
前端零改动；bundle 管线只消费 `GO_CORE_SCRIPTS_DIR`（经 `BUNDLE_SCRIPTS`），
非脚本型内核不碰它，无需适配。

验收：configure 后 `[GO]` 摘要正确（`GO_CORE_SCRIPTS_DIR` 指向正确、
`GO_BUNDLE_DIR` / `GO_ZIP_FILE` 命名无后端段）；前端运行与发行流程不回归。

## 血统

三代仓库，一条主线：**把"拖一拖就能用"的散装脚本收口成一个启动器。**

| 代 | 仓库 | 形态 |
|---|---|---|
| 第一代 | `D:\code\dev\python\GeneralOperations` | 纯 Python 拖拽脚本集，13 个文件（含 3 个 .bat 拖拽启动器）。每个脚本各自配 bat 重复实现拖拽适配样板，且直接用系统 Python |
| 第二代 | `D:\code\dev\qt\GeneralOperations-Qt5` | Qt5 QML 启动器单仓：脚本适配成 docstring 契约头收进 `scripts/`，启动器收口"统一入口 + Python 环境托管"。版本止于 0.1.00x |
| 第三代 | 本仓库（`D:\code\dev\GeneralOperations`） | monorepo：多前端（ui/）× 多后端（core/），ui/qt5 由第二代仓库迁移而来。版本自 **0.2.000** 起算 |

注意：第一代仓库的 README 已陈旧（引用了不存在的文件名），**勿以其为事实
来源**；脚本事实以下表与本仓库 `core/python/README.md` 为准。

### 脚本映射表

第一代 13 个源文件的完整去向（第二代完成适配与合并，第三代原样迁入
`core/python/scripts/`）：

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

合并结果：**13 个源文件 → 7 个内核脚本**（5 个重写保留；EXR2PNG 两版合并为
`Image/Format Convert/img.EXR2PNG_Large.py`；Resize 三档合并为
`Image/ReSize/img.Resize_jpg.py`；3 个 .bat 启动器被 GUI 取代淘汰，未迁移）。

对应的 C++ 内核占位（core/cpp）挑了两个函数下沉为内存纯函数：
`flip_horizontal_rgba8` ↔ `img.FlipImage_Horizontal`、`udim_mirror` ↔
`img.Zbrush_UDIM_Correction` 的数学核心（见 `core/cpp/README.md`）。

### 版本策略

- 版本号全仓库只此一处：根 `project(... VERSION 0.2.xxx)`；发行 zip 文件名
  携带它；AGENTS.md 收尾流程规定每轮收尾 patch 位 +1。
- 0.1.x 属于第二代旧仓库，本仓库从 **0.2.000** 起算——monorepo 重构在发布
  语义上是一次断代（输出路径、zip 命名、构建入口全变），不与旧版本号连续。
- 未来接入 qt6.8lts / tauri2 前端**不**换主版本号：前端是同一产品的可替换
  组件，版本跟着用户可见的产物走，不跟着仓库内部结构走。
