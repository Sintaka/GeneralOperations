// 脚本 Outliner：左侧面板里的可折叠脚本树。
//
// 【结构照 dsh (deepseek-harness) 的 WorkspaceBrowser】
// 分组头 34px 可点击折叠，脚本行 32px，8px 圆角，行间距 2px，组间距 4px，
// 悬停与选中共用同一个填充色，行尾操作图标只在悬停时出现，
// 折叠箭头 150ms 旋转，行展开时 150ms 淡入。
// 配色走鹰角（见 Theme.qml），所以这里不写死任何颜色/时长/圆角。
//
// 【折叠是怎么做的，以及为什么这么做】
// 模型 (ScriptListModel) 是扁平的 QAbstractListModel，只有一个 group role，
// 分组靠 ListView.section 呈现 —— 模型里没有"分组头"这种假行。
// 要折叠就得让某些行不占位，做法是把 delegate 的 height 设为 0 且 visible
// 设为 false，而不是改模型。
//
// 这样选的原因：折叠是纯视觉状态，不是数据。放进 C++ 模型意味着
// 每次展开/收起都要 beginRemoveRows/endInsertRows，模型要额外维护
// "可见行 → 真实行"的映射，而 QML 侧只是想少画几行而已。
// 状态留在 QML 里，C++ 侧一行不用改。
//
// 代价是被折叠的 delegate 仍然存在（只是零高度），所以行数极多时
// 省不掉 delegate 的创建成本。脚本总量是十几个量级，无所谓。

import QtQuick 2.15
import App 1.0

