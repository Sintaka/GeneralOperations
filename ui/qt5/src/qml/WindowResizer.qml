// 无边框窗口的缩放热区。
//
// FramelessWindowHint 之后系统缩放边框没了，窗口只能看不能拉 ——
// 这里用 8 条贴边的 MouseArea 补回来。
//
// 【为什么交给系统而不是手写】QQuickWindow::startSystemResize（Qt 5.15
// 起，正是我们的基线）进入系统 resize 循环后，光标反馈、最小尺寸钳制、
// 拖动中的坐标换算全部由 Windows 处理；手写 onPositionChanged 改
// width/height 则每一项都得自己做，还容易和高 DPI、吸附手势打架。
//
// 【层级约定】本组件要声明成窗口根 Item 的最后一个子节点（盖在面板
// 之上）：边缘 6px 以内归缩放，其余才轮得到面板内容。窗口控制按钮
// 离边缘 28px（见 WindowControls.floatOffset），不会被热区挡住。
// 角上的热区故意压在直边热区之上：鼠标想抓的肯定是角不是边。

import QtQuick 2.15
import QtQuick.Window 2.15

Item {
    id: root

    /// 要缩放的窗口。
    property Window target: null

    /// 热区厚度。鹰角原包没暴露这个值（缩放逻辑在原生壳里），取桌面
    /// 无边框应用的常见手感 6px。
    readonly property int thickness: 6

    // ---- 四条直边 ----
    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.thickness
        cursorShape: Qt.SizeHorCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.thickness
        cursorShape: Qt.SizeHorCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.thickness
        cursorShape: Qt.SizeVerCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.thickness
        cursorShape: Qt.SizeVerCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.BottomEdge)
    }

    // ---- 四个角（声明在后，盖在直边热区上）----
    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        width: root.thickness
        height: root.thickness
        cursorShape: Qt.SizeFDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        width: root.thickness
        height: root.thickness
        cursorShape: Qt.SizeBDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.RightEdge | Qt.TopEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: root.thickness
        height: root.thickness
        cursorShape: Qt.SizeBDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.thickness
        height: root.thickness
        cursorShape: Qt.SizeFDiagCursor
        onPressed: if (root.target) root.target.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
    }
}
