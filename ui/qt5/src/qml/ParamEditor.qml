// 参数面板 —— 把选中脚本的 @param 契约渲染成控件。
//
// 控件映射（docs/SCRIPT_SPEC.md「参数声明」节）：
//   int / float → 滑块；第 6 段带 presets 的（如 max_pixels）多一排
//                 快捷档位胶囊（1024→1K、2048→2K、4096→4K）+ "自定义"，
//                 点自定义才展开滑块；其中 int 参数的滑块吸附到倍增
//                 档位（min, min*2, ... 直到 ≥max 取 max），跑在索引空间
//                 并在滑块右侧实时显示当前值
//   bool        → 行尾自绘复选框（store_true 语义：开 = 传 --flag）
//   choice      → 第二行互斥胶囊组
//   str / path  → 暂不支持编辑，只提示用默认值（当前 8 个脚本没有这类参数）
//
// 【values 的更新纪律】字典整体重建赋值（setValue），禁止 in-place 写 ——
// 对 var 对象做 obj[k] = v 不触发任何绑定，档位胶囊的选中态会不刷新，
// 和 ScriptOutliner.expandedGroups 是同一个坑。

import QtQuick 2.15
import App 1.0

Item {
    id: root

    /// scriptInfo().params —— 见 ScriptListModel。
    property var params: []
    /// name → 当前值。含未改动的默认值，执行时整体交给 ScriptRunner。
    property var values: ({})

    onParamsChanged: {
        var next = {};
        for (var i = 0; i < params.length; ++i)
            next[params[i].name] = params[i].default;
        values = next;
    }

    function setValue(name, v) {
        var next = {};
        for (var k in values)
            next[k] = values[k];
        next[name] = v;
        values = next;
    }

    // 1024 → "1K"、2048 → "2K"，非整 KB 的值原样显示。
    function formatPreset(v) {
        if (v >= 1024 && Math.abs(v % 1024) < 0.5) {
            var k = v / 1024;
            if (Math.abs(k - Math.round(k)) < 0.01)
                return Math.round(k) + "K";
        }
        return String(Math.round(v * 100) / 100);
    }

    // 当前值是否等于某预设（浮点比较留 0.5 的容差，够 UI 粒度用）。
    function matchesPreset(name, preset) {
        var v = values[name];
        return v !== undefined && Math.abs(v - preset) < 0.5;
    }

    // 有 presets 的 int 参数专属：滑块跑在索引空间，档位从 min 起倍增，
    // 直到 ≥max 时收在 max（min, min*2, min*4, ...）。例如 min=8、
    // max=16384 → 8,16,...,16384 共 12 档。普通 int/float 返回空数组，
    // 滑块保持连续。
    function presetLadder(p) {
        if (p === undefined || p.min === undefined || p.max === undefined
                || p.max <= p.min)
            return [];
        var ladder = [p.min];
        var cur = p.min;
        while (cur < p.max) {
            cur = Math.min(cur * 2, p.max);
            ladder.push(cur);
        }
        return ladder;
    }

    // 当前值最近的档位索引（初始化装载用，只换滑块位置，不改值本身）。
    function ladderIndex(ladder, v) {
        if (!ladder || ladder.length === 0 || v === undefined)
            return 0;
        var best = 0;
        for (var i = 1; i < ladder.length; ++i)
            if (Math.abs(ladder[i] - v) < Math.abs(ladder[best] - v))
                best = i;
        return best;
    }

    clip: true

    Column {
        anchors.fill: parent
        spacing: 12

        Repeater {
            model: root.params

            Column {
                id: paramItem

                required property var modelData

                readonly property string name: modelData.name
                readonly property string type: modelData.type
                readonly property bool isNumeric: type === "int" || type === "float"
                readonly property bool hasPresets: modelData.presets !== undefined
                /// 有 presets 的 int 参数：滑块跑在倍增档位的索引空间。
                readonly property var ladder: hasPresets && type === "int"
                    ? root.presetLadder(modelData) : []
                readonly property bool snapToLadder: ladder.length > 1
                /// 点了"自定义"档位后滑块才展开。
                property bool custom: false

                // 重新展开"自定义"时把滑块对回当前值的最近档位 ——
                // 否则上一次拖动残留的索引会在 onValueChanged 里把值改回旧档。
                onCustomChanged: {
                    if (!custom || !sliderReady || !snapToLadder)
                        return;
                    var idx = root.ladderIndex(ladder, root.values[name]);
                    if (paramSlider.value !== idx)
                        paramSlider.value = idx;
                }

                width: parent ? parent.width : 0
                spacing: 8

                // ---- 第一行：标签 + 行尾控件 ----
                Item {
                    width: parent.width
                    height: 30

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: paramItem.modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                        // 5 个预设胶囊 + "自定义"（18px 内边距、5px 间距）
                        // 实测约 264px：有 presets 的行把标签空间收到 280，
                        // 窗口最小宽 720（参数区约 380px）时胶囊压不到标签。
                        width: parent.width - (paramItem.hasPresets ? 280 : 220)
                    }

                    // 快捷档位胶囊（max_pixels 的 512/1K/2K/4K/8K/自定义）。
                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 5
                        visible: paramItem.hasPresets

                        Repeater {
                            model: paramItem.modelData.presets

                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool active: !paramItem.custom
                                    && root.matchesPreset(paramItem.name, modelData)

                                height: 28
                                width: chipText.width + 18
                                radius: Theme.radius
                                color: active ? Theme.fillSelected
                                    : chipMouse.containsMouse ? Theme.fillHover : "transparent"
                                border.width: 1
                                border.color: active ? Qt.rgba(1, 1, 1, 0.25) : Theme.stroke
                                Behavior on color {
                                    ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                                }

                                Text {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: root.formatPreset(parent.modelData)
                                    color: parent.active ? Theme.textPrimary : Theme.textTertiary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontBody
                                }
                                MouseArea {
                                    id: chipMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        paramItem.custom = false;
                                        root.setValue(paramItem.name, parent.modelData);
                                    }
                                }
                            }
                        }

                        // "自定义"档位：当前值不等于任何预设时视为选中。
                        Rectangle {
                            height: 28
                            width: customText.width + 18
                            radius: Theme.radius
                            color: paramItem.custom ? Theme.fillSelected
                                : customMouse.containsMouse ? Theme.fillHover : "transparent"
                            border.width: 1
                            border.color: paramItem.custom ? Qt.rgba(1, 1, 1, 0.25) : Theme.stroke
                            Behavior on color {
                                ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                            }

                            Text {
                                id: customText
                                anchors.centerIn: parent
                                text: qsTr("自定义")
                                color: paramItem.custom ? Theme.textPrimary : Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                            }
                            MouseArea {
                                id: customMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: paramItem.custom = true
                            }
                        }
                    }

                    // 无档位的数值参数：行尾直接显示当前值。
                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: paramItem.isNumeric && !paramItem.hasPresets
                        text: {
                            var v = root.values[paramItem.name];
                            return v === undefined ? "" : String(Math.round(v * 100) / 100);
                        }
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                    }

                    // bool：行尾复选框。开 = 传 --flag（store_true）。
                    Item {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: paramItem.type === "bool"
                        width: 20
                        height: 20

                        readonly property bool checked: root.values[paramItem.name] === true

                        Rectangle {
                            anchors.fill: parent
                            radius: 4
                            color: parent.checked ? Theme.accent : "transparent"
                            border.width: 1
                            border.color: parent.checked ? Theme.accent : Theme.strokeHover
                            Behavior on color {
                                ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                            }
                        }
                        // 勾：一小横一竖杠拼出来的对勾没有旋转简洁，
                        // 这里直接放一个深色内块，语义和开关一致。
                        Rectangle {
                            anchors.centerIn: parent
                            width: 10; height: 10; radius: 3
                            color: Theme.textOnAccent
                            visible: parent.checked
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.setValue(paramItem.name,
                                                     !(parent.checked))
                        }
                    }
                }

                // ---- 第二行：滑块 / choice 胶囊 / 不支持提示 ----

                // 【sliderReady 守卫，踩过坑】GlassSlider 的 value 默认绑定
                // from，delegate 创建时 from 一赋值 value 就变成范围最小值，
                // onValueChanged 会把脚本头声明的默认值覆盖成 min（实测
                // max_pixels 默认 1024 被写成 256、quality 80 被写成 1）。
                // 所以初始化期间不回写，等 value 装载完默认值再放开。
                property bool sliderReady: false

                // 滑块行包一层 Item：吸附档位时滑块右侧还要挂一个实时
                // 数值文本，Column 的子项不能再锚定，得借这层父级定位。
                Item {
                    width: parent.width
                    height: 28
                    visible: paramItem.isNumeric
                             && paramItem.modelData.min !== undefined
                             && (!paramItem.hasPresets || paramItem.custom)

                    GlassSlider {
                        id: paramSlider
                        objectName: "paramSlider"
                        // 吸附档位时右侧留 52px 给实时数值文本。
                        width: paramItem.snapToLadder ? parent.width - 52 : parent.width
                        // bool/无约束参数的 min/max 是 undefined，滑块又不可见，
                        // 但绑定仍会求值 —— 不兜底会刷 "assign undefined" 警告。
                        from: paramItem.snapToLadder || paramItem.modelData.min === undefined
                              ? 0 : paramItem.modelData.min
                        to: paramItem.snapToLadder
                            ? paramItem.ladder.length - 1
                            : (paramItem.modelData.max === undefined
                               ? 100 : paramItem.modelData.max)
                        integer: paramItem.type === "int"
                        Component.onCompleted: {
                            var v = root.values[paramItem.name];
                            // str/bool 参数的滑块不可见但这里照常执行，
                            // 它们的默认值（""、false）塞进 value 会刷
                            // "Cannot assign QString to double"。
                            if (v !== undefined && paramItem.isNumeric)
                                value = paramItem.snapToLadder
                                    ? root.ladderIndex(paramItem.ladder, v) : v;
                            paramItem.sliderReady = true;
                        }
                        onValueChanged: if (paramItem.sliderReady)
                            root.setValue(paramItem.name,
                                paramItem.snapToLadder
                                    ? paramItem.ladder[Math.round(value)] : value)
                    }

                    // 自定义滑块的实时数值（跟随 values，拖动即刷新）。
                    Text {
                        objectName: "paramSliderValue"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: paramItem.snapToLadder && paramItem.custom
                        text: {
                            var v = root.values[paramItem.name];
                            return v === undefined ? "" : root.formatPreset(v);
                        }
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                    }
                }

                Row {
                    spacing: 6
                    visible: paramItem.type === "choice"

                    Repeater {
                        model: paramItem.modelData.choices || []

                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root.values[paramItem.name] === modelData

                            height: 28
                            width: choiceText.width + 24
                            radius: Theme.radius
                            color: active ? Theme.fillSelected
                                : choiceMouse.containsMouse ? Theme.fillHover : "transparent"
                            border.width: 1
                            border.color: active ? Qt.rgba(1, 1, 1, 0.25) : Theme.stroke

                            Text {
                                id: choiceText
                                anchors.centerIn: parent
                                text: parent.modelData
                                color: parent.active ? Theme.textPrimary : Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                            }
                            MouseArea {
                                id: choiceMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.setValue(paramItem.name, parent.modelData)
                            }
                        }
                    }
                }

                Text {
                    visible: paramItem.type === "str" || paramItem.type === "path"
                    text: qsTr("该类型参数暂不支持编辑，执行时使用默认值")
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                }
            }
        }
    }
}
