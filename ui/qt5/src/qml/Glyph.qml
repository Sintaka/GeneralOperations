// 图标：全部用 Rectangle 拼，不引入任何图标资源或新模块。
//
// 【为什么不用别的方案】
//   - QtSvg / QtQuick.Shapes：都是独立模块，要多带 dll 和 qml 插件。
//     依赖预算是"只允许 Qt5::Quick"（见 CMakeLists 的说明），这条最硬。
//   - 字体字形（"▶" "⋯" 之类）：字形有没有、长什么样、基线在哪，
//     全看用户装了什么字体。图标位置飘忽是最难查的一类 UI bug。
//   - Canvas：每个图标都会变成一张自己的纹理，还要走 JS 绘制。
//     列表里几十个图标同时存在时不划算（Canvas 适合画一次的大块背景，
//     见 Backdrop.qml）。
//
// Rectangle 是 QtQuick 内建、GPU 直接画、无采样、任意缩放都清晰。
// 代价是只能画直线构成的形状，但下面这几个恰好都是。
//
// 用法：Glyph { kind: Glyph.Chevron; color: "white" }

import QtQuick 2.15

Item {
    id: root

    enum Kind {
        Chevron,   ///< 折叠箭头（指右，展开时由调用方 rotate 90°）
        Folder,    ///< 分组
        Ellipsis,  ///< 更多操作
        Plus,      ///< 新增
        Bang,      ///< 破坏性警告
        Minimize,  ///< 窗口最小化
        Close      ///< 窗口关闭
    }

    property int kind: Glyph.Kind.Chevron
    property color color: "white"

    /// dsh 的图标槽是 16x16，默认对齐它
    implicitWidth: 16
    implicitHeight: 16

    // 线宽随尺寸缩放但取整，避免半像素导致的发虚。
    //
    // 【为什么是 /8 而不是 /12】最初写的是 /12，在默认 16px 尺寸下
    // round(16/12) = 1，画出来只有 1px 宽 —— 实测渲染后整个感叹号
    // 只有 9 个亮像素，在近黑背景上完全读不出是什么，破坏性脚本的
    // 警示作用等于没有。/8 在 16px 下得到 2px，是这个尺寸下能同时
    // 保证"看得清"和"不糊"的最小值。
    readonly property real stroke: Math.max(2, Math.round(width / 8))

    // ---- 折叠箭头 ----
    // 两条短横旋转 ±45° 拼成 ">"。
    // 交点放在中心，两条各占一半长度。
    Item {
        anchors.fill: parent
        visible: root.kind === Glyph.Kind.Chevron

        // 视觉上 ">" 的高度约等于宽度的 1.6 倍才不显得扁，
        // 所以每条臂长取宽度的 0.42，两条合起来跨度 0.6*height。
        readonly property real arm: root.width * 0.42

        Rectangle {
            width: parent.arm
            height: root.stroke
            radius: root.stroke / 2
            color: root.color
            antialiasing: true
            // 上臂：从中心往左上。旋转中心设在右端，这样两条共用一个端点。
            x: root.width / 2 - width
            y: root.height / 2 - height / 2
            transformOrigin: Item.Right
            rotation: -45
        }
        Rectangle {
            width: parent.arm
            height: root.stroke
            radius: root.stroke / 2
            color: root.color
            antialiasing: true
            x: root.width / 2 - width
            y: root.height / 2 - height / 2
            transformOrigin: Item.Right
            rotation: 45
        }
    }

    // ---- 文件夹 ----
    // 一个小凸起（标签）+ 一个主体。
    Item {
        anchors.fill: parent
        visible: root.kind === Glyph.Kind.Folder

        // 标签：左上角一小块，宽度约 40%
        Rectangle {
            x: root.width * 0.06
            y: root.height * 0.20
            width: root.width * 0.40
            height: root.height * 0.14
            radius: root.stroke
            color: root.color
            antialiasing: true
        }
        // 主体
        Rectangle {
            x: root.width * 0.06
            y: root.height * 0.30
            width: root.width * 0.88
            height: root.height * 0.48
            radius: root.stroke * 1.5
            color: root.color
            antialiasing: true
        }
    }

    // ---- 更多（三点横排）----
    Row {
        anchors.centerIn: parent
        visible: root.kind === Glyph.Kind.Ellipsis
        spacing: root.stroke * 1.6

        Repeater {
            model: 3
            Rectangle {
                width: root.stroke * 1.5
                height: root.stroke * 1.5
                radius: width / 2
                color: root.color
                antialiasing: true
            }
        }
    }

    // ---- 加号 ----
    Item {
        anchors.fill: parent
        visible: root.kind === Glyph.Kind.Plus

        Rectangle {
            anchors.centerIn: parent
            width: root.width * 0.62
            height: root.stroke
            radius: root.stroke / 2
            color: root.color
            antialiasing: true
        }
        Rectangle {
            anchors.centerIn: parent
            width: root.stroke
            height: root.height * 0.62
            radius: root.stroke / 2
            color: root.color
            antialiasing: true
        }
    }

    // ---- 感叹号（破坏性标记）----
    Item {
        anchors.fill: parent
        visible: root.kind === Glyph.Kind.Bang

        // 竖杠
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.height * 0.18
            width: root.stroke
            height: root.height * 0.44
            radius: root.stroke / 2
            color: root.color
            antialiasing: true
        }
        // 点
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.height * 0.70
            width: root.stroke
            height: root.stroke
            radius: width / 2
            color: root.color
            antialiasing: true
        }
    }

    // ---- 最小化 ----
    // 鹰角 index.d41038.js 的 vf 组件：14x14 视框里一条 y=6.8 的横线，
    // stroke-width 2。stroke 在 width=14 时正好取整到 2，几何吻合。
    // 原线端点是平头（<line> 无 linecap），所以不设 radius。
    Item {
        anchors.fill: parent
        visible: root.kind === Glyph.Kind.Minimize

        Rectangle {
            anchors.centerIn: parent
            width: root.width
            height: root.stroke
            color: root.color
            antialiasing: true
        }
    }

    // ---- 关闭 ----
    // 鹰角的 of 组件，24x24 视框。不是一条普通叉线，而是"中段断开的 X"：
    // 四段 5x2 的 45° 斜杠围出中心空档，空档正中还有一颗 1x1 的小菱形。
    // 这是它关闭按钮的辨识特征，必须照抄，不能"顺手"画成实心叉。
    //
    // 各段中心点由原 SVG 路径的四个平行四边形顶点取平均得出：
    //   M13.5562 9.31353 L17.0917 5.778 L18.5059 7.19221 L14.9704 10.7277 Z
    //   M5.77746 17.0912 L9.31299 13.5556 L10.7272 14.9698 L7.19167 18.5054 Z
    //   M14.687 13.5556 L18.2225 17.0912 L16.8083 18.5054 L13.2728 14.9698 Z
    //   M6.90883 5.77745 L10.4444 9.31298 L9.03015 10.7272 L5.49462 7.19166 Z
    // 全部坐标按 width/24 等比缩放，任意尺寸不变形。
    Item {
        id: closeIcon
        anchors.fill: parent
        visible: root.kind === Glyph.Kind.Close

        readonly property real u: root.width / 24

        Repeater {
            model: [
                { cx: 16.031, cy:  8.253, rot: -45 },
                { cx:  8.252, cy: 16.031, rot: -45 },
                { cx: 15.748, cy: 16.031, rot:  45 },
                { cx:  7.969, cy:  8.252, rot:  45 }
            ]
            Rectangle {
                width: 5 * closeIcon.u
                height: 2 * closeIcon.u
                color: root.color
                antialiasing: true
                x: modelData.cx * closeIcon.u - width / 2
                y: modelData.cy * closeIcon.u - height / 2
                rotation: modelData.rot
            }
        }

        // 中心菱形：原 SVG 是 1x1 方块 rotate(45) 后落在 (12,12)。
        Rectangle {
            width: closeIcon.u
            height: closeIcon.u
            anchors.centerIn: parent
            color: root.color
            antialiasing: true
            rotation: 45
        }
    }
}
