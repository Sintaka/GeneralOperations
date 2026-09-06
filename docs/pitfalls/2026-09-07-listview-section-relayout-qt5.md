# Qt 5.15 ListView 对 section delegate 自身的高度变化不重排

## 现象

ScriptOutliner 里先收起子分类（脚本行已全部归零、不再参与高度动画），再点
大类头折叠：下方其它大类的 section 头与脚本行永久停在旧 y，留出 28px 空洞
（contentHeight 同样不缩）。若折叠时脚本行还在动画（item 高度逐帧变化），
每帧的重排会把问题掩盖——所以只在"纯 section 高度路径"下暴露。

## 根因

Qt 5.15 的 ListView 只把 **item delegate** 的高度变化当作重排触发器；
section delegate 自身高度变化既不重排后续 delegate，也不重算 contentHeight。

## 解决

折叠后等高度动画结束（durFast + 40ms 的 Timer），把
`listView.section.criteria` 换成 `FirstCharacter` 再立即换回 `FullString`：
逼 ListView 作废全部 section 状态、按当前 delegate 高度重建布局。两次赋值
在同一个 JS 调用内完成，中间不产生帧，重建不可见。实测否决的死路：
`forceLayout()`（含 Timer 周期推）不重算 section 缓存；section delegate 的
`onHeightChanged` 里调 forceLayout 会从布局通知里重入布局 pass，delegate
被整批废掉（h=0、visible=false）；`section.property` 赋 `""` 再还原会破坏
本来正确的布局。

## 验证

behaviour.py 新增几何栈探针 `check_stack`（逐 delegate 断言 y/height 无重叠
无空洞、contentHeight 正确）接入全部折叠矩阵，并加"纯 section 路径"回归
序列（修复前 contentHeight 断言 468 实得 496，必失败）。四件套退出码全 0。
