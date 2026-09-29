# QML 静态/行为检查

不需要装 Qt 开发包，也不需要能编译 C++ —— 靠 pip 装的 **PySide2**（内含
Qt 5.15.2 运行时和 QtGraphicalEffects，与本项目目标版本一致）把 QML 真的
加载、实例化、跑起来。

```bash
pip install PySide2
cd <monorepo 仓库根>
python  ui/qt5/tools/qmltest/qmlcheck.py   ui/qt5/src/qml   # 解析 + 实例化全部 QML
python  ui/qt5/tools/qmltest/themecheck.py ui/qt5/src/qml   # Theme.<prop> 引用是否都存在
python  ui/qt5/tools/qmltest/behaviour.py  ui/qt5/src/qml   # Outliner 折叠行为
python  ui/qt5/tools/qmltest/smoke_main.py ui/qt5/src/qml   # 真实 main.qml 冒烟 + Backdrop 重绘计数
```

四个脚本都以退出码表示结果（0 通过），可直接进 CI。

## 为什么需要这套东西

QML 的错误绝大多数是**静默**的：引用一个不存在的属性不会报错，只会得到
`undefined` —— 颜色变透明、数值变 0、界面看起来"差不多对"。光看代码或者
光看截图都发现不了。

这套检查实际抓到过三个 bug，都不是靠读代码看出来的：

1. **`Glyph` 线宽只有 1px** —— `Math.round(16/12)` = 1，破坏性脚本的红色
   感叹号渲染出来只有 9 个亮像素，在近黑背景上完全读不出来，警示作用为零。
   靠逐像素统计发现（`x range [23]`，1 列）。
2. **`ListView.currentIndex` 默认是 0** —— 启动时第一个脚本显示成选中态，
   但 `scriptSelected` 信号并没有发出，右侧面板还停在"从左侧选一个脚本"。
   左右状态不一致。
3. **`Theme.fog` / `Theme.fogHover` 重命名后残留引用** —— 改配色时漏改，
   面板和行的背景会静默变透明。`themecheck.py` 就是为这类问题写的。

## 各脚本做什么

| 脚本 | 检查什么 |
| --- | --- |
| `qmlcheck.py` | 逐个 QML **解析 + 实例化**。只解析不够：不存在的属性引用能通过解析，只在 `create()` 时才暴露为绑定错误。 |
| `themecheck.py` | 实例化 `Theme` 单例，枚举其属性，与全部 QML 里出现的 `Theme.X` 求差集。顺带列出定义了但没用到的 token。 |
| `behaviour.py` | 驱动 `ScriptOutliner`：调 `toggleGroup()`（大类 = group 首段、子分类 = 完整 group 串两级 key）后读回 `ListView.contentHeight` 与每个 section delegate 的分叉状态（大类头/子分类头、折叠徽标数值），断言折叠后行真的不占位、两级门互不串扰。 |
| `smoke_main.py` | 用 `QQmlApplicationEngine` 加载**真实 main.qml**（与 main.cpp 同一加载方式），等绑定跑完断言零 warning；再连 Canvas 的 `paint` 信号数重绘：创建时恰好 1 次、连改 10 次窗口宽度仍是 1 次 —— 这是"背景 Canvas 不随 resize 重画"优化的可断言证据（`docs/pitfalls/2026-09-06-canvas-resize-repaint.md`）。 |
| `mockmodel.py` | `ScriptListModel` / `ScriptRunner` 的 Python 替身，各脚本共用。 |

## 三个容易踩的坑

写这套东西时踩过，改的时候注意：

**1. 必须用 `QQuickView`，不能用裸 `QQmlComponent`。**
没有窗口就不会跑 layout/polish pass，`ListView.contentHeight` 会一直等于
视图高度。折叠前后都读到 600，断言"折叠后变矮"就在一个无意义的数字上
通过了。加了窗口才报真实几何（530 全展开 / 138 全折叠）。

**1b. 加载真实 main.qml 必须 `QQmlApplicationEngine`，不能 `QQuickView`。**
main.qml 的根是 `Window`，`QQuickView.setSource` 要求 Item 根，给 Window 根
直接报 "invalid root object"。smoke_main.py 因此走 QQmlApplicationEngine
（与 main.cpp 相同），并自己 `show()` 根窗口。另外 PySide2 会把这个 QML
Window 根包装成 `QWindow`（没有 contentItem/childItems），在树里找组件要
用 `findChildren(QObject)` + `metaObject().className()` 子串匹配。

**2. `setContextProperty` 不接管所有权。**
`setContextProperty("scriptModel", MockModel())` 写成临时对象，Python 立刻
回收它，QML 侧拿到空模型（`count == 0`），而且不报错。必须用变量持有。

**3. mock 的方法要加 `@Slot`，属性要用 `Property`。**
QObject 子类上的普通 Python 方法对 QML **不可见**。少了 `@Slot`，
`model.groupCount` 求值为 `undefined`，QML 走到 `: ""` 兜底分支，分组计数
角标静默渲染成空 —— 看起来像产品 bug，实际是 mock 的问题。
`@Slot` 才等价于真实 C++ 里的 `Q_INVOKABLE`，`Property` 才等价于
`Q_PROPERTY`。

## 沙箱/无 GL 环境

- `QT_QPA_PLATFORM=offscreen` 无窗口系统也能跑。
- `QT_QUICK_BACKEND=software` 无 GL 也能跑，但 **`ShaderEffectSource` 和
  `FastBlur` 没有软件实现**，所以 `main.qml`（含毛玻璃）在软件后端下会挂住。
  `behaviour.py` 因此直接实例化 `ScriptOutliner`，不走 `main.qml`。
  `smoke_main.py` 相反：它必须走 main.qml，在 Windows 上实测 offscreen +
  **默认后端**（PySide2/Qt 5.15.2，2026-09-06）能完整加载且零 warning，
  所以不设软件后端；脚本里预留了只豁免 QtGraphicalEffects 加载类 warning
  的口子，其余 warning 一律算失败。
- `offscreen` 平台自建字体库，只会去 `<PySide2>/lib/fonts` 找字体 —— PySide2
  的 wheel 不带这个目录，于是每次查字体都发一条 "Cannot find font directory"
  的 QtWarning，把真正的绑定错误淹没（qmlcheck 会当成失败）。四个脚本在
  Windows 上默认把 `QT_QPA_FONTDIR` 指到 `C:/Windows/Fonts` 规避；其它平台
  走系统字体库，没这个问题。
- 截图：`QQuickWindow.grabWindow()` 在 offscreen 下返回空图，要用 QML 侧的
  `grabToImage()`（异步，回调是唯一可靠的完成信号）。
