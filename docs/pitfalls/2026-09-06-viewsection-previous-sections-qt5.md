# 2026-09-06 Qt 5.15 的 section delegate 拿不到相邻 section 信息

## 现象

按 Qt 6 文档用 `ViewSection.previousSections` / `nextSections`，在 Qt 5.15.2
下静默失效：取值一律 `undefined`，不报错不告警——所有 section 被当成大类头，
或整个树塌成一层。

## 根因（实测：PySide2 5.15.2.1 = Qt 5.15.2 运行时）

这两个是 **Qt 6 的 API**：attached metaobject 枚举只有
`view / isCurrentItem / delayRemove / section / previousSection / nextSection`，
没有复数形式；`ViewSection.currentSection` 同样 undefined（5.15 的 ViewSection
只是 criteria 枚举命名空间）。section delegate 只拿得到当前 section 字符串；
attached 的 `previousSection/nextSection` 只在 **item delegate** 上有值。
另：`Item.childItems` 在 QML JS 里是 `undefined`，遍历子项要用 `children`。

## 解决

C++ 零改动：QML 侧 root 放零尺寸 `Repeater { model: root.model }` 探针读
group role，行序去重推出 `sectionTree`（大类头/子分类分叉、折叠徽标总数都从
它查）；反应性靠显式读 `groupProbe.count` 与 `root.model.count`。

## 验证

探针脚本确认 `previousSections` undefined 及 attached 属性枚举；
`ui/qt5/tools/qmltest/behaviour.py` 断言 5 个 section delegate 的分叉
（标题、徽标数值、高度）与全部折叠矩阵 contentHeight。
