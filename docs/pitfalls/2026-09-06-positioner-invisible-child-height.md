# Column 位置器跳过不可见子项的布局，但跳不过它的 height 属性值

## 现象

contentHeight 断言恒定多 28px（恰一个子分类头行高），且只在"有子分类头可见"
时出现；逐 delegate 的 height 锚点单独读又全"正确"。09-07 后续：收起大类后
子分类头残影探进大类头可视带，悬停还会高亮。

## 根因

位置器（如 Column）对 `visible: false` 的子项不参与布局，但子项属性值照旧：
聚合公式对 majorRow 判了 visible、对 subRow 漏了（其 height 绑定只依赖
`majorExpanded`，不可见仍算 28），delegate 白高 28px——只有 contentHeight
这个总量能暴露。后续问题的根因：subRow 高度归零但内容居中锚定（文本/箭头
y≈25..43），跨进 delegate 可视带 0..38；而 delegate 根节点上任何 `visible`
绑定都不可靠——`visible: height > 0` 是绑定环，无环写法也会被 ListView
命令式写 visible 杀掉绑定（实测 hasVisibleRow=false 而 visible 仍 true）。

## 解决

聚合公式每个子项都按 visible 门（`visible ? child.height : 0`），且子项
height 绑定自带与可见性相同的条件（`hasSub && majorExpanded ? 28 : 0`）；
残影问题最终防线是 **clip**：delegate 根与 subRow 都 `clip: true`（随行高
归零把居中内容裁掉；0 高度 MouseArea 天然不参与命中测试）。

## 验证

behaviour.py 折叠矩阵全绿：全展开 530 / 仅收子分类 326 / 全收大类 138
（修复前恒 +28）；新增 delegateClip / subClip 回归锚点。
