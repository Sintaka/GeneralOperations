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

## 后续（09-07）：收起大类后子分类头残影探进大类头可视带

修完 28px 又撞到同族问题的另一面：用户报告"收起 Image 后，底部还露出
半个 Edit 子分类头，悬停还会高亮"。几何探针实测（carrier delegate
Image/Edit，收起后高 38）：subRow 高度归零但内容是居中锚定的，子项
文本 y=25..43、箭头 y=26..42，跨进 delegate 可视带 0..38 —— 大类头
底部画出文字/箭头的上半截，且正好落在表头悬停热区里。

修的过程又实测出两条 QML 死路，最终防线是 **clip**：

1. `visible: height > 0`（height 绑定要读子行的 visible/height）→
   **绑定环**，QML 掐断绑定后 visible 卡死在 true，反而更糟。
2. 改成"重算行高"的无环写法（visible 只依赖几何常量）→ 依然无效：
   **ListView 会命令式写 delegate 根节点的 visible，绑定直接被杀**
   （探针实测 hasVisibleRow=false 而 visible 仍为 true）。delegate
   根节点上任何 visible 绑定都不可靠 —— 旧代码那行其实是摆设，
   一直真正兜底的是 clip。

最终结构：delegate 根 `clip: true`（高度归零时可视带是空矩形）+
subRow 自身 `clip: true`（carrier 收起后行高归零，居中内容随动画被
裁掉）。鼠标侧天然安全：随行高归零的 MouseArea（0 高度）不参与命中
测试。behaviour.py 新增 delegateClip / subClip 回归锚点。
