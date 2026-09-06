# GeneralOperations Launcher（Qt5 前端）

统一脚本启动器的 Qt5 Quick (QML) 前端，monorepo 的默认前端（`GO_FRONTEND=qt5`）。左侧选脚本，右侧调参数，拖文件进去执行；Python 环境由发行包自带，不碰系统 Python。

Qt5 Quick (QML) / C++17 / MinGW（MSVC 备选）。

## 本前端在 monorepo 中的位置

- **前端不持有脚本副本。** 脚本实体统一住在内核 `core/python/scripts/`（7 个 `.py`，docstring 头即契约，规范见 [`docs/SCRIPT_SPEC.md`](../../docs/SCRIPT_SPEC.md)）。
- **开发期**：CMake 把内核脚本目录以 `GO_DEV_SCRIPTS_DIR` 宏编译进 exe（见本目录 `CMakeLists.txt`），`build/` 里的裸 exe 不经装配就能跑。
- **发行期**：bundle 管线把 `core/python/scripts` 拷到 exe 旁的 `scripts/`。
- 运行期解析链（`src/main.cpp` 的 `resolveScriptsDir()`）：exe 同级 `scripts/` → `GO_DEV_SCRIPTS_DIR` → 都没有则明确报"目录不存在"，不静默回退成空列表。完整契约见 [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md)「脚本目录解析链」。

## 当前状态

可用：左侧 Outliner 以两级树显示真实脚本树（大类 = `@group` 首段即脚本目录第一层，子分类 = 第二层；点大类头折叠整个大类并显示该大类脚本总数，点子分类头只折叠该子分类并显示其成员数；破坏性脚本行首红色感叹号、解析失败的脚本灰掉且不可选中）；右侧参数面板按 docstring 的 `@param` 动态生成控件（int/float 滑块 + presets 快捷档位胶囊、bool 自绘复选框、choice 互斥胶囊组；str/path 参数暂不支持编辑，按默认值跑）；拖多文件执行，破坏性脚本先二次确认；stdout/stderr 合流回显到底部输出块；`@host blender` 脚本经 `blender --background --python <脚本> <flags> -- <files>` 执行；`GeneralOperationsLauncher.exe <脚本> <文件...>` 命令行直跑是自动化验证入口。

未做：脚本执行队列（一次一个进程，运行中再触发会拒绝）、执行历史、设置持久化面板。

界面配色照鹰角启动器，行几何与交互照 dsh —— 都是从各自样式表里量出来的值，出处见下面"设计来源"一节。

## 构建

在**仓库根**执行（preset 已收敛到根 `CMakePresets.json`）。需要 `D:\Lib\Qt\5.15.2\mingw81_64`；编译器和 ninja 用 Qt 自带那套（`Tools\mingw810_64`、`Tools\Ninja`），preset 里写的是绝对路径，所以 **VSCode 里不需要选 kit，也不依赖 PATH**。

```
cmake --preset qt5-mingw-debug
cmake --build --preset qt5-mingw-debug
```

Qt 路径写在根 `CMakePresets.json` 的 `CMAKE_PREFIX_PATH`，装在别处就改那里。

MSVC 作为备选保留（产物小约 10MB），但它需要在 VSCode 里选 MSVC v142/v143 amd64 kit 来注入编译器环境 —— Ninja 不像 VS 生成器会自己找。

构建产物两份：

- **开发期** `build/<preset>/` —— exe + Qt 运行时 dll + qt.conf（绝对路径指回本机 Qt 安装，仅开发期用）。
- **发行装配** `output/x64-qt5/<Debug|Release>/` —— bundle 管线每次构建后增量装配：windeployqt 收 Qt 运行时与 QML 模块、qt.conf 相对化、内核脚本拷到 exe 旁 `scripts/`、内嵌 Python（Windows）/run.sh（Linux）。整目录拷走即可分发，细节见 [`docs/RELEASE.md`](../../docs/RELEASE.md)。

分发：