Item {
    id: root

    // 外部注入 ScriptListModel 实例（或任何 role 名匹配的模型）。
    property var model

    signal scriptSelected(string filePath, string name)

    /// 当前悬停行的说明文字，显示在列表底部（见文件末尾 hoverHint）。
    /// 由行的 MouseArea 进出时写入。
    property string hoverDesc: ""

    // ---- 折叠状态 ----
    // key = 分组名，value = 是否展开。缺省视为展开。
    //
    // 【为什么要整体重新赋值】QML 的属性绑定只在属性本身变化时重新求值。
    // 对 var 里的 JS 对象做 obj[k] = v 是原地修改，属性引用没变，
    // 不会触发任何依赖它的绑定 —— 界面不会更新。
    // 所以 toggle 里必须构造一个新对象再整体赋回去。
    property var expandedGroups: ({})

    function isExpanded(group) {
        return expandedGroups[group] !== false;
    }

    function toggleGroup(group) {
        var next = {};
        for (var k in expandedGroups)
            next[k] = expandedGroups[k];
        next[group] = !isExpanded(group);
        expandedGroups = next;
    }

    ListView {
        id: listView
        // 给测试用的稳定锚点。QML 的 id 只在声明它的文件里可见，
        // 外部（测试脚本）要定位这个 ListView 只能靠 objectName。
        objectName: "scriptListView"
        anchors.fill: parent
        // 滚动条只在需要时占位，不需要时把这几像素还给内容。
        anchors.rightMargin: root.scrollVisible ? Theme.scrollWidth + 2 : 0
        // 底部让位给 scanErrors 提示，不然滚动到底的脚本会被那段红字挡住。
        anchors.bottomMargin: errorFooter.visible ? errorFooter.height + 12 : 0

        model: root.model
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // 【必须显式设 -1】ListView.currentIndex 默认是 0，于是启动时
        // 第一个脚本会显示成选中态，而 scriptSelected 信号并没有发出 ——
        // 右侧面板还停在"从左侧选一个脚本"。左边看着已选、右边说没选，
        // 两边状态不一致。-1 表示"无选中"。
        currentIndex: -1

        // 同理关掉自动高亮：我们的选中态是 delegate 自己按 isCurrent 画的
        // （见 rowBg.color），ListView 内建的 highlight 会在它下面再画一层。
        highlight: null
        highlightFollowsCurrentItem: false

        section.property: "group"
        section.criteria: ViewSection.FullString
        section.delegate: sectionDelegate

        delegate: itemDelegate

        // 底部留一点呼吸空间，让最后一行不贴着渐隐遮罩。
        footer: Item { width: 1; height: Theme.fadeHeight }
    }

    // ==== 分组头 ====
    Component {
        id: sectionDelegate

        Item {
            id: sectionRoot
            width: listView.width
            // 组间距：靠分组头自己的上边距实现，不用 spacing ——
            // ListView 的 spacing 会同时作用于普通行，那样行间距就不是 2px 了。
            height: Theme.rowGroupHeight + Theme.groupGap

            readonly property bool expanded: root.isExpanded(section)

            Rectangle {
                id: sectionBg
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.rowGroupHeight
                radius: Theme.radius
                color: sectionMouse.containsMouse ? Theme.fillHover : "transparent"

                Behavior on color {
                    ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                }

                // ---- 图标槽 ----
                // dsh 的行为：默认显示文件夹，悬停时换成折叠箭头。
                // 两个图标占同一个 16px 槽，靠 visible 互斥切换。
                Item {
                    id: sectionIcon
                    width: Theme.iconSlot
                    height: Theme.iconSlot
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.rowPadding
                    anchors.verticalCenter: parent.verticalCenter

                    Glyph {
                        anchors.fill: parent
                        kind: Glyph.Kind.Folder
                        color: Theme.textTertiary
                        visible: !sectionMouse.containsMouse
                    }

                    Glyph {
                        anchors.fill: parent
                        kind: Glyph.Kind.Chevron
                        color: Theme.textSecondary
                        visible: sectionMouse.containsMouse
                        // 展开时箭头指下。dsh 是 rotate(90deg) 配 150ms。
                        rotation: sectionRoot.expanded ? 90 : 0
                        Behavior on rotation {
                            NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                        }
                    }
                }

                Text {
                    anchors.left: sectionIcon.right
                    anchors.leftMargin: Theme.iconGap
                    anchors.right: sectionCount.left
                    anchors.rightMargin: Theme.iconGap
                    anchors.verticalCenter: parent.verticalCenter
                    text: section
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    font.bold: true
                    elide: Text.ElideRight
                }

                // 折叠后看不见成员，给个数量提示，不然收起来就完全没信息了。
                Text {
                    id: sectionCount
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.rowPadding
                    anchors.verticalCenter: parent.verticalCenter
                    // groupCount 是模型的 Q_INVOKABLE。这里没有 model.count
                    // 那样的依赖问题 —— section 变化本身就会让绑定重算，
                    // 而模型重载时整个 delegate 会被重建。
                    text: (root.model && root.model.groupCount)
                        ? root.model.groupCount(section) : ""
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    visible: !sectionRoot.expanded
                }

                MouseArea {
                    id: sectionMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggleGroup(section)
                }
            }
        }
    }

    // ==== 单条脚本 ====
    Component {
        id: itemDelegate

        Item {
            id: delegateRoot
            width: listView.width

            readonly property bool isValid: model.valid === true
            readonly property bool isDestructive: model.destructive === true
            readonly property bool inExpandedGroup: root.isExpanded(model.group)
            readonly property bool isCurrent: isValid && listView.currentIndex === index

            // 折叠时零高度且不可见。clip 是必须的 —— 高度动画到 0 的过程中
            // 内容会溢出到相邻行上面。
            height: inExpandedGroup ? Theme.rowItemHeight + Theme.rowGap : 0
            visible: height > 0
            clip: true

            Behavior on height {
                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
            }

            // dsh 的 row-in：展开时淡入。
            opacity: inExpandedGroup ? 1 : 0
            Behavior on opacity {
                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
            }

            Rectangle {
                id: rowBg
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: Theme.rowItemHeight
                radius: Theme.radius

                // dsh 的 selected 和 hover 是同一族填充色，选中更亮一档。
                color: delegateRoot.isCurrent
                    ? Theme.fillSelected
                    : (rowMouse.containsMouse ? Theme.fillHover : "transparent")

                Behavior on color {
                    ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                }

                // ---- 状态槽 ----
                // 缩进就靠这个 16px 槽对齐到分组头的文件夹图标下方
                // （dsh 注释里的 "indent step 22px = 16px slot + 6px gap"）。
                Item {
                    id: rowIcon
                    width: Theme.iconSlot
                    height: Theme.iconSlot
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.rowPadding
                    anchors.verticalCenter: parent.verticalCenter

                    // 破坏性脚本：警告标记。这是安全相关的提示，
                    // 必须常驻，不能只在悬停时出现。
                    Glyph {
                        anchors.fill: parent
                        kind: Glyph.Kind.Bang
                        color: Theme.danger
                        visible: delegateRoot.isValid && delegateRoot.isDestructive
                    }

                    // 无效脚本：也用感叹号，但灰色 —— 它不是危险，是不可用。
                    Glyph {
                        anchors.fill: parent
                        kind: Glyph.Kind.Bang
                        color: Theme.textTertiary
                        visible: !delegateRoot.isValid
                    }
                }

                Text {
                    id: rowTitle
                    anchors.left: rowIcon.right
                    anchors.leftMargin: Theme.iconGap
                    anchors.right: rowActions.left
                    anchors.rightMargin: Theme.iconGap
                    anchors.verticalCenter: parent.verticalCenter
                    text: model.name !== undefined ? model.name : ""
                    // 无效脚本灰掉（不是隐藏）。破坏性脚本标题不染色 ——
                    // 左边已经有红色感叹号了，标题再染一遍就过了。
                    color: delegateRoot.isValid ? Theme.textPrimary : Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                }

                // ---- 行尾操作 ----
                // dsh：只在悬停/菜单打开时出现。这里先放一个"更多"，
                // 具体菜单是后续切片的事。
                Row {
                    id: rowActions
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.rowPadding
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.actionGap
                    visible: rowMouse.containsMouse && delegateRoot.isValid

                    Glyph {
                        width: Theme.iconSlot
                        height: Theme.iconSlot
                        kind: Glyph.Kind.Ellipsis
                        color: Theme.textTertiary
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: delegateRoot.isValid ? Qt.PointingHandCursor : Qt.ArrowCursor

                    // 底部提示行的内容。无效脚本显示错误原因，破坏性脚本
                    // 显示破坏性原因（比 desc 重要），其余显示 desc。
                    onEntered: {
                        if (!delegateRoot.isValid)
                            root.hoverDesc = model.errorText !== undefined ? model.errorText : "";
                        else if (delegateRoot.isDestructive)
                            root.hoverDesc = model.destructiveReason !== undefined ? model.destructiveReason : "";
                        else
                            root.hoverDesc = model.desc !== undefined ? model.desc : "";
                    }
                    // 只清自己写的那份：鼠标从 A 直接移到 B 时，B 的 onEntered
                    // 可能先于 A 的 onExited 触发，无条件清空会把 B 刚写的擦掉。
                    onExited: {
                        if (root.hoverDesc === model.desc
                                || root.hoverDesc === model.errorText
                                || root.hoverDesc === model.destructiveReason)
                            root.hoverDesc = "";
                    }

                    onClicked: {
                        // 无效脚本点击不发信号，也不进入选中态 —— 它本来就不可选中。
                        if (!delegateRoot.isValid)
                            return;
                        listView.currentIndex = index;
                        root.scriptSelected(model.filePath, model.name);
                    }
                }
            }
        }
    }

    // ==== 悬停说明 ====
    // dsh 把描述放在 hover card 里。这里简化成面板底部一行 ——
    // 上一版是把 desc 塞进行内副标题，导致行高在 36/52px 之间跳变，
    // 悬停时整个列表会重排。挪到固定位置后行高恒定。
    Text {
        id: hoverHint
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: errorFooter.visible ? errorFooter.top : parent.bottom
        anchors.bottomMargin: 4
        text: root.hoverDesc
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontMicro
        elide: Text.ElideRight
        visible: text.length > 0
    }

    // ==== 滚动条 ====
    // QtQuick 基础库没有 ScrollBar（那是 Controls 里的），自己用一条细
    // Rectangle 按 Flickable.visibleArea 算位置和高度。
    // 尺寸配色走鹰角原值：4px 宽、3px 圆角、白 25%、悬停白 50%。
    readonly property bool scrollVisible: listView.visibleArea.heightRatio < 1.0

    Rectangle {
        visible: root.scrollVisible
        width: Theme.scrollWidth
        radius: Theme.scrollRadius
        color: scrollHover.containsMouse ? Theme.scrollThumbHover : Theme.scrollThumb
        anchors.right: parent.right
        y: listView.y + listView.visibleArea.yPosition * listView.height
        height: Math.max(24, listView.visibleArea.heightRatio * listView.height)

        Behavior on color {
            ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
        }

        MouseArea {
            id: scrollHover
            // 4px 宽的条鼠标很难压上去，热区往两边各放宽 4px。
            anchors.fill: parent
            anchors.margins: -4
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
    }

    // ==== 底部扫描错误 ====
    // scanErrors() 是 Q_INVOKABLE，不是 role，所以这里直接调用模型方法，
    // 而不是走 ListView 的 delegate。非空才显示，避免空面板占地方。
    Column {
        id: errorFooter
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 2

        // scanErrors() 是方法调用，QML 的属性绑定只追踪它读到的属性，
        // 追踪不到方法返回值的变化。这里显式读一下 model.count 建立
        // 依赖：load() 每次扫描后都会 emit countChanged()，
        // 靠这个信号触发本绑定重新求值，间接带动 scanErrors() 重新调用。
        readonly property var errs: {
            if (!root.model || !root.model.scanErrors)
                return [];
            var _dep = root.model.count; // 建立依赖，本身不使用
            return root.model.scanErrors();
        }

        visible: errs.length > 0

        Repeater {
            model: errorFooter.errs

            Text {
                width: errorFooter.width
                text: modelData
                color: Theme.danger
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontMicro
                wrapMode: Text.WordWrap
            }
        }
    }
}
