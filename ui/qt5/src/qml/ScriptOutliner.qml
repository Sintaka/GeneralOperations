// 脚本 Outliner：左侧面板里的两级可折叠脚本树（大类 / 子分类 / 脚本行）。
//
// 【结构照 dsh (deepseek-harness) 的 WorkspaceBrowser】
// 大类头 34px 可点击折叠，脚本行 32px，8px 圆角，行间距 2px，组间距 4px，
// 悬停与选中共用同一个填充色，行尾操作图标只在悬停时出现，
// 折叠箭头 150ms 旋转，行展开时 150ms 淡入。
// 子分类头 28px 是本仓库在 dsh 之上新增的层级（Theme.rowSubGroupHeight），
// 刻意比大类头弱一档：不加粗、次级文字色、缩进对齐脚本行的图标槽。
// 配色走鹰角（见 Theme.qml），所以这里不写死任何颜色/时长/圆角。
//
// 【两级树是怎么做的 —— 两级头从同一个 section delegate 里分叉】
// 模型 (ScriptListModel) 是扁平的 QAbstractListModel，只有一个 group role，
// group 是脚本的目录路径（"Image/Edit"、"System"……），模型里没有
// "分组头"这种假行。分组靠 ListView.section（"group" + FullString）呈现：
// 每个唯一的 group 串一个 section delegate。于是约定：
//   大类   = group 第一段（"/" 之前），如 "Image"；
//   子分类 = 第二段，如 "Edit"；
//   不含 "/" 的 group（根目录脚本，"System"）本身就是大类，没有子分类层。
//
// 一个 section delegate 渲染 1~2 行（Column 布局）：
//   - 大类头行（34px，加粗，folder/chevron 悬停互换）：只由该大类的【第一个】
//     section 承担（"大类 carrier"）；
//   - 子分类头行（28px，常驻小箭头，可整行点击）：凡是含 "/" 的 group 都渲染
//     自己这一行。于是第一个 section 的 delegate 同时给出大类头 + 它自己的
//     子分类头 —— Image/Edit 槽位上先画 "Image" 再画 "Edit"，
//     大类下的三个子分类（Edit / Format Convert / ReSize）一个不缺。
// 分叉依据是"前面有没有同大类的 section"。Qt 6 有 ViewSection.previousSections
// /nextSections 可以直接回答，Qt 5.15 没有 —— 实测 5.15.2 的 section delegate
// 上 ViewSection.* 一律 undefined，attached 对象上只有 section /
// previousSection / nextSection，且在 section delegate 上三者恒为空串
// （previousSection 只在 item delegate 上有值，帮不上忙）。
// 所以 root 上放了一个零尺寸 Repeater 探针把模型的行序走一遍，自己推导出
// sectionTree（每个大类：有序 section 列表 + 脚本总数），见下方注释。
// 模型行序即 section 序：ScriptRegistry 已按相对路径字典序排序，
// 同组连续、同大类连续；行序去重后与 ListView 实际生成的 section 序一致。
//
// 【折叠状态与 key 语义】
// expandedGroups 的 key 有两种：大类用第一段字符串（如 "Image"），子分类用
// 完整 group 串（如 "Image/Edit"）。大类 key 不含 "/"，与子分类 key 不会撞；
// 对不含 "/" 的组（"System"）两者天然是同一个 key，点一下即折整个组。
// 脚本行可见 = 大类展开 && 所属子分类（完整 group）展开。
//
// 【为什么折叠不动模型】
// 折叠是纯视觉状态，不是数据。放进 C++ 模型意味着每次展开/收起都要
// beginRemoveRows/endInsertRows，模型要额外维护"可见行 → 真实行"的映射，
// 而 QML 侧只是想少画几行而已。状态留在 QML 里，C++ 侧一行不用改。
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
    // key = 大类名（第一段）或完整子分类 group 串，value = 是否展开。
    // 缺省视为展开。语义见文件头【折叠状态与 key 语义】。
    //
    // 【为什么要整体重新赋值】QML 的属性绑定只在属性本身变化时重新求值。
    // 对 var 里的 JS 对象做 obj[k] = v 是原地修改，属性引用没变，
    // 不会触发任何依赖它的绑定 —— 界面不会更新。
    // 所以 toggle 里必须构造一个新对象再整体赋回去。
    property var expandedGroups: ({})

    function isExpanded(key) {
        return expandedGroups[key] !== false;
    }

    function toggleGroup(key) {
        var next = {};
        for (var k in expandedGroups)
            next[k] = expandedGroups[k];
        next[key] = !isExpanded(key);
        expandedGroups = next;
    }

    /// group 的第一段（"/" 之前）；不含 "/" 时就是 group 本身。
    /// 这就是 expandedGroups 里的"大类 key"，也是大类头的显示文本。
    function groupMajor(group) {
        var i = group.indexOf("/");
        return i >= 0 ? group.substring(0, i) : group;
    }

    // ---- section 结构探针 ----
    // Qt 5.15 的 section delegate 只拿得到当前 section 字符串，拿不到
    // "前面还有哪些 section"（见文件头）。这里用 Repeater 把模型的每一行
    // 过一遍、只读 group role，行序去重即 section 序。delegate 是零尺寸
    // 不可见 Item，不参与布局；行数十几个量级，开销可忽略。
    Repeater {
        id: groupProbe

        model: root.model

        delegate: Item {
            width: 0
            height: 0
            visible: false

            readonly property string probeGroup: model.group !== undefined ? model.group : ""
        }
    }

    // 行序 → section 树：major -> { total, sections }。
    //   sections = 该大类下的 section 串（行序去重后的出现顺序）；
    //   total    = 对这些 section 逐个求 groupCount 之和（大类折叠徽标
    //              要显示的总数，等价于"自身 + 前后同大类 section"求和）。
    // 依赖两处：groupProbe.count（行数变化时重算）、root.model.count
    // （reload 时行数可能不变但分组变了，靠 load() 每次扫描后 emit 的
    // countChanged 触发重算 —— 与文件末尾 errorFooter 同一套做法）。
    readonly property var sectionTree: {
        var tree = {};
        if (!root.model || !root.model.groupCount)
            return tree;
        var sections = [];
        var seen = {};
        var n = groupProbe.count;
        for (var i = 0; i < n; ++i) {
            var obj = groupProbe.itemAt(i);
            var g = obj ? obj.probeGroup : "";
            if (g === "" || seen[g])
                continue;
            seen[g] = true;
            sections.push(g);
        }
        var _dep = root.model.count; // 建立依赖，本身不使用
        for (i = 0; i < sections.length; ++i) {
            var s = sections[i];
            var m = root.groupMajor(s);
            if (!tree[m])
                tree[m] = { total: 0, sections: [] };
            tree[m].sections.push(s);
            tree[m].total += root.model.groupCount(s);
        }
        return tree;
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

    // ==== 分组头（大类头行 + 子分类头行，见文件头）====
    Component {
        id: sectionDelegate

        Item {
            id: sectionRoot
            width: listView.width

            // ---- 测试锚点：behaviour.py 据此断言两行的标题/徽标/高度 ----
            objectName: "sectionHeader"
            readonly property string headerSection: section
            readonly property string majorRowTitle: majorRow.visible ? major : ""
            readonly property real majorRowHeight: majorRow.visible ? majorRow.height : 0
            readonly property string majorRowBadge: majorRow.visible ? majorCount.text : ""
            readonly property bool majorRowBadgeVisible: majorRow.visible && majorCount.visible
            readonly property string subRowTitle: subRow.visible ? subLabel : ""
            readonly property real subRowHeight: subRow.visible ? subRow.height : 0
            readonly property string subRowBadge: subRow.visible ? subCount.text : ""
            readonly property bool subRowBadgeVisible: subRow.visible && subCount.visible

            // ---- 本 delegate 承担哪几行 ----
            readonly property string major: root.groupMajor(section)
            readonly property bool hasSub: section.indexOf("/") >= 0
            readonly property string subLabel: hasSub ? section.substring(section.indexOf("/") + 1) : ""
            // 本大类的 sectionTree 条目；查不到时按"大类 carrier"兜底 ——
            // 宁可重复画大类头，也不能把整个大类藏没了。
            readonly property var majorInfo: root.sectionTree[major]
            // 大类 carrier：该大类的第一个 section。只有它渲染大类头行。
            readonly property bool isMajorCarrier: majorInfo === undefined
                || majorInfo.sections.length === 0 || majorInfo.sections[0] === section

            readonly property bool majorExpanded: root.isExpanded(major)
            readonly property bool subExpanded: root.isExpanded(section)

            // 组间距：靠分组头自己的底部留白实现，不用 spacing ——
            // ListView 的 spacing 会同时作用于普通行，那样行间距就不是 2px 了。
            // 两行全为零时 delegate 必须真正归零（子分类头随大类收起、非
            // carrier 的子分类 delegate），否则收起的大类会留下 4px 空隙。
            height: {
                var rows = (majorRow.visible ? majorRow.height : 0)
                    + (subRow.visible ? subRow.height : 0);
                return rows > 0 ? rows + Theme.groupGap : 0;
            }
            // 高度动画到 0 的过程中内容会溢出到相邻行上面，clip 是必须的。
            clip: true

            Column {
                anchors.left: parent.left
                anchors.right: parent.right

                // ---- 大类头行 ----
                // 只有 carrier 渲染。高度恒定，carrier 身份只在模型 reload
                // 时才会变（delegate 随之重建），不需要动画。
                Rectangle {
                    id: majorRow
                    visible: sectionRoot.isMajorCarrier
                    width: parent.width
                    height: Theme.rowGroupHeight
                    radius: Theme.radius
                    color: majorMouse.containsMouse ? Theme.fillHover : "transparent"

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
                            visible: !majorMouse.containsMouse
                        }

                        Glyph {
                            anchors.fill: parent
                            kind: Glyph.Kind.Chevron
                            color: Theme.textSecondary
                            visible: majorMouse.containsMouse
                            // 展开时箭头指下。dsh 是 rotate(90deg) 配 150ms。
                            rotation: sectionRoot.majorExpanded ? 90 : 0
                            Behavior on rotation {
                                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                            }
                        }
                    }

                    Text {
                        anchors.left: sectionIcon.right
                        anchors.leftMargin: Theme.iconGap
                        anchors.right: majorCount.left
                        anchors.rightMargin: Theme.iconGap
                        anchors.verticalCenter: parent.verticalCenter
                        text: sectionRoot.major
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    // 折叠后看不见成员，给个数量提示：该大类所有 section 的
                    // groupCount 之和（sectionTree 已算好，reload 依赖也在
                    // 那里建立）。
                    Text {
                        id: majorCount
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.rowPadding
                        anchors.verticalCenter: parent.verticalCenter
                        text: sectionRoot.majorInfo !== undefined ? sectionRoot.majorInfo.total : 0
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontMicro
                        visible: !sectionRoot.majorExpanded
                    }

                    MouseArea {
                        id: majorMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // 大类头折整个大类（key = 第一段）。
                        onClicked: root.toggleGroup(sectionRoot.major)
                    }
                }

                // ---- 子分类头行 ----
                // 凡是含 "/" 的 group 都渲染自己这一行（carrier 的 delegate
                // 排在大类头行之后）。所属大类折叠时高度归零（150ms，与脚本
                // 行同一套 Behavior），子分类自身的折叠只收脚本行、这行保留。
                Rectangle {
                    id: subRow
                    visible: sectionRoot.hasSub
                    width: parent.width
                    // 高度绑定带 hasSub 门：visible 只决定"参不参与布局"，
                    // 不挡这行自己的高度值 —— 少了这个门，无子分类的大类
                    // （System）收起时 delegate 会白占 28px。
                    height: sectionRoot.hasSub && sectionRoot.majorExpanded
                        ? Theme.rowSubGroupHeight : 0
                    radius: Theme.radius
                    color: subMouse.containsMouse ? Theme.fillHover : "transparent"

                    Behavior on height {
                        NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                    }

                    Behavior on color {
                        ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                    }

                    // ---- 图标槽 ----
                    // 常驻一个小箭头，不换文件夹 —— 它不是目录入口，
                    // 是筛选开关，箭头要始终指明"这行能折叠"。尺寸沿用 16px
                    // 图标槽（规格允许 12~16px，取与行图标同槽对齐），颜色压暗
                    // 一档与大类头的悬停箭头区分层级。
                    Item {
                        id: subIcon
                        width: Theme.iconSlot
                        height: Theme.iconSlot
                        anchors.left: parent.left
                        // 缩进对齐到脚本行的图标槽之后（标题与脚本行同 x）。
                        anchors.leftMargin: Theme.rowPadding + Theme.iconSlot + Theme.iconGap
                        anchors.verticalCenter: parent.verticalCenter

                        Glyph {
                            anchors.fill: parent
                            kind: Glyph.Kind.Chevron
                            color: Theme.textTertiary
                            rotation: sectionRoot.subExpanded ? 90 : 0
                            Behavior on rotation {
                                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                            }
                        }
                    }

                    Text {
                        anchors.left: subIcon.right
                        anchors.leftMargin: Theme.iconGap
                        anchors.right: subCount.left
                        anchors.rightMargin: Theme.iconGap
                        anchors.verticalCenter: parent.verticalCenter
                        text: sectionRoot.subLabel
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }

                    // 折叠后看不见成员，给个数量提示（本子分类的成员数）。
                    Text {
                        id: subCount
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.rowPadding
                        anchors.verticalCenter: parent.verticalCenter
                        text: (root.model && root.model.groupCount)
                            ? root.model.groupCount(section) : ""
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontMicro
                        visible: !sectionRoot.subExpanded
                    }

                    MouseArea {
                        id: subMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // 子分类头只折自己（key = 完整 group 串）。
                        onClicked: root.toggleGroup(section)
                    }
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
            // 两级折叠的门：大类（第一段）与子分类（完整 group 串）都展开
            // 才可见。不含 "/" 的组两个 key 相同，天然一次折叠生效。
            readonly property string rowGroup: model.group !== undefined ? model.group : ""
            readonly property bool inExpandedGroup:
                root.isExpanded(root.groupMajor(rowGroup)) && root.isExpanded(rowGroup)
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
                // （dsh 注释里的 "indent step 22px = 16px slot + 6px gap"；
                // 子分类头的缩进同样对齐到这里）。
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
