# 2026-09-06 Qt 5.15 的 section delegate 拿不到相邻 section 信息

## 现象

ScriptOutliner 要做两级树（大类 = group 第一段、子分类 = 第二段），需要每个
section delegate 判断"前面是否已有同大类的 section"。按 Qt 6 文档的写法用
`ViewSection.previousSections` / `nextSections`，在 Qt 5.15.2 下**静默失效**：
取值一律 `undefined`，不报错、不告警 —— 所有 section 都被当成大类头，
或者（取决于兜底方向）整个树塌成一层。引用不存在的 attached property 和
其它 QML 错误一样是静默的（undefined），读代码看不出来。

## 根因（实测结论，PySide2 5.15.2.1 = Qt 5.15.2 运行时）

- `ViewSection.previousSections` / `nextSections` 是 **Qt 6 的 API**，
  Qt 5.15 的 attached 对象上根本没有：对 section delegate 的
  `QQuickListViewAttached`（继承 `QQuickItemViewAttached`）跑 metaobject
  枚举，属性只有 `view / isCurrentItem / delayRemove / section /
  previousSection / nextSection`，没有复数形式那两个。
- `ViewSection.currentSection` 在 5.15 也一样 undefined —— `ViewSection`
  在 5.15 只是 `section.criteria` 的枚举命名空间（`ViewSection.FullString`），
  不是 attached 类型。
- section delegate 拿到的只有**当前** section 字符串（context property）；
  attached 的 `ListView.section / previousSection / nextSection` 在
  section delegate 上恒为空串（previousSection/nextSection 只在
  **item delegate** 上有值）。
- 另踩一个 QML/JS 边界：`Item.childItems` 是 `QQmlListProperty`，从 QML JS
  读到的是 `undefined`（C++ 侧 `childItems()` 方法不等于 QML 属性）；
  遍历子项要用 `children`。

## 解决

不动 C++（ScriptListModel 零改动），在 QML 侧自己推导 section 结构：

- root 上放一个零尺寸不可见的 `Repeater { model: root.model }` 探针，
  每行只读 group role；行序去重即 section 序（ScriptRegistry 按相对路径
  字典序排序，同组连续、同大类连续）。
- 由行序推导 `sectionTree`（每个大类：有序 section 列表 + groupCount 求和
  的总数）。"该大类的第一个 section 承担大类头行；含 "/" 的 group 一律
  再渲染自己的子分类头行（第一个 section 的 delegate 给出两行）" 的分叉、
  大类折叠徽标总数都从这里查。
- 反应性：绑定里显式读 `groupProbe.count` 和 `root.model.count`（后者同
  errorFooter 的做法 —— reload 时行数可能不变，靠 load() 后的 countChanged
  触发重算）。实测 modelReset 后 countChanged 再入绑定求值时 Repeater 已
  重建完毕，`itemAt(i)` 拿到的是新数据。

## 验证

- 探针脚本逐项确认：`typeof ViewSection.previousSections === "undefined"`、
  attached metaobject 属性枚举、item delegate 上 `ListView.previousSection`
  有值而 section delegate 上为空。
- `ui/qt5/tools/qmltest/behaviour.py` 断言 5 个 section delegate 的分叉
  （大类/子分类、标题、折叠徽标数值、高度）与全部折叠矩阵的 contentHeight。
