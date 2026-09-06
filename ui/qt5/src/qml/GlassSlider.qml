// 滑块 —— Quick Controls 不在依赖预算里，手写三件套：轨道 / 填充 / 手柄。
// 填充用 Theme.accent（全站唯一的强调色，交互控件专用），轨道近透明的
// 白 —— 层次靠明度差，不靠描边，和面板的语言一致。
//
// 【value 不是绑定的】拖动时 imperative 赋值。调用方（ParamEditor）
// 不要把 value 绑到外部状态上 —— 那个绑定会在第一次拖动时被销毁；
// 初始化用 Component.onCompleted，变更走 onValueChanged 回写。

import QtQuick 2.15
import App 1.0

Item {
    id: root

    property real from: 0
    property real to: 100
    property real value: from
    /// int 参数开启：拖动时吸附到整数。
    property bool integer: false

    implicitWidth: 200
    implicitHeight: 28

    readonly property real knobR: 8
    readonly property real span: Math.max(1, to - from)
    readonly property real knobX: knobR + (value - from) / span * (width - knobR * 2)

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: 4
        radius: 2
        color: Qt.rgba(1, 1, 1, 0.10)
    }

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        width: root.knobX
        height: 4
        radius: 2
        color: Theme.accent
    }

    Rectangle {
        width: root.knobR * 2
        height: root.knobR * 2
        radius: root.knobR
        anchors.verticalCenter: parent.verticalCenter
        x: root.knobX - root.knobR
        color: "white"
    }

    MouseArea {
        anchors.fill: parent
        // 24px 高的条不好按，热区上下各放宽 6px。
        anchors.margins: -6
        cursorShape: Qt.PointingHandCursor
        onPressed: seek(mouse.x)
        onPositionChanged: if (pressed) seek(mouse.x)
    }

    function seek(mx) {
        var t = Math.min(1, Math.max(0, (mx - knobR) / (width - knobR * 2)));
        var v = from + t * (to - from);
        value = integer ? Math.round(v) : v;
    }
}