```
cmake --build --preset qt5-mingw-deploy    # release + windeployqt 收集 dll 和 QML 模块
cmake --build --preset qt5-mingw-release --target release_zip
```

发行 zip：`output/GeneralOperations-<版本>_x64-qt5-<release|debug>.zip`（版本号唯一来源是根 CMakeLists 的 `project(... VERSION ...)`，zip 名由根定义的 `GO_ZIP_FILE` 决定）。

## 改了 QML 之后

在仓库根执行：

```
pip install PySide2
python ui/qt5/tools/qmltest/qmlcheck.py   ui/qt5/src/qml   # 解析 + 实例化
python ui/qt5/tools/qmltest/themecheck.py ui/qt5/src/qml   # Theme 引用是否都存在
python ui/qt5/tools/qmltest/behaviour.py  ui/qt5/src/qml   # Outliner 折叠行为
```

不需要装 Qt 开发包。QML 的错误绝大多数是静默的（引用不存在的属性不报错，只是得到 `undefined`，然后颜色变透明、数值变 0），这套检查实际抓到过三个这类 bug。详见 [`tools/qmltest/README.md`](tools/qmltest/README.md)。

> 以下「设计来源」「性能取向」「代码约定（QML/Theme 条目）」三节，自第二代仓库
> `GeneralOperations-Qt5` 的 `docs/DEVELOPMENT.md` 并入，内容未改；该文档随旧
> 仓库退役，此处是其中前端相关内容的唯一延续。

## 设计来源

界面不是凭感觉调的，是从两个真实产品的样式表里量出来的值，各取一半。全部
落在 `src/qml/App/Theme.qml` 里，那个文件的注释标注了每个值的出处。

**配色与表面 —— 鹰角启动器**（Hypergryph Launcher 1.5.0，`res/web/index.css`）

| 项 | 值 |
|---|---|
| 表面 | 近黑 `#18171a`，主面板 95% / 弹层 90% / 遮罩 78% |
| 描边 | `.5px solid #ffffff1a`（Qt 给不了 0.5px，改 1px 配折半的 alpha） |
| 阴影 | `0 2px 6px #0000008c` / `0 2px 8px #000000a6` |
| 模糊 | `blur(4px)`~`blur(6px)`，很克制 |
| 强调色 | 明黄 `rgb(253 252 0)`，其上文字必须是黑的 |
| 危险 | `#f96f68` |
| 悬停 | 纯白 15% 平铺叠加 |
| 圆角 | 主力 8px，胶囊 100px |
| 滚动条 | 4px 宽，thumb 白 25%，hover 白 50%，radius 3px |
| 字体 | Source Han Sans CN，12–13px / line-height 18px |

**行几何与交互 —— dsh**（deepseek-harness，`packages/client/ui-workspace`）

| 项 | 值 |
|---|---|
| 分组行 | 34px |
| 条目行 | 32px |
| 缩进步长 | 22px = 16px 图标槽 + 6px 间距 |
| 圆角 | 8px |
| 悬停/选中 | **同一个填充色**，选中只是更亮一档 |
| 行间距 | 2px；组间距 4px |
| 分组图标 | 默认文件夹，悬停换成折叠箭头，展开时箭头转 90°（150ms） |
| 行尾操作 | 只在悬停时出现 |
| 底部渐隐 | 24px |

两边共同遵守一条原则，也是这套界面的要点：**层次靠描边和阴影，不靠颜色饱和
度。** 所以 `Theme` 里除了唯一的强调色，没有任何高饱和色；背景光斑的 alpha
全部压在 0.10 以下，它们只负责给模糊提供亮度梯度（纯平底色模糊前后完全一样，
毛玻璃就白做了），不负责提供颜色观感。

有意没照搬的两处：

- **鹰角的噪点纹理**（`pattern3.png`，alpha 峰值 4/255 ≈ 0.016）。面板内部
  20px 模糊会直接抹掉这种高频细节；面板外部要么是数万次 rect fill，要么是
  几十万像素的 JS 循环，且每次 resize 重来。为 1.6% 不透明度的效果付这个代价
  不值得。真要加的正确做法是 ShaderEffect 里做 hash noise。
