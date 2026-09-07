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
// 【布局机制：Flickable + Column + Repeater，分组头是普通行】
// 模型 (ScriptListModel) 是扁平的 QAbstractListModel，分组信息只有 group
// role。旧版用 ListView.section 呈现分组，但 Qt 5.15 的 section delegate
// 高度必须恒定 —— 两级树的分组头高度是动态的（大类头/子分类头/全收），而
// ListView 对 section delegate 的高度变化不做重排：折叠空洞、criteria 换切
// 闪烁、动画期间交叠，三个版本的症状全是这一个根。所以分组头不再走
// section，降级成普通行：
//   rowList（行对象数组，只依赖模型）按模型行序展开成
//     {kind:"major"} 大类头 / {kind:"sub"} 子分类头 / {kind:"script"} 脚本行，
//   一个 Repeater 全量创建，折叠只驱动每行的 height/opacity（150ms）。
// 普通行的高度变化在 Column 位置器里每帧正确重排 —— 动画全程无交叠无
// 空洞，contentHeight 随之正确，不再需要任何重排补丁。行数是十几个量级，
// 全量创建 + 零高行不省的成本可以忽略。
//
// 【间隙烘进行高】Column 的 spacing 对零高行也生效，必须为 0；rowGap 和
// groupGap 照旧烘进各自行：脚本行 32+2、子分类头 28+4（大类头和首个子分类
// 头之间不留隙，与旧版一致）、无子分类的大类头 34+4（其后直接是脚本行）。
// 行收起时间隙份额随高度一并归零，不留残隙。
//
// 【折叠状态与 key 语义】（不变）
// expandedGroups 的 key 有两种：大类用第一段字符串（如 "Image"），子分类用
// 完整 group 串（如 "Image/Edit"）。大类 key 不含 "/"，与子分类 key 不会撞；
// 对不含 "/" 的组（"System"）两者天然是同一个 key，点一下即折整个组。
// 脚本行可见 = 大类展开 && 所属子分类（完整 group）展开。
//
// 【为什么折叠不动模型】折叠是纯视觉状态，不是数据；状态留在 QML 里，
// C++ 侧一行不用改。行数组只依赖模型，绝不依赖 expandedGroups —— 否则
// 每次折叠重建全部 delegate，悬停丢失、动画重放，闪烁全回来。

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

    /// 当前选中的脚本（绝对路径）。旧版用 ListView.currentIndex 记选中，
    /// 位置器里没有 currentIndex，改成按 filePath 身份判断；空串 = 无选中。
    property string selectedFilePath: ""

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

    // ---- 结构探针 ----
    // 模型是扁平列表，"前面有哪些分组/每个脚本长什么样"都要自己走一遍。
    // Repeater 把模型的每一行过一遍，delegate 是零尺寸不可见 Item，只负责
    // 把行数据摆成可读属性；行数十几个量级，开销可忽略。
    Repeater {
        id: groupProbe

        model: root.model

        delegate: Item {
            width: 0
            height: 0
            visible: false

            readonly property string probeGroup: model.group !== undefined ? model.group : ""
            readonly property string probeName: model.name !== undefined ? model.name : ""
            readonly property string probeFilePath: model.filePath !== undefined ? model.filePath : ""
            readonly property bool probeValid: model.valid === true
            readonly property bool probeDestructive: model.destructive === true
            readonly property string probeErrorText: model.errorText !== undefined ? model.errorText : ""
            readonly property string probeDestructiveReason: model.destructiveReason !== undefined ? model.destructiveReason : ""
            readonly property string probeDesc: model.desc !== undefined ? model.desc : ""
        }
    }

    // 行序 → section 树：major -> { total, sections, hasSubs }。
    //   sections = 该大类下的 section 串（行序去重后的出现顺序）；
    //   total    = 对这些 section 逐个求 groupCount 之和（大类折叠徽标）；
    //   hasSubs  = 是否存在子分类（决定大类头后面跟的是子分类头还是脚本行，
    //              也就决定了组间隙烘在哪一行）。
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
                tree[m] = { total: 0, sections: [], hasSubs: false };
            tree[m].sections.push(s);
            tree[m].total += root.model.groupCount(s);
            if (s !== m)
                tree[m].hasSubs = true;
        }
        return tree;
    }

    // 行对象数组：Repeater 的 model。只依赖模型（sectionTree + 探针数据），
    // 折叠状态不进来 —— 折叠只改行 delegate 自己的 height/opacity 绑定，
    // 行的身份永远稳定。JS 对象按插入序枚举字符串键，与模型行序一致。
    readonly property var rowList: {
        var rows = [];
        if (!root.model || !root.model.groupCount)
            return rows;
        var _dep = root.model.count; // 建立依赖（探针 delegate 重建时行数变）
        var n = groupProbe.count;
        var entries = [];
        for (var i = 0; i < n; ++i) {
            var e = groupProbe.itemAt(i);
            if (e)
                entries.push(e);
        }
        var tree = sectionTree;
        for (var major in tree) {
            var info = tree[major];
            rows.push({ kind: "major", key: major, label: major,
                        total: info.total, hasSubs: info.hasSubs });
            for (var j = 0; j < info.sections.length; ++j) {
                var g = info.sections[j];
                if (g !== major)
                    rows.push({ kind: "sub", key: g,
                                label: g.substring(g.indexOf("/") + 1) });
                for (var k = 0; k < entries.length; ++k) {
                    var p = entries[k];
                    if (p.probeGroup !== g)
                        continue;
                    rows.push({
                        kind: "script", key: g,
                        name: p.probeName, filePath: p.probeFilePath,
                        valid: p.probeValid, destructive: p.probeDestructive,
                        errorText: p.probeErrorText,
                        destructiveReason: p.probeDestructiveReason,
                        desc: p.probeDesc
                    });
                }
            }
        }
        return rows;
    }

    // ==== 视图 ====
    Flickable {
        id: listView
        // 给测试用的稳定锚点（名字沿用旧版 ListView）。
        objectName: "scriptListView"
        anchors.fill: parent
        // 滚动条只在需要时占位，不需要时把这几像素还给内容。
        anchors.rightMargin: root.scrollVisible ? Theme.scrollWidth + 2 : 0
        // 底部让位给 scanErrors 提示，不然滚动到底的脚本会被那段红字挡住。
        anchors.bottomMargin: errorFooter.visible ? errorFooter.height + 12 : 0

        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // 不设 contentWidth：默认从 childrenRect 取（Column 宽 = 视图宽），
        // 显式绑 width 反而构成 contentWidth ↔ width 绑定环。
        // 不 clamp contentHeight 到视图高度：全收起时内容比视图矮，
        // 测试要读到真实内容高。
        contentHeight: rowsColumn.height

        Column {
            id: rowsColumn
            objectName: "rowsColumn"
            width: listView.width
            // spacing 必须为 0：Column 的 spacing 对零高行也生效，间隙全部
            // 烘进行高（见文件头【间隙烘进行高】）。
            spacing: 0

            Repeater {
                model: root.rowList

                delegate: Loader {
                    required property var modelData

                    width: rowsColumn.width
                    // 行根节点用显式 height 绑定（非 implicit），Loader 不会
                    // 自动跟随，这里手动接一次；Column 按 Loader 高度排版，
                    // 高度动画每帧传导、逐帧重排。
                    height: item ? item.height : 0
                    sourceComponent: modelData.kind === "major" ? majorComp
                                   : modelData.kind === "sub" ? subComp
                                   : scriptComp
                }
            }

            // 底部留一点呼吸空间，让最后一行不贴着渐隐遮罩（旧 ListView footer）。
            Item { objectName: "listFooter"; width: 1; height: Theme.fadeHeight }
        }
    }

    // ==== 大类头行 ====
    Component {
        id: majorComp

        Item {
            id: majorRow
            objectName: "majorHeader"
            // Loader 的 modelData 不在组件的普通作用域链上，经 parent 转发。
            readonly property var modelData: parent.modelData
            // 测试锚点：behaviour.py 据此断言标题/徽标/高度。
            readonly property string rowKey: modelData.key
            readonly property string rowTitle: modelData.label
            readonly property real rowHeight: height
            readonly property string rowBadge: majorCount.text
            readonly property bool rowBadgeVisible: majorCount.visible

            width: parent.width
            // 无子分类的大类（System）头后直接是脚本行，组间隙烘在这里；
            // 有子分类的头后紧跟子分类头，不留隙（旧版分组头同样的分账）。
            height: Theme.rowGroupHeight + (modelData.hasSubs ? 0 : Theme.groupGap)
            clip: true

            Rectangle {
                id: majorBg
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: Theme.rowGroupHeight
                radius: Theme.radius
                color: rowMouse.containsMouse ? Theme.fillHover : "transparent"

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
                        visible: !rowMouse.containsMouse
                    }

                    Glyph {
                        anchors.fill: parent
                        kind: Glyph.Kind.Chevron
                        color: Theme.textSecondary
                        visible: rowMouse.containsMouse
                        // 展开时箭头指下。dsh 是 rotate(90deg) 配 150ms。
                        rotation: root.isExpanded(modelData.key) ? 90 : 0
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
                    text: modelData.label
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    font.bold: true
                    elide: Text.ElideRight
                }

                // 折叠后看不见成员，给个数量提示：该大类全部脚本数。
                Text {
                    id: majorCount
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.rowPadding
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.total
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontMicro
                    visible: !root.isExpanded(modelData.key)
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    // 大类头折整个大类（key = 第一段）。
                    onClicked: root.toggleGroup(modelData.key)
                }
            }
        }
    }

    // ==== 子分类头行 ====
    Component {
        id: subComp

        Item {
            id: subRoot
            objectName: "subHeader"
            // Loader 的 modelData 不在组件的普通作用域链上，经 parent 转发。
            readonly property var modelData: parent.modelData
            readonly property string rowKey: modelData.key
            readonly property string rowTitle: modelData.label
            readonly property real rowHeight: height
            readonly property string rowBadge: badgeText.text
            readonly property bool rowBadgeVisible: badgeText.visible

            width: parent.width
            // 所属大类折叠时高度归零（150ms）；子分类自身的折叠只收脚本行、
            // 这行保留。间隙份额（组间隙）随高度一并归零。
            height: root.isExpanded(root.groupMajor(modelData.key))
                ? Theme.rowSubGroupHeight + Theme.groupGap : 0
            // 高度归零时内容是居中锚定的，不 clip 会探进上一行可视带
            // （实测：文本 y=25..43 对可视带 0..38）。零高度 MouseArea
            // 不参与命中测试，鼠标天然安全。
            clip: true

            Behavior on height {
                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: Theme.rowSubGroupHeight
                radius: Theme.radius
                color: rowMouse.containsMouse ? Theme.fillHover : "transparent"

                Behavior on color {
                    ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                }

                // 文字/图标不全程参与显现：高度动画是裁切式（clip 自上而下揭开），
                // 文字若跟着裁切线一起露出来，会被切成半截、切边紧贴下一行，
                // 视觉上读成"文字叠进相邻行"（真速实拍确认）。改为行高长到
                // 最后 10px 区间才随高度线性淡入（此时文字已完整落在可视带内），
                // 收起方向对称、文字先走。纯高度函数，与动画时长无关；
                // 背板不参与门控，裁切揭开本身仍是可见的运动反馈。
                Item {
                    id: subContent
                    anchors.fill: parent
                    opacity: Math.max(0, Math.min(1,
                        (subRoot.height - (Theme.rowSubGroupHeight + Theme.groupGap - 10)) / 8))

                    // ---- 图标槽 ----
                    // 常驻一个小箭头，不换文件夹 —— 它不是目录入口，是筛选开关，
                    // 箭头要始终指明"这行能折叠"。尺寸沿用 16px 图标槽，颜色压暗
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
                            rotation: root.isExpanded(modelData.key) ? 90 : 0
                            Behavior on rotation {
                                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
                            }
                        }
                    }

                    Text {
                        anchors.left: subIcon.right
                        anchors.leftMargin: Theme.iconGap
                        anchors.right: badgeText.left
                        anchors.rightMargin: Theme.iconGap
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }

                    // 折叠后看不见成员，给个数量提示（本子分类的成员数）。
                    Text {
                        id: badgeText
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.rowPadding
                        anchors.verticalCenter: parent.verticalCenter
                        text: (root.model && root.model.groupCount)
                            ? root.model.groupCount(modelData.key) : ""
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontMicro
                        visible: !root.isExpanded(modelData.key)
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    // 子分类头只折自己（key = 完整 group 串）。
                    onClicked: root.toggleGroup(modelData.key)
                }
            }
        }
    }

    // ==== 单条脚本 ====
    Component {
        id: scriptComp

        Item {
            id: delegateRoot
            objectName: "scriptRow"
            // Loader 的 modelData 不在组件的普通作用域链上，经 parent 转发。
            readonly property var modelData: parent.modelData
            // 测试锚点：behaviour.py 的几何探针据此定位脚本行并读取它
            // 所属的 group（折叠后 h=0 的行不占位，靠这个名字区分 footer）。
            readonly property string rowKey: modelData.key
            readonly property string rowFilePath: modelData.filePath

            readonly property bool isValid: modelData.valid === true
            readonly property bool isDestructive: modelData.destructive === true
            // 两级折叠的门：大类（第一段）与子分类（完整 group 串）都展开
            // 才可见。不含 "/" 的组两个 key 相同，天然一次折叠生效。
            readonly property bool inExpandedGroup:
                root.isExpanded(root.groupMajor(modelData.key))
                && root.isExpanded(modelData.key)
            readonly property bool isCurrent: isValid
                && root.selectedFilePath === modelData.filePath

            width: parent.width
            // 折叠时零高度。clip 是必须的 —— 高度动画到 0 的过程中内容会
            // 溢出到相邻行上面。
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

                // 文字/图标不全程参与显现：高度动画是裁切式（clip 自上而下揭开），
                // 文字若跟着裁切线一起露出来，会被切成半截、切边紧贴下一行，
                // 视觉上读成"文字叠进相邻行"（真速实拍确认）。改为行高长到
                // 最后 10px 区间才随高度线性淡入（此时文字已完整落在可视带内），
                // 收起方向对称、文字先走。纯高度函数，与动画时长无关；
                // 背板不参与门控，裁切揭开本身仍是可见的运动反馈。
                Item {
                    id: rowContent
                    anchors.fill: parent
                    opacity: Math.max(0, Math.min(1,
                        (delegateRoot.height - (Theme.rowItemHeight + Theme.rowGap - 10)) / 8))

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
                        text: modelData.name
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
                            root.hoverDesc = modelData.errorText;
                        else if (delegateRoot.isDestructive)
                            root.hoverDesc = modelData.destructiveReason;
                        else
                            root.hoverDesc = modelData.desc;
                    }
                    // 只清自己写的那份：鼠标从 A 直接移到 B 时，B 的 onEntered
                    // 可能先于 A 的 onExited 触发，无条件清空会把 B 刚写的擦掉。
                    onExited: {
                        if (root.hoverDesc === modelData.desc
                                || root.hoverDesc === modelData.errorText
                                || root.hoverDesc === modelData.destructiveReason)
                            root.hoverDesc = "";
                    }

                    onClicked: {
                        // 无效脚本点击不发信号，也不进入选中态 —— 它本来就不可选中。
                        if (!delegateRoot.isValid)
                            return;
                        root.selectedFilePath = modelData.filePath;
                        root.scriptSelected(modelData.filePath, modelData.name);
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
    // Rectangle 按 Flickable.visibleArea 算位置和高度（Flickable 与 ListView
    // 同一套 visibleArea API，这份代码原样沿用）。
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
    // 而不是走视图 delegate。非空才显示，避免空面板占地方。
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
