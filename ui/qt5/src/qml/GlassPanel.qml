// 毛玻璃面板 —— 鹰角启动器的表面语言。
//
// 四层，从后往前：
//   0. 投影（RectangularGlow，画在面板矩形之外）
//   1. 从共享背景里取"自己这块矩形"并模糊
//   2. 近黑半透明表面 (#18171a 95%)
//   3. 1px 发丝描边
//
// 【与上一版的区别 / 也是这一版的核心】
// 上一版是"白雾 10% + 64px 重模糊"，玻璃感全靠模糊。
// 鹰角的做法相反：表面近黑 95% 基本不透光，模糊只有 4~6px 很克制，
// **层次感靠描边和分层阴影撑，不靠模糊，也不靠颜色饱和度**。
// 所以这一版加了投影、把模糊降到 20（等效 CSS 6px，见 Theme.blurRadius
// 的说明）、白雾换成近黑。
//
// 【重要前提】本组件必须是窗口根 Item 的直接子节点，用 anchors/x/y 定位，
// 不要塞进嵌套 Layout。原因：sourceRect 需要"面板在背景坐标系里的位置"，
// 作为直接子节点时 x/y 就是窗口坐标，且是响应式绑定。
// 若改用嵌套 Layout，就得靠 mapToItem() 换算 —— 那是函数调用，只求值一次，
// 不参与绑定跟踪，窗口一缩放就错位。

import QtQuick 2.15
import QtGraphicalEffects 1.0
import App 1.0

Item {
    id: root

    /// 由 main.qml 传入的整窗背景 ShaderEffectSource
    property var backdropSource: null

    /// 内容直接写进 <GlassPanel> 标签体即可
    default property alias content: contentArea.data

    /// 弹层用更低的不透明度（鹰角 .pop-content 是 90%，主面板是 95%）
    property color surfaceColor: Theme.surface

    // ---- 层 0：投影 ----
    // 鹰角的 box-shadow: 0 2px 6px #0000008c。
    //
    // 用 RectangularGlow 而不是 DropShadow：DropShadow 是"把源 item 整个
    // 跑一遍高斯模糊"，而我们要投的只是一个圆角矩形，形状已知。
    // RectangularGlow 是这个特例的解析实现（不采样源纹理），明显更省。
    //
    // 必须画在被遮罩的层之外 —— layer.effect 的 OpacityMask 会把
    // 面板矩形外的一切裁掉，投影正好全在矩形外，放里面就什么都不剩了。
    RectangularGlow {
        anchors.fill: shell
        anchors.topMargin: Theme.shadowOffsetY
        glowRadius: Theme.shadowBlur
        cornerRadius: Theme.radius + Theme.shadowBlur
        color: Theme.shadowColor
        // spread 0 = 亮度从边缘就开始衰减，最接近 CSS box-shadow 无扩散的观感。
        spread: 0.0
    }

    // ---- 被遮罩的部分：模糊背景 + 表面 ----
    // 单独包一层 Item 是为了让 OpacityMask 只作用于这两层。
    // 内容（文字、图标）刻意留在遮罩外面 —— 走 OpacityMask 会让文字
    // 多经一次纹理采样，边缘发虚。上一版把内容也裹进去了，这是个改进。
    Item {
        id: shell
        anchors.fill: parent

        // 层 1：自己这块区域的模糊背景
        ShaderEffectSource {
            id: slice
            anchors.fill: parent
            sourceItem: root.backdropSource
            // 只取面板覆盖的那一块。root.x/y 是窗口坐标（见上方前提）。
            sourceRect: Qt.rect(root.x, root.y, root.width, root.height)
            // 背景是静态的，取样结果不需要每帧重算。
            live: false
            // 【半分辨率取样】live resize 期间 sourceRect 每帧都在变，slice
            // 每帧重取样、FastBlur(radius 20) 每帧全分辨率重跑。模糊天生
            // 抹掉高频，把输入像素量降到 1/4 在 20px 模糊后面完全看不出差别。
            // textureSize 同时把这张 FBO 锁在半分辨率：否则重取样本身还会
            // 随面板尺寸逐帧重分配 FBO。
            // 尺寸再量化到 4px 步进（/8 后取整再 *4 = 半宽对齐 4）：精确的
            // width/2 会在 resize 时每像素都变，FBO 就又回到逐帧重分配；
            // 量化后只有跨过 8 逻辑像素的边界才重分配一次。
            textureSize: Qt.size(
                Math.max(1, Math.round(root.width / 8) * 4),
                Math.max(1, Math.round(root.height / 8) * 4))
            visible: false
        }

        FastBlur {
            anchors.fill: parent
            source: slice
            radius: Theme.blurRadius
            // 结果缓存成纹理，上层状态变化时不重跑模糊。
            cached: true
        }

        // 层 2：近黑表面。
        // 不做 hover 提亮 —— 鹰角的面板本身是静态的，只有可点击的控件
        // （.header-icon / .grid-item / .label-item-container）才有 hover 态。
        // 面板整体跟着鼠标忽明忽暗会显得廉价，且会与内部行的 hover 打架。
        Rectangle {
            anchors.fill: parent
            color: root.surfaceColor
        }

        // 圆角裁切：对模糊+表面做一次遮罩，而不是每层各自 radius。
        // 否则两层的直角会从圆角描边外面透出来。
        layer.enabled: true
        layer.effect: OpacityMask {
            maskSource: Rectangle {
                width: shell.width
                height: shell.height
                radius: Theme.radius
                visible: false
            }
        }
    }

    // ---- 层 3：发丝描边 ----
    // 在遮罩外面画，这样描边自己是干净的 1px，不被遮罩重采样。
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: Theme.radius
        border.width: 1
        border.color: Theme.stroke
    }

    // ---- 内容 ----
    Item {
        id: contentArea
        anchors.fill: parent
    }
}
