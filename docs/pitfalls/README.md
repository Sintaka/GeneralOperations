# 踩坑记录

解决"根因不直观、靠实测定位"级别的问题后，在本目录写一个最简 md
（`YYYY-MM-DD-主题.md` 命名），只记 现象 / 根因 / 解决 / 验证 四段，
并在下面的索引里加一行 —— 方便以后撞同类坑时检索。

## 索引

- [2026-09-06 DOpus 拖入 URL 跨 QML→C++ 桥丢失 + OLE 延迟渲染](2026-09-06-dragdrop-urls-empty.md)
- [2026-09-06 Qt 5.15 的 section delegate 拿不到相邻 section 信息（ViewSection.previousSections 是 Qt 6 的）](2026-09-06-viewsection-previous-sections-qt5.md)
- [2026-09-06 Canvas 随 resize 每帧主线程重绘（live resize 卡顿）](2026-09-06-canvas-resize-repaint.md)
- [2026-09-06 Column 位置器跳过不可见子项的布局，但跳不过它的 height 属性值](2026-09-06-positioner-invisible-child-height.md)
- [2026-09-07 Qt 5.15 ListView 对 section delegate 自身的高度变化不重排](2026-09-07-listview-section-relayout-qt5.md)
