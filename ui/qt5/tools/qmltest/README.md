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
```

三个脚本都以退出码表示结果（0 通过），可直接进 CI。

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
| `behaviour.py` | 驱动 `ScriptOutliner`：调 `toggleGroup()` 再读回 `ListView.contentHeight`，断言折叠后行真的不占位。 |
| `mockmodel.py` | `ScriptListModel` 的 Python 替身，三个脚本共用。 |

## 三个容易踩的坑

写这套东西时踩过，改的时候注意：

**1. 必须用 `QQuickView`，不能用裸 `QQmlComponent`。**
没有窗口就不会跑 layout/polish pass，`ListView.contentHeight` 会一直等于
视图高度。折叠前后都读到 600，断言"折叠后变矮"就在一个无意义的数字上
通过了。加了窗口才报真实几何（410 展开 / 138 折叠）。

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
- 截图：`QQuickWindow.grabWindow()` 在 offscreen 下返回空图，要用 QML 侧的
  `grabToImage()`（异步，回调是唯一可靠的完成信号）。