- **面板整体的 hover 提亮**。鹰角的面板是静态的，只有可点击控件才有 hover 态。
  面板跟着鼠标忽明忽暗会显得廉价，而且会与内部行的 hover 打架。

## 性能取向

毛玻璃的成本全在模糊上。这里的关键决定是：**背景由我们自己生成，因此是静态
的**。整窗只有一份 `Backdrop`，用一个 `ShaderEffectSource { live: false }` 包
成纹理——只渲染一次，之后各面板取样都是白拿。面板自己那层 `ShaderEffectSource`
也是 `live: false`，`FastBlur` 再 `cached: true`，所以 hover 之类的状态变化不会
重跑模糊。

如果背景改成动态的（比如播视频），这套全部不成立，得换思路。

几个具体取舍：

- **`sourceRect` 靠 `x`/`y` 而不是 `mapToItem()`。** 面板必须是窗口根 Item 的
  直接子节点，这样 `x`/`y` 就是窗口坐标且参与绑定跟踪。`mapToItem()` 是函数
  调用，只求值一次，不参与绑定，窗口一缩放就错位。所以 `GlassPanel` 不能塞进
  嵌套 Layout。
- **投影用 `RectangularGlow`，不用 `DropShadow`。** DropShadow 会把源 item
  整个跑一遍高斯模糊；我们要投的只是一个已知的圆角矩形，RectangularGlow 是
  这个特例的解析实现，不采样源纹理。
- **圆角只对"模糊+表面"做一次 `OpacityMask`。** 文字和图标留在遮罩外面——走
  OpacityMask 会让它们多经一次纹理采样，边缘发虚。
- **背景光斑用一个 `Canvas` 一次画完**，不堆多个渐变元素。Qt5 的 QtQuick 基础
  库没有径向渐变元素（在 QtGraphicalEffects 里，且每个都拖一个 ShaderEffect）。
  Canvas 只在尺寸变化时重绘。
- **`Theme.blurRadius = 20` 不是笔误。** 视觉目标是 CSS `blur(6px)`（鹰角原
  值），但 `FastBlur` 的 radius 与 CSS 像素不是同一量纲——它内部
  `lod = sqrt(radius/64)*1.2-0.2` 是非线性映射，直接写 6 在 Qt 下几乎看不见
  效果。反推得 20。改的时候别"修正"回 6。另外 radius 超过 64 后 lod 饱和，
  再加大只是白烧 GPU。

## 代码约定（QML / Theme）

- **视觉常量一律从 `Theme` 取**，QML 文件里不写死颜色、时长、圆角、行高。
  改配色应该只改 `Theme.qml` 一个文件。
- **状态过渡用 `Behavior on <属性>`**，不手写 `NumberAnimation` 去 start/stop。
  Behavior 会自动从当前值接管插值，不需要管"上一个动画还没结束"。
- **动画时长 150ms，缓动 `OutCubic`**（`Theme.durFast`）。超过 250ms 界面
  会显得拖沓。
- **改了 QML 就跑 `tools/qmltest/`**（见上文「改了 QML 之后」）。QML 的错误
  绝大多数是静默的：引用一个不存在的属性不报错，只是得到 `undefined`，然后
  颜色变透明、数值变 0。这套脚本不需要装 Qt 开发包（靠 pip 的 PySide2），
  实际抓到过三个这类 bug。

## 文档

- [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) —— monorepo 分层、GO_* 变量契约、脚本目录解析链、多前端扩展点
- [`docs/SCRIPT_SPEC.md`](../../docs/SCRIPT_SPEC.md) —— 脚本 docstring 头格式。加脚本只需往 `core/python/scripts/` 放个 `.py` 并写好头，C++ 侧不用改
- [`docs/RELEASE.md`](../../docs/RELEASE.md) —— bundle 装配与发行细节
- [`tools/qmltest/README.md`](tools/qmltest/README.md) —— QML 检查脚本，以及写它时踩过的三个坑
