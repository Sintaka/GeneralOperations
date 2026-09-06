# 2026-09-06 Canvas 随 resize 每帧主线程重绘（live resize 卡顿）

## 现象

拖拽缩放窗口（无边框窗口，`WindowResizer.qml` 走 `startSystemResize`，
系统级 resize 循环本身没有问题）时卡顿肉眼可见；同窗口的移动拖拽不卡。

## 根因（叠加的两层，都不直观）

1. **主因：Backdrop 的 Canvas 用"尺寸变了才重画"的策略，恰恰在 live resize
   时每帧全量重画。** `onWidthChanged/onHeightChanged: requestPaint()` 的注释
   写着"背景是静态的，只有尺寸变了才需要重画"——单看一次 resize 是对的，但
   live resize 是**每帧都在变尺寸**：系统 resize 循环的每一帧都触发一次
   `requestPaint()`，Canvas.Cooperative 又把绘制排在主线程，等于每帧在主线程
   跑一遍全窗 JS 径向渐变重绘（`Theme.blobs` 四个 ellipse + gradient 构造）。
   "静态背景"的优化假设在连续 resize 下反转成了最贵的路径。
2. 次因：GlassPanel 的 `slice`（ShaderEffectSource）`sourceRect` 绑定
   面板几何，resize 期间每帧变化 → 每帧全分辨率重取样 + FastBlur(radius 20)
   全分辨率重跑（`cached: true` 的结果也随源失效）+ shell `layer.enabled`
   遮罩 FBO 重分配；整窗 `backdropSource` 的纹理尺寸 = item 尺寸，也在逐帧
   重分配 FBO。Canvas 的重绘拖慢主线程，这些 GPU 侧成本叠上去就是肉眼卡顿。

## 解决

- **Backdrop Canvas 固定逻辑尺寸 1600×900，只画一次**；外面包一层 cover 式
  等比缩放（`scale: Math.max(容器宽/1600, 容器高/900)`，中心对齐）。resize
  期间只有 scale 这一个数值在变，纹理一个字节不重画。代价：r 由窗口对角线
  改为固定画布对角线，窗口比例偏离 16:9 越多构图偏差越大——光斑 alpha 全部
  < 0.10 且是软渐变，微差不可感知（Backdrop.qml 文件头有完整取舍说明）。
- **GlassPanel.slice 设 `textureSize` 半分辨率**（模糊输入像素量降 4 倍，
  20px 模糊后无视觉差），并量化到 4px 步进——否则半分辨率仍会随面板尺寸
  逐帧重分配 FBO；量化后只有跨过 8 逻辑像素边界才重分配一次。
- **main.qml 的 `backdropSource` 同样半分辨率 + 8px 步进量化**，把整窗 FBO
  的逐帧重分配也变成偶发。
- 最小化/还原的 Windows 风格动画是同轮的另一项改动，与此坑无关，见
  main.qml「最小化 / 还原动画」注释。

## 验证

`ui/qt5/tools/qmltest/smoke_main.py`（本轮新增）：QQmlApplicationEngine
加载真实 main.qml，连 Canvas 的 `paint` 信号计数——

- 修复前（探针实测）：加载 + 10 次改宽 = **12 次 onPaint**（创建 1~2 次 +
  每次 resize 约 1 次，正是"每帧重画"的形状）；
- 修复后：创建时恰好 **1** 次，10 次改宽后仍是 **1** 次（脚本断言
  "10 次改宽 → onPaint 累计 1"通过，退出码 0）。

`qmlcheck.py` / `themecheck.py` / `behaviour.py` 全绿，`cmake --build
--preset qt5-mingw-debug` 零错误。

## 附：offscreen 冒烟顺带踩到的两个环境坑

- `QQuickView.setSource` 要求 Item 根；main.qml 根是 `Window`，直接报
  "invalid root object"。加载真实 main.qml 必须用 `QQmlApplicationEngine`
  （main.cpp 就是这么做的），再对根窗口自己 `show()`。
- PySide2 把 QML 的 Window 根对象包装成 `QWindow`（没有 `contentItem` /
  `childItems`），树内找组件用 `findChildren(QObject)` +
  `metaObject().className()` 子串匹配（QML 类型报 `Backdrop_QMLTYPE_13` 这类名字）。
