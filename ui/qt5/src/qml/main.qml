// 切片 2：Outliner 显示真实脚本树。
//
// 面板刻意不用 Layout 嵌套，而是 anchors 直接定位在根 Item 上 ——
// GlassPanel 的 sourceRect 需要窗口坐标，见 GlassPanel.qml 顶部说明。
//
// 【无边框窗口】系统标题栏被砍掉（FramelessWindowHint），顶替的三件套：
//   右上角圆形按钮  → WindowControls.qml（抄鹰角启动器）
//   四边缩放热区    → WindowResizer.qml
//   整窗拖拽        → 本文件的 rootDrag MouseArea
// 最小化/还原不走原生瞬间切换：WindowControls 只发请求信号，这里先播
// rootItem 的内容收起/展开动画（Windows 观感），见"最小化 / 还原动画"。

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

    // 运行中由窗口级快捷键独占 Ctrl+C；TextEdit 不再额外处理按键，因此一次
    // 按键只会走一次 cancel()。空闲时禁用，Ctrl+C 继续交给有焦点的文本编辑器复制。
    Shortcut {
        sequence: "Ctrl+C"
        context: Qt.WindowShortcut
        autoRepeat: false
        enabled: scriptRunner.running
        onActivated: scriptRunner.cancel()
    }

    // ---- 参数存档恢复 ----
    // selectedInfo 赋值后 ParamEditor 会同步用默认值重建 values（其
    // onParamsChanged），所以恢复必须等那一轮跑完 —— Qt.callLater 恰好
    // 排在本轮全部同步求值之后。只覆盖当前契约里存在的参数名：脚本改过
    // 参数名/删过参数后，旧存档里多出来的键静默丢弃，不会塞进 values。
    function applySavedValues(savedValues) {
        var merged = {};
        for (var k in paramEditor.values)
            merged[k] = paramEditor.values[k];
        for (var k in savedValues)
            if (k in merged)
                merged[k] = savedValues[k];
        paramEditor.values = merged;
    }

    // ---- 最小化 / 还原动画（Windows 风格的简单缩放）----
    // 原生窗口最小化/还原是瞬间消失/出现；这里补一个贴近 Windows 观感的
    // 内容收起/展开：rootItem 以底部中心为原点 scale 1→0.9、opacity 1→0，
    // 走 Animator（渲染线程），时长/easing 复用 Theme.durFast/Theme.easing。
    //
    // 时序约定：
    //   收起  = 收起动画播完（onStopped）才真正 showMinimized()。
    //   还原  = 任何进入 Minimized 的时刻把内容置为收起态（此刻窗口不可见，
    //           赋值不产生可见帧；UI 按钮路径下动画本来就停在收起态，赋值
    //           是幂等的；任务栏/Win+D 路径下内容还停在正常态，趁不可见先
    //           收起，还原动画才有正确起点），回到 Windowed 再播展开动画
    //           ——还原的第一帧就是收起态，不会闪一帧全尺寸。
    //   防重入 = 收起/展开进行中再点最小化直接忽略（requestMinimize）。
    //   首启豁免 = minimizeArmed 只在真的进过一次 Minimized 后置位，
    //           启动时 visibility 变成 Windowed 不会触发任何动画。
    // 显式动画而不是 Behavior：只在最小化/还原时按需触发，不劫持程序性
    // 的 scale/opacity 赋值。动画对象声明为单例复用，不每次 new。
    readonly property real collapseScale: 0.9
    property bool minimizeArmed: false

    function requestMinimize() {
        if (collapseScaleAnim.running || restoreScaleAnim.running)
            return;
        collapseOpacityAnim.start();
        collapseScaleAnim.start();   // stopped 回调负责真正 showMinimized
    }

    onVisibilityChanged: {
        if (visibility === Window.Minimized) {
            rootItem.scale = collapseScale
            rootItem.opacity = 0.0
            minimizeArmed = true
        } else if (minimizeArmed && visibility === Window.Windowed) {
            minimizeArmed = false
            restoreOpacityAnim.start()
            restoreScaleAnim.start()
        }
    }

    ScaleAnimator {
        id: collapseScaleAnim
        target: rootItem
        from: 1.0
        to: win.collapseScale
        duration: Theme.durFast
        easing.type: Theme.easing
        onStopped: win.showMinimized()
    }

    OpacityAnimator {
        id: collapseOpacityAnim
        target: rootItem
        from: 1.0
        to: 0.0
        duration: Theme.durFast
        easing.type: Theme.easing
    }

    ScaleAnimator {
        id: restoreScaleAnim
        target: rootItem
        from: win.collapseScale
        to: 1.0
        duration: Theme.durFast
        easing.type: Theme.easing
    }

    OpacityAnimator {
        id: restoreOpacityAnim
        target: rootItem
        from: 0.0
        to: 1.0
        duration: Theme.durFast
        easing.type: Theme.easing
    }

    // 命令行直跑（调试入口）：launcher.exe <脚本> <文件...>。
    // 选中脚本后立刻按"面板当前参数"执行 —— selectedInfo 的赋值会同步
    // 触发 ParamEditor 的 values 初始化，所以这里拿到的就是脚本头里
    // 声明的默认值，不用额外等一帧。
    Component.onCompleted: {
        // 布局存档只在启动时读一次（运行中不回读，见 LayoutStore）。
        // 折叠表整体赋给 outliner 后，onExpandedGroupsChanged 会把它
        // 原样存回去一次，幂等无害。
        outliner.expandedGroups = layoutStore.expandedGroups();

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
        // 收起动画的缩放原点（scale 恒为 1 时无任何视觉影响）。
        transformOrigin: Item.Bottom

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
            // 【半分辨率 + 量化】resize 时本 item 尺寸逐帧跟着变，若纹理
            // 尺寸 = item 尺寸，整窗 FBO 就逐帧重分配。textureSize 锁半分辨率
            // （对齐 8px，GlassPanel.slice 同思路）：背景只有渐变 + 一张等比
            // 缩放的光斑纹理，没有高频内容，两块面板取样后还要过 20px 模糊
            // ——半分辨率无感，重分配从每帧变偶发。
            textureSize: Qt.size(
                Math.max(1, Math.round(rootItem.width / 16) * 8),
                Math.max(1, Math.round(rootItem.height / 16) * 8))
        }

        GlassPanel {
            id: leftPanel
            backdropSource: backdropSource
            x: win.margin
            y: win.margin
            width: win.leftWidth
            height: rootItem.height - win.margin * 2

            ScriptOutliner {
                id: outliner
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
                    // 【顺序】savedValues 必须在 selectedInfo 赋值【之前】取：
                    // selectedInfo 一变 ParamEditor 就用默认值重建 values，
                    // onValuesChanged 会把"默认值"当成该脚本的当前参数存进
                    // layoutStore，先存后取等于自己把自己的存档冲掉。
                    var savedValues = layoutStore.scriptParams(filePath)
                    rightPanel.selectedInfo = scriptModel.scriptInfo(filePath)
                    if (Object.keys(savedValues).length > 0)
                        Qt.callLater(applySavedValues, savedValues)
                }
                // 折叠状态每次变化都交给 LayoutStore（内部防抖落盘）。
                onExpandedGroupsChanged: layoutStore.saveExpandedGroups(expandedGroups)
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
                // 参数每次变化都进存档（内部防抖落盘）。没选中脚本时
                // detailPath 为空，跳过 —— 不给空 key 写记录。
                onValuesChanged: {
                    if (detailPath.text !== "")
                        layoutStore.saveScriptParams(detailPath.text, values)
                }
            }

            // ---- 底部块：警示 / 提示 / 运行状态 / 固定高度输出框 ----
            // 输出框高度固定，日志增长只改变内部 Flickable 的 contentHeight，
            // 不再向上挤压参数区。
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
                        var what = i.accepts === "dir" ? qsTr("文件夹")
                                 : i.accepts === "both" ? qsTr("文件或文件夹") : qsTr("文件");
                        var exts = i.extensions && i.extensions.length > 0
                                 ? i.extensions.join(" / ") : qsTr("任意扩展名");
                        var how = i.multi ? qsTr("可多选") : qsTr("一次一个");
                        return qsTr("拖入%1执行 · %2 · %3").arg(what).arg(exts).arg(how);
                    }
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    elide: Text.ElideMiddle
                }

                Text {
                    id: runStatus
                    objectName: "runStatus"
                    width: parent.width
                    visible: text !== ""
                    text: {
                        if (scriptRunner.running)
                            return qsTr("执行中…");
                        if (scriptRunner.spawnError !== "")
                            return scriptRunner.spawnError;
                        if (scriptRunner.cancelled)
                            return qsTr("执行已取消");
                        if (scriptRunner.hasRun)
                            return scriptRunner.lastExitCode === 0
                                ? qsTr("执行完成")
                                : qsTr("执行失败（退出码 %1）").arg(scriptRunner.lastExitCode);
                        return "";
                    }
                    color: scriptRunner.running ? Theme.accent
                         : scriptRunner.cancelled ? Theme.textTertiary
                         : (scriptRunner.spawnError === "" && scriptRunner.lastExitCode === 0)
                         ? Theme.textSecondary : Theme.danger
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                    Behavior on color {
                        ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                    }
                }

                CommandOutput {
                    width: parent.width
                    height: 132
                    visible: scriptRunner.tail !== ""
                    text: scriptRunner.tail
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
            // 最小化只发请求：真正的 showMinimized 要等收起动画播完
            // （见文件顶部"最小化 / 还原动画"的时序约定）。
            onMinimizeRequested: requestMinimize()
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
