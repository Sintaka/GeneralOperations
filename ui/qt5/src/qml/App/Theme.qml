// 视觉常量集中在这里。
//
// 放在自己的模块目录 src/qml/App/ 下，由同目录的 qmldir 声明为
// `singleton Theme 1.0 Theme.qml`，其它 QML 文件写 `import App 1.0` 使用。
//
// 【为什么必须单独一个目录】QML 会把同目录下所有 .qml 隐式注册成类型。
// 如果 Theme.qml 和 main.qml 放一起，`Theme` 就同时是"目录里的局部组件类型"
// 和"App 模块的单例"，两者撞名；局部类型优先，于是 Theme.xxx 取到的是
// 组件类型而不是单例实例，所有值都是 undefined。
// 单独成目录后 src/qml/ 里不再有 Theme.qml，撞名消失。
//
// ============================================================================
// 【设计来源】两个真实产品，各取一半，都是从它们的实际样式表里量出来的值，
// 不是凭感觉调的。
//
// 配色与表面 → 鹰角启动器 (Hypergryph Launcher 1.5.0, res/web/index.css)
//   近黑 #18171a 配不同透明度、0.5px 白色发丝描边、分层阴影、4~6px 克制模糊。
//
// 行几何与交互 → dsh (deepseek-harness, packages/client/ui-workspace)
//   分组行 34px / 条目行 32px、8px 圆角、悬停与选中共用同一个填充色、
//   行尾操作图标只在悬停时出现、150ms 过渡。
//
// 两边都遵守一条共同原则：**层次靠描边和阴影，不靠颜色饱和度**。
// 所以下面没有任何高饱和色，除了唯一的强调色。
// ============================================================================
pragma Singleton

import QtQuick 2.15

