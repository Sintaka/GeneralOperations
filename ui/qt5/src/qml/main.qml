// 切片 2：Outliner 显示真实脚本树。
//
// 面板刻意不用 Layout 嵌套，而是 anchors 直接定位在根 Item 上 ——
// GlassPanel 的 sourceRect 需要窗口坐标，见 GlassPanel.qml 顶部说明。
//
// 【无边框窗口】系统标题栏被砍掉（FramelessWindowHint），顶替的三件套：
//   右上角圆形按钮  → WindowControls.qml（抄鹰角启动器）
//   四边缩放热区    → WindowResizer.qml
//   整窗拖拽        → 本文件的 rootDrag MouseArea

import QtQuick 2.15
import QtQuick.Window 2.15
import App 1.0

Window {
    id: win
    width: 1040
    height: 660
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: qsTr("GeneralOperations Launcher")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: Theme.bgBottom

    readonly property int margin: 18
    readonly property int gap: 14
    readonly property int leftWidth: 260

    // 命令行直跑（调试入口）：launcher.exe <脚本> <文件...>。
    // 选中脚本后立刻按"面板当前参数"执行 —— selectedInfo 的赋值会同步
    // 触发 ParamEditor 的 values 初始化，所以这里拿到的就是脚本头里
    // 声明的默认值，不用额外等一帧。
    Component.onCompleted: {
        // 空 QVariantMap 在 QML 是空对象，.script 取出来是 undefined 而不是
        // ""，所以这里用真值判断而不是和空串比较。
        if (!startupRequest.script)
            return;
        var p = scriptModel.resolveScript(startupRequest.script);
        if (p === "") {
            console.warn("[命令行] 找不到脚本:", startupRequest.script);
            return;
        }
        var info = scriptModel.scriptInfo(p);
        detailName.text = info.name || startupRequest.script;
        detailPath.text = p;
        rightPanel.selectedInfo = info;
        scriptRunner.run(p, startupRequest.files, paramEditor.values);
    }

    Item {
        id: rootItem
        anchors.fill: parent

        // ---- 整窗拖拽 ----
        // 无边框后没有标题栏可抓。这个 MouseArea 声明在最前面（z 最低），
        // 只兜住其它 MouseArea 都没接住的空白处：面板外的背景、面板上
        // 没有控件的区域。列表行、按钮的点击/悬停仍然优先命中它们自己。
        MouseArea {
            id: rootDrag
            anchors.fill: parent
            // 系统级移动循环：光标反馈和松开时的定位由 Windows 处理，
            // 手写 onPositionChanged 改 x/y 会绕过窗口管理器。
            onPressed: win.startSystemMove()
        }

        // 背景本体。整窗只有这一份，两块面板都从它取样。
        Backdrop {
            id: backdrop
            anchors.fill: parent
        }

        // 背景的纹理化包装。live: false 是关键 ——
        // 背景是静态的，只渲染一次，之后面板取样都是白拿。
        ShaderEffectSource {
            id: backdropSource
            sourceItem: backdrop
            anchors.fill: parent
            live: false
            hideSource: false
            visible: false
        }

        GlassPanel {
            id: leftPanel
            backdropSource: backdropSource
            x: win.margin
            y: win.margin
            width: win.leftWidth
            height: rootItem.height - win.margin * 2

            ScriptOutliner {
                anchors.fill: parent
                // 上下留白比左右大：行自己已带 8px 内边距（Theme.rowPadding），
                // 左右再给多了会让图标缩进看起来偏心。
                anchors.margins: 8
                anchors.topMargin: 12
                anchors.bottomMargin: 12
                model: scriptModel
                onScriptSelected: function(filePath, name) {
                    detailName.text = name
                    detailPath.text = filePath
                    // 右面板按快照刷新拖放提示；校验以 validateDrop 为准。
                    rightPanel.selectedInfo = scriptModel.scriptInfo(filePath)
                }
            }
        }

        GlassPanel {
            id: rightPanel
            backdropSource: backdropSource
            x: leftPanel.x + leftPanel.width + win.gap
            y: win.margin
            width: rootItem.width - x - win.margin
            height: rootItem.height - win.margin * 2

            // 当前选中脚本的 scriptInfo() 快照。放面板上而不是 DropZone 里：
            // onScriptSelected 是面板数据流的入口，DropZone 只管画。
            property var selectedInfo: ({})

            Column {
                id: detailHeader
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 8

                Text {
                    text: qsTr("Parameters")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    id: detailName
                    text: qsTr("从左侧选一个脚本")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    id: detailPath
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    width: parent.width
                    elide: Text.ElideMiddle
                }
            }

            // ---- 参数面板 ----
            // 占 header 和底部输出块之间的全部空间；没有 @param 的脚本
            // 这里是空的（Repeater 空转，不占视觉）。
            ParamEditor {
                id: paramEditor
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: detailHeader.bottom
                anchors.bottom: bottomBlock.top
                anchors.margins: 20
                anchors.topMargin: 14
                anchors.bottomMargin: 4
                params: rightPanel.selectedInfo.params || []
            }

            // ---- 底部块：警示 / 提示 / 运行状态 / 输出尾部 ----
            // 只锚 bottom，高度随内容向上长；ParamEditor 的 bottom 锚着
            // 它的 top，输出变多时参数区自动让位。
            Column {
                id: bottomBlock
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 20
                anchors.bottomMargin: 14
                spacing: 6

                Text {
                    width: parent.width
                    visible: rightPanel.selectedInfo.destructive === true
                    text: qsTr("破坏性操作：%1").arg(rightPanel.selectedInfo.destructiveReason || "")
                    color: Theme.danger
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    wrapMode: Text.WrapAnywhere
                }

                Text {
                    width: parent.width
                    visible: rightPanel.selectedInfo.accepts !== undefined
                    text: {
                        var i = rightPanel.selectedInfo;
                        var what = i.accepts === "dir" ? "文件夹"
                                 : i.accepts === "both" ? "文件或文件夹" : "文件";
                        var exts = i.extensions && i.extensions.length > 0
                                 ? i.extensions.join(" / ") : "任意扩展名";
                        var how = i.multi ? "可多选" : "一次一个";
                        return qsTr("拖入%1执行 · %2 · %3").arg(what).arg(exts).arg(how);
                    }
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    elide: Text.ElideMiddle
                }

                Text {
                    id: runStatus
                    width: parent.width
                    visible: text !== ""
                    text: {
                        if (scriptRunner.running)
                            return qsTr("执行中…");
                        if (scriptRunner.spawnError !== "")
                            return scriptRunner.spawnError;
                        if (scriptRunner.hasRun)
                            return scriptRunner.lastExitCode === 0
                                ? qsTr("执行完成")
                                : qsTr("执行失败（退出码 %1）").arg(scriptRunner.lastExitCode);
                        return "";
                    }
                    color: scriptRunner.running ? Theme.accent
                         : (scriptRunner.spawnError === "" && scriptRunner.lastExitCode === 0)
                         ? Theme.textSecondary : Theme.danger
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                    Behavior on color {
                        ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                    }
                }

                Text {
                    width: parent.width
                    visible: scriptRunner.tail !== ""
                    text: scriptRunner.tail
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    // 输出多于 12 行时 clip 到可用空间（tail 本身也只留 12 行）。
                    clip: true
                }
            }

            // ---- 隐藏式拖放覆盖层 ----
            // 平时 opacity 0 不挡视觉，拖入时淡入盖住整个面板（含参数区）。
            DropZone {
                id: dropZone
                anchors.fill: parent
                model: scriptModel
                runner: scriptRunner
                filePath: detailPath.text
                info: rightPanel.selectedInfo
                paramValues: paramEditor.values
            }
        }

        // ---- 无边框窗口三件套的另外两件 ----
        // 缩放热区盖在面板之上（四边 6px 内优先于面板内容），
        // 控制按钮离边缘 28px，两者不重叠（见 WindowResizer.qml 的层级约定）。
        WindowResizer {
            anchors.fill: parent
            target: win
        }

        WindowControls {
            id: winControls
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: winControls.floatOffset
            anchors.rightMargin: winControls.floatOffset
            target: win
        }

        // ==== 拖放诊断模式（--dropdebug）====
        // 不拦截拖放：C++ 事件过滤器被动记录平台层（QMimeData 原始格式），
        // DropZone 的 onEntered/onDropped 记录 QML 层校验链路，都写
        // dropdebug.log。正常 UI 保持可用 —— 诊断的就是真实链路。
        Text {
            visible: dropDebugMode === true
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 6
            text: qsTr("诊断模式运行中（日志：dropdebug.log）")
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontMicro
        }
    }
}
