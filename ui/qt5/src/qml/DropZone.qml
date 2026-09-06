// 隐藏式拖放覆盖层 —— 平时完全不占视觉，拖入时盖住整个右面板。
//
// 【结构铁律，踩过一次坑】DropArea 必须放在常驻可见的 root 上，
// 视觉覆盖层（cover）是它的兄弟节点而不是父节点。之前把 DropArea
// 塞在 visible:false 的覆盖层里 —— Qt Quick 里父项不可见时子项收不到
// 任何事件，于是"拖入才显示"的覆盖层永远等不到拖入，死锁。
//
// 覆盖层可见 = containsDrag（拖入悬停）|| confirming（破坏性确认）||
// rejectFlash（非法投放的原因还在闪）。确认态必须让覆盖层留在场上 ——
// 确认按钮就在里面。
//
// 规则仍全部来自 docs/SCRIPT_SPEC.md，裁决在 C++（validateDrop），
// 这里只画结果。虚线框 Canvas 画（静态一次绘制，见 Glyph.qml 顶部
// 对 Canvas 使用边界的约定）。

import QtQuick 2.15
import App 1.0

Item {
    id: root

    /// ScriptListModel（main.qml 传入），只调 validateDrop。
    property var model: null
    /// ScriptRunner 实例（main.qml 传入 scriptRunner）。
    property var runner: null
    /// 当前选中的脚本路径；空 = 未选中。
    property string filePath: ""
    /// scriptModel.scriptInfo(filePath) 的快照，见 ScriptListModel。
    property var info: ({})
    /// ParamEditor 的当前值字典，原样转交 ScriptRunner.run()。
    property var paramValues: ({})

    /// 悬停中的实时校验结果（validateDrop 返回值），null = 不在悬停。
    property var hoverResult: null

    // ---- 破坏性确认态 ----
    property bool confirming: false
    property var pendingFiles: []

    /// 非法投放的闪示文案（带超时自动清除）。
    property string rejectFlash: ""

    readonly property bool selected: filePath !== "" && info.accepts !== undefined
    readonly property bool dragHover: area.containsDrag

    // 选中目标变化时复位所有交互态：确认里换脚本，挂着的确认必须作废。
    onInfoChanged: {
        hoverResult = null;
        confirming = false;
        pendingFiles = [];
    }

    // ---- 拖放事件 ----
    // 常驻可见，盖住整个面板（含参数区）。没有选中脚本时也接 ——
    // 悬停给"先选脚本"的引导，比资源管理器一个哑巴禁止光标友好。
    DropArea {
        id: area
        anchors.fill: parent
        // 确认中 / 运行中不接受新拖入：并发拒绝在 C++ 有兜底，
        // UI 直接关掉更直观。
        enabled: !root.confirming && !(root.runner && root.runner.running)

        // drag.urls 是 QML 的 url 列表对象，直接传给 Q_INVOKABLE 的
        // QVariantList 参数实测会变成空列表（url 包装类型的转换丢了元素）。
        // 先序列化成纯字符串数组再过桥 —— 字符串到 QVariant 的转换是可靠的。
        function urlStrings(urls) {
            var out = [];
            for (var i = 0; i < urls.length; ++i)
                out.push(String(urls[i]));
            return out;
        }

        onEntered: {
            // 校验链路日志随 --dropdebug 启用（普通启动 debugLogger 是
            // 哑实例，零开销）。这次的坑记录在 docs/pitfalls/
            // 2026-09-06-dragdrop-urls-empty.md，下次撞同类坑先看那里。
            var msg;
            if (!root.selected) {
                msg = { ok: false, reason: qsTr("先在左侧选择一个脚本"), files: [] };
            } else if (!drag.hasUrls) {
                msg = { ok: false, reason: qsTr("只支持拖入文件"), files: [] };
            } else {
                var res = root.model.validateDrop(root.filePath, urlStrings(drag.urls));
                if (!res.ok && res.uncertain === true) {
                    // OLE 延迟渲染（DOpus 等）：悬停阶段 GetData 拿不到路径，
                    // 只有松手时数据才可用。这里不判死刑 —— 给中性反馈，
                    // 裁决推迟到 onDropped（那边会用渲染好的数据重新校验）。
                    res = { ok: true, uncertain: true, files: [], reason: "" };
                }
                msg = res;
            }
            debugLogger.log("[DropZone entered] selected=" + root.selected
                            + " filePath=" + root.filePath
                            + " hasUrls=" + drag.hasUrls
                            + " rawUrls=" + JSON.stringify(drag.urls)
                            + " result=" + JSON.stringify(msg));
            root.hoverResult = msg;
        }
        onExited: root.hoverResult = null

        onDropped: {
            // 重新校验而不是复用 hoverResult：延迟渲染的来源（DOpus 等）
            // 在悬停阶段拿不到路径，松手这一刻数据才真正可用 —— 以此为准。
            var res = root.selected
                ? root.model.validateDrop(root.filePath, urlStrings(drop.urls))
                : { ok: false, reason: qsTr("先在左侧选择一个脚本") };
            root.hoverResult = null;
            debugLogger.log("[DropZone dropped] selected=" + root.selected
                            + " filePath=" + root.filePath
                            + " hasUrls=" + drop.hasUrls
                            + " rawUrls=" + JSON.stringify(drop.urls)
                            + " result=" + JSON.stringify(res));

            if (!res.ok) {
                root.rejectFlash = res.reason;
                rejectTimer.restart();
            } else if (res.files.length === 0) {
                // uncertain 状态下松手仍拿不到路径：不是延迟渲染了，
                // 是真给不出文件。
                root.rejectFlash = qsTr("没有拖入有效的文件");
                rejectTimer.restart();
            } else if (root.info.destructive === true) {
                root.pendingFiles = res.files;
                root.confirming = true;
            } else {
                root.runner.run(root.filePath, res.files, root.paramValues);
            }
        }
    }

    // 非法投放原因的自动清除。
    Timer {
        id: rejectTimer
        interval: 2600
        onTriggered: root.rejectFlash = ""
    }

    // ---- 覆盖层：纯视觉，可隐藏 ----
    Item {
        id: cover
        anchors.fill: parent

        opacity: (root.dragHover || root.confirming || root.rejectFlash !== "") ? 1 : 0
        visible: opacity > 0.001
        Behavior on opacity {
            NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
        }

        // 近黑 90% 压住下面的参数面板，虚线框内缩 10px。
        Rectangle {
            anchors.fill: parent
            color: Theme.surfacePop
        }

        Item {
            id: frame
            anchors.fill: parent
            anchors.margins: 10

            Canvas {
                id: dash
                anchors.fill: parent

                onPaint: {
                    var ctx = getContext("2d");
                    ctx.reset();
                    // 6/4 的疏密没有 CSS 原值可抄（web 版没有拖放框），
                    // 对着鹰角 .switch-bar 分隔线的手感调的。
                    ctx.setLineDash([6, 4]);
                    ctx.lineWidth = 1;
                    // 确认态比悬停态更醒目一档。
                    ctx.strokeStyle = root.confirming
                            ? Qt.rgba(1, 1, 1, 0.30)
                            : root.hoverResult
                            ? (root.hoverResult.ok ? Qt.rgba(1, 1, 1, 0.45) : Theme.danger)
                            : Qt.rgba(1, 1, 1, 0.18);

                    var r = Theme.radius;
                    var w = width, h = height;
                    ctx.beginPath();
                    ctx.moveTo(r, 0);
                    ctx.lineTo(w - r, 0);  ctx.arcTo(w, 0, w, r, r);
                    ctx.lineTo(w, h - r);  ctx.arcTo(w, h, w - r, h, r);
                    ctx.lineTo(r, h);      ctx.arcTo(0, h, 0, h - r, r);
                    ctx.lineTo(0, r);      ctx.arcTo(0, 0, r, 0, r);
                    ctx.closePath();
                    ctx.stroke();
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Connections {
                    target: root
                    function onConfirmingChanged() { dash.requestPaint() }
                    function onHoverResultChanged() { dash.requestPaint() }
                }
            }
        }

        // ---- 中央文案 ----
        Column {
            anchors.centerIn: parent
            width: parent.width - 80
            spacing: 6

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                text: {
                    if (!root.selected)
                        return qsTr("先在左侧选择一个脚本");
                    if (root.confirming)
                        return qsTr("即将执行：%1").arg(root.info.name || "");
                    if (root.hoverResult)
                        return root.hoverResult.ok
                            ? (root.hoverResult.uncertain === true
                               ? qsTr("松手执行")
                               : qsTr("松手执行 · %1 个目标").arg(root.hoverResult.files.length))
                            : qsTr("不能拖入");
                    return qsTr("松手把文件交给当前脚本");
                }
                color: {
                    if (!root.selected)
                        return Theme.textTertiary;
                    if (root.hoverResult)
                        return root.hoverResult.ok ? Theme.textPrimary : Theme.danger;
                    return Theme.textSecondary;
                }
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                Behavior on color {
                    ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                }
            }

        // 悬停时的文件清单（最多 4 行，余下的合并计数）。
        // uncertain（延迟渲染）阶段拿不到路径，不显示这行。
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            visible: root.hoverResult !== null && root.hoverResult.ok
                     && root.hoverResult.uncertain !== true
            text: {
                if (!root.hoverResult || !root.hoverResult.ok)
                    return "";
                    var files = root.hoverResult.files;
                    var names = [];
                    for (var i = 0; i < files.length && i < 4; ++i) {
                        var s = String(files[i]).replace(/\\/g, "/");
                        names.push(s.substring(s.lastIndexOf("/") + 1));
                    }
                    if (files.length > 4)
                        names.push("…和另外 " + (files.length - 4) + " 个");
                    return names.join("\n");
                }
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontMicro
            }

            // 破坏性警示：确认态的主文案。
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                visible: root.confirming && root.info.destructive === true
                text: qsTr("破坏性操作：%1").arg(root.info.destructiveReason || "")
                color: Theme.danger
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
            }

            // 非法投放的闪示原因。
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: root.rejectFlash !== ""
                text: root.rejectFlash
                color: Theme.danger
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontMicro
            }

            // ---- 破坏性确认按钮 ----
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                visible: root.confirming

                Rectangle {
                    width: 84; height: 30
                    radius: Theme.radius
                    color: confirmMouse.containsMouse ? Theme.accentHover : Theme.accent
                    Behavior on color {
                        ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("确认执行")
                        color: Theme.textOnAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: confirmMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var files = root.pendingFiles;
                            root.confirming = false;
                            root.pendingFiles = [];
                            root.runner.run(root.filePath, files, root.paramValues);
                        }
                    }
                }

                Rectangle {
                    width: 64; height: 30
                    radius: Theme.radius
                    color: cancelMouse.containsMouse ? Theme.fillHover : "transparent"
                    border.width: 1
                    border.color: Theme.stroke
                    Behavior on color {
                        ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("取消")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    MouseArea {
                        id: cancelMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.confirming = false;
                            root.pendingFiles = [];
                        }
                    }
                }
            }
        }
    }
}
