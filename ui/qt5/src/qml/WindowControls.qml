// 悬浮窗口控制按钮 —— 抄鹰角启动器右上角那组圆形按钮。
//
// 原型是 index.ea704a.css 的 .header-container .header-icon：
//   40px 圆、底色 #18171af2、0.5px 白色发丝描边、
//   box-shadow 0 2px 6px #0000008c、hover 叠一层 #ffffff26。
// 这几个 token 全部已经在 Theme 里（surface / stroke / fillStrong /
// shadow* 当初就是从这组规则量出来的），这里只是用回按钮上。
//
// 鹰角的头部只有最小化和关闭（i18n 字典里只有 header.minimize /
// header.close，没有 maximize），所以这里也只放两个，顺序照原版：
// 关闭在最右。图标是 Glyph 的 Minimize / Close，几何逐值对照原 SVG
// （index.d41038.js 的 vf / of），不是目测画的。
//
// 【定位】原版容器是 fixed top-3 right-3 (12px) + p-4 内边距 (16px)，
// 按钮离窗口边缘 28px，按钮间距 gap-4 = 16px。这些数都在本文件里，
// main.qml 只引用 floatOffset，不重复写魔法数。
//
// 【最小化不是直接 showMinimized】无边框窗口的最小化要伴随 main.qml 里
// rootItem 的内容收起动画（Windows 观感），所以这里只发 minimizeRequested()
// 请求信号，由窗口决定真正 showMinimized 的时机；关闭没有动画前置，
// 仍然直接 target.close()。
//
// 【前提】窗口必须是无边框的（main.qml 的 FramelessWindowHint），
// 这组按钮顶替的就是系统标题栏；窗口拖拽在 main.qml 的背景
// MouseArea，四边缩放热区在 WindowResizer.qml。

import QtQuick 2.15
import QtGraphicalEffects 1.0
import QtQuick.Window 2.15
import App 1.0

Row {
    id: root

    /// 最小化请求。收起动画的裁决在 main.qml（播完动画才 showMinimized）。
    signal minimizeRequested()

    /// 关闭动作作用的目标窗口（最小化已改为信号，不再直接操作窗口）。
    property Window target: null

    /// 按钮离窗口上/右边缘的距离：top-3 (12) + p-4 (16)。
    readonly property int floatOffset: 12 + 16

    /// 鹰角 2.5rem。
    readonly property int buttonSize: 40

    spacing: 16   // 鹰角 gap-4

    // ---- 单个圆形按钮 ----
    // Qt 5.15 起支持 inline component，正好是我们的基线。
    component CircleButton: Item {
        id: circle

        signal activated()
        property int glyphKind: Glyph.Kind.Minimize
        /// 无障碍名（原版按钮有 title tooltip 文案，见 vC 的 title 参数）。
        property string label: ""

        width: root.buttonSize
        height: root.buttonSize

        // 投影。和 GlassPanel 同一套 RectangularGlow 方案：
        // DropShadow 要对整个源跑一遍高斯模糊，而这里投的只是个圆，
        // cornerRadius 拉过半边长后 glow 自然退化为圆形，解析实现更省。
        RectangularGlow {
            anchors.fill: disc
            anchors.topMargin: Theme.shadowOffsetY
            glowRadius: Theme.shadowBlur
            cornerRadius: root.buttonSize / 2 + Theme.shadowBlur
            color: Theme.shadowColor
            spread: 0.0
        }

        Rectangle {
            id: disc
            anchors.fill: parent
            radius: width / 2
            color: Theme.surface                    // #18171af2
            border.width: 1
            border.color: Theme.stroke              // 0.5px #ffffff1a 的发丝等价

            // hover 叠层：原 CSS 是 linear-gradient(0deg,#ffffff26,#ffffff26)
            // 平铺叠加，等价于一整层白 15% 的圆 → Theme.fillStrong。
            // 没有按下态 —— 原版 header-icon 就只定义了 :hover。
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.fillStrong
                opacity: area.containsMouse ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                }
            }

            Glyph {
                anchors.centerIn: parent
                // 图标原始视框：关闭 24x24，最小化 14x14。
                width: circle.glyphKind === Glyph.Kind.Close ? 24 : 14
                height: width
                kind: circle.glyphKind
                color: "white"                      // 原 SVG 的 stroke/fill 全是 white
            }
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor      // 原 CSS cursor: pointer
            onClicked: circle.activated()
        }

        Accessible.role: Accessible.Button
        Accessible.name: circle.label
    }

    CircleButton {
        glyphKind: Glyph.Kind.Minimize
        label: qsTr("最小化")
        onActivated: root.minimizeRequested()
    }

    CircleButton {
        glyphKind: Glyph.Kind.Close
        label: qsTr("关闭")
        onActivated: if (root.target) root.target.close()
    }
}
