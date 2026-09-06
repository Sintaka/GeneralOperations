# Column 位置器跳过不可见子项的布局，但跳不过它的 height 属性值

## 现象

ScriptOutliner 两级树首版： behaviour.py 的 contentHeight 断言恒定多出
28px（恰好一个 rowSubGroupHeight），且只在"有子分类头可见"的状态出现，
大类全部折叠时（子分类头全部高度归零）反而精确吻合。逐 delegate 的高度
锚点单独读又全都"正确"。

## 根因

`Column` 这类位置器对 `visible: false` 的子项**不参与布局**（不占位、
不重排兄弟项），但子项自身的属性值一切照旧 —— `height` 绑定该是多少还是
多少。聚合高度如果在父级直接读 `child.height` 而不按 `child.visible` 门
一下，就会把"看不见但没归零"的子项算进去。具体到本次：

- System 组没有子分类，`subRow.visible: false`，但 `subRow.height` 的
  绑定只依赖 `majorExpanded`，仍算出 28；
- delegate 总高公式写了 `majorRow.visible ? majorRow.height : 0 +
  subRow.height` —— 对 majorRow 判了 visible，对 subRow 漏了，
  System 的 delegate 白高 28px。

逐 delegate 读 height 的测试锚点没抓到它，因为锚点自己也按 visible 门过
（读出来是 0）；只有 ListView.contentHeight 这个总量暴露了差值。

## 解决

两条规则一起上（写哪条都够，两条都在更稳）：

1. 聚合公式对每个子项都按 visible 门：`visible ? child.height : 0`。
2. 位置器子项的 height 绑定自带与 visible 相同的条件
   （`hasSub && majorExpanded ? 28 : 0`），让属性值本身与可见性一致。

## 验证

behaviour.py 的折叠矩阵全绿：全展开 530 / 仅收子分类 326 / 全收大类
138，与模型推算逐一吻合（修复前恒 +28）。
