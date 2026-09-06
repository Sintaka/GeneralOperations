# 2026-09-06 Canvas 随 resize 每帧主线程重绘（live resize 卡顿）

## 现象

拖拽缩放无边框窗口（`WindowResizer.qml` 走 `startSystemResize`）卡顿肉眼
可见；同窗口移动拖拽不卡。

## 根因（叠加两层）

主因：Backdrop 的 Canvas "尺寸变了才重画"——live resize 每帧都在变尺寸，
每帧触发 `requestPaint()`，Cooperative 模式把全窗 JS 径向渐变重绘排在主线程
（"静态背景"的优化假设在连续 resize 下反转为最贵路径）。次因：GlassPanel
的 `slice` sourceRect 绑定面板几何，每帧全分辨率重取样 + FastBlur(20) +
FBO 重分配；main.qml 的 `backdropSource` 纹理也逐帧重分配 FBO。

## 解决

Backdrop Canvas 固定逻辑尺寸 1600×900 只画一次，外包 cover 式等比缩放
（resize 期间只有 scale 在变，构图偏差因光斑 alpha < 0.10 不可感知，取舍见
Backdrop.qml 文件头）；`slice` 与 `backdropSource` 设半分辨率 `textureSize`
并量化到 4px/8px 步进，把逐帧 FBO 重分配变成偶发。

## 验证

`ui/qt5/tools/qmltest/smoke_main.py` 计数 Canvas `paint`：修复前加载 +
10 次改宽 = 12 次 onPaint；修复后恒为 1 次（断言通过）。qmlcheck /
themecheck / behaviour 全绿。附：加载 Window 根的 main.qml 须用
QQmlApplicationEngine（QQuickView 要求 Item 根）；PySide2 把 Window 根包成
QWindow，树内找组件用 findChildren + className 子串匹配。