QtObject {
    // ========================================================================
    // 背景
    // ========================================================================
    // 鹰角真实背景是游戏主视觉大图 + 一层几乎全透明的噪点纹理
    // (pattern3.png，405x100，alpha 基本在 0~4 之间)。
    // 我们没有主视觉图，所以用近黑做底 + 极低饱和的冷色调偏移，
    // 让它有一点纵深而不至于死板。刻意不用高饱和渐变 ——
    // 那是上一版的做法，与鹰角的克制观感冲突。
    readonly property color bgTop:    "#1d1c21"
    readonly property color bgBottom: "#141317"

    // 背景上的大块柔光。鹰角靠主视觉图提供"模糊得出来的细节"，
    // 我们没有图，所以留几团极低透明度的光斑代替 ——
    // 纯平底色模糊前后完全一样，毛玻璃就白做了。
    //
    // 注意 alpha 全部压到 0.10 以下（上一版是 0.43~0.75）。
    // 这是"近黑配色"和"紫蓝渐变"的分界线：光斑只负责给模糊提供梯度，
    // 不负责提供颜色观感。
    readonly property var blobs: [
        { cx: 0.20, cy: 0.10, r: 0.46, color: "#6b7280", alpha: 0.085 },
        { cx: 0.88, cy: 0.28, r: 0.38, color: "#8b8f99", alpha: 0.060 },
        { cx: 0.60, cy: 0.92, r: 0.50, color: "#5b6472", alpha: 0.070 },
        { cx: 0.04, cy: 0.80, r: 0.32, color: "#7a7f8a", alpha: 0.045 }
    ]

    // 鹰角还在主视觉图上叠了一层噪点纹理 (pattern3.png)，我们没照做，
    // 原因见 Backdrop.qml 顶部说明（20px 模糊会直接抹掉 1.6% 的颗粒，
    // 成本与收益不成比例）。所以这里没有 grain 相关的 token。

    // ========================================================================
    // 表面（面板 / 弹层）
    // ========================================================================
    // 鹰角的原值，直接照搬：
    //   #18171af2 = 近黑 95% —— 主要面板
    //   #18171ae6 = 近黑 90% —— 弹层 (.pop-content)
    //   #18171ac7 = 近黑 78% —— 渐变遮罩上沿 (.switch-bar)
    readonly property color surface:      Qt.rgba(0x18 / 255, 0x17 / 255, 0x1a / 255, 0.95)
    readonly property color surfacePop:   Qt.rgba(0x18 / 255, 0x17 / 255, 0x1a / 255, 0.90)
    readonly property color surfaceVeil:  Qt.rgba(0x18 / 255, 0x17 / 255, 0x1a / 255, 0.78)

    // 描边：鹰角写的是 .5px solid #ffffff1a。
    // Qt 的 border.width 是 int，给不了 0.5px，所以用 1px 配更低的 alpha
    // 来还原"发丝感"—— 1px @ 0.10 视觉上比 0.5px @ 0.10 重一倍，
    // 因此 alpha 折半到 0.08 附近。
    readonly property color stroke:      Qt.rgba(1, 1, 1, 0.085)
    readonly property color strokeHover: Qt.rgba(1, 1, 1, 0.20)

    // 悬停/选中填充。
    // 鹰角是 linear-gradient(0deg,#ffffff26,#ffffff26) 叠在底色上，
    // 即纯白 15% 的平铺叠加 —— 等价于一层 rgba(1,1,1,0.15)。
    // dsh 那边是 rgba(255,255,255,0.08)。
    // 两者取中并分成两级：行悬停用轻的，面板/选中用重的。
    readonly property color fillHover:    Qt.rgba(1, 1, 1, 0.065)   ///< dsh 行悬停
    readonly property color fillSelected: Qt.rgba(1, 1, 1, 0.095)   ///< dsh 选中（与悬停同色系，更亮一档）
    readonly property color fillStrong:   Qt.rgba(1, 1, 1, 0.150)   ///< 鹰角 #ffffff26，按钮悬停

    // 阴影。鹰角两档，原值照搬：
    //   0 2px 6px #0000008c  (55%) —— 圆形图标按钮
    //   0 2px 8px #000000a6  (65%) —— 弹层、胶囊
    readonly property color shadowColor:    Qt.rgba(0, 0, 0, 0.55)
    readonly property color shadowColorPop: Qt.rgba(0, 0, 0, 0.65)
    readonly property int   shadowOffsetY:  2
    readonly property int   shadowBlur:     6
    readonly property int   shadowBlurPop:  8

    // ========================================================================
    // 强调色
    // ========================================================================
    // 鹰角 :root 里的 --color-primary 系列，明黄。用户明确要求照搬。
    readonly property color accent:         "#fdfc00"   ///< rgb(253 252 0)
    readonly property color accentHover:    "#fefd4d"   ///< rgb(254 253 77)
    readonly property color accentPressed:  "#e4e300"   ///< rgb(228 227 0)
    readonly property color accentDisabled: "#a0a0a0"   ///< rgb(160 160 160)
    /// 明黄底上的文字必须是黑的（--color-text-in-primary: 0 0 0）
    readonly property color textOnAccent:   "#000000"

    // 危险/警告。鹰角红点用的 #f96f68。
    // 破坏性脚本标记复用它 —— 比上一版自造的 #ff9d5c 更贴原版。
    readonly property color danger: "#f96f68"

    // ========================================================================
    // 文字
    // ========================================================================
    // 鹰角正文就是纯白 #fff，次级信息靠透明度压，不靠另调一个灰。
    // dsh 那边是四级 (primary/secondary/tertiary/caption)，
    // 这里取三级够用。
    readonly property color textPrimary:   "#ffffff"
    readonly property color textSecondary: Qt.rgba(1, 1, 1, 0.62)
    readonly property color textTertiary:  Qt.rgba(1, 1, 1, 0.38)

    // 字号。鹰角全站 12/13px 为主，line-height 18px。
    // dsh 标题 14px/20px。列表标题取 13，说明文字取 12。
    readonly property int fontTitle:    14
    readonly property int fontBody:     13
    readonly property int fontCaption:  12
    readonly property int fontMicro:    11

    // 字族。鹰角用 Source Han Sans CN（思源黑体）。
    // 本机不一定装了，QML 的 font.family 给不存在的字族会静默回退，
    // 所以这里给一串候选，靠 Qt 的字体匹配自己挑。
    // 微软雅黑是 Windows 自带的，作为最后兜底。
    readonly property string fontFamily: "Source Han Sans CN, Noto Sans CJK SC, Microsoft YaHei UI, Microsoft YaHei, sans-serif"

    // ========================================================================
    // 几何
    // ========================================================================
    // 鹰角圆角用得很杂（4/8/10/12/100px），主力是 8px；
    // dsh 的行圆角也正好是 8px。统一到 8。
    readonly property real radius:      8    ///< 面板、行
    readonly property real radiusSmall: 4    ///< 小图标按钮（鹰角 .iconButton 同值）
    readonly property real radiusPill:  100  ///< 胶囊（鹰角 .switch-tabs / .header-icon_login）

    // ========================================================================
    // 模糊
    // ========================================================================
    // 【这里是与上一版差别最大的地方，也最容易被改错，所以说清楚】
    //
    // 上一版 blurRadius = 64（FastBlur 的饱和上限）。那是配紫蓝高饱和渐变的，
    // 面板底色也只有白雾 10%，所以需要很重的模糊才出玻璃感。
    //
    // 鹰角的实际值是 backdrop-filter: blur(4px)~blur(6px) —— 非常克制。
    // 它敢这么轻，是因为面板底色本身就是近黑 95% 不透明：
    // 表面几乎不透光，模糊只是给边缘一点柔化，层次全靠描边和阴影撑。
    //
    // 但 CSS 的 blur(6px) 和 Qt FastBlur 的 radius 6 不是同一个量纲
    // （FastBlur 内部 lod = sqrt(radius/64)*1.2-0.2，是个非线性映射）。
    // 直接写 6 在 Qt 下几乎看不见效果。经算式反推，要得到视觉上相当于
    // CSS 6px 的柔化，radius 需要在 20 附近。
    //
    // 结论：数值不同源但观感对齐，这是刻意的。改的时候别"修正"回 6。
    readonly property real blurRadius: 20

    // ========================================================================
    // 滚动条
    // ========================================================================
    // 鹰角原值：width 4px、thumb #ffffff40 (25%)、hover #ffffff80 (50%)、
    // radius 3px、track 透明。全部照搬。
    // （dsh 是 8px 宽、指针离开后淡出；这里按鹰角走，更细。）
    readonly property real  scrollWidth:      4
    readonly property real  scrollRadius:     3
    readonly property color scrollThumb:      Qt.rgba(1, 1, 1, 0.25)
    readonly property color scrollThumbHover: Qt.rgba(1, 1, 1, 0.50)

    // ========================================================================
    // 动画
    // ========================================================================
    // 鹰角：颜色 .15s ease-in-out、变换 .3s、指示器 .3s cubic-bezier(.4,0,.2,1)。
    // dsh：行淡入 150ms、箭头旋转 150ms、折叠 150ms。
    // 两边的"快"档正好都是 150，取 150。
    readonly property int durFast: 150   ///< 颜色、悬停、箭头旋转、行淡入
    readonly property int durSlow: 300   ///< 位移、整块入场

    // 鹰角的 cubic-bezier(.4,0,.2,1) 就是 Material 的 standard easing，
    // Qt 里没有完全等价的枚举；OutCubic 的收尾手感最接近。
    readonly property int easing: Easing.OutCubic

    // ========================================================================
    // Outliner 行几何（全部来自 dsh Rows.module.css）
    // ========================================================================
    // dsh 原注释：
    //   "Tree rows: project 34px, session 32px, radius 8, indent step 22px
    //    (16px slot + 6px gap)."
    readonly property int rowGroupHeight: 34   ///< 分组头
    readonly property int rowItemHeight:  32   ///< 脚本行
    readonly property int rowGap:         2    ///< 行间距 (.sessionRow margin-top)
    readonly property int groupGap:       4    ///< 组间距 (.groupSection + .groupSection)
    readonly property int rowPadding:     8    ///< 行左右内边距
    readonly property int iconSlot:       16   ///< 图标槽宽（缩进就靠它对齐）
    readonly property int iconGap:        6    ///< 图标与标题间距
    /// 行尾操作图标之间的间距 (.rowActions gap: 12px)
    readonly property int actionGap:      12

    // 列表底部的渐隐遮罩高度 (.fade height: 24px)。
    // 作用是让滚动到一半的行淡出，而不是被硬边裁断。
    readonly property int fadeHeight: 24
}
