import QtQuick 2.15
import App 1.0

Rectangle {
    id: root
    objectName: "commandOutput"

    property string text: ""
    property bool followTail: true

    readonly property bool hasSelection: outputEdit.selectionStart !== outputEdit.selectionEnd
    readonly property real maximumContentY: Math.max(0, outputView.contentHeight - outputView.height)
    readonly property bool atBottom: maximumContentY - outputView.contentY <= 1
    readonly property bool scrollVisible: outputView.visibleArea.heightRatio < 1.0

    property bool followScheduled: false
    property bool settingContentY: false
    property bool syncingText: false

    implicitHeight: 132
    radius: Theme.radius
    color: Theme.surfacePop
    border.width: 1
    border.color: Theme.stroke
    clip: true

    function syncText() {
        var start = outputEdit.selectionStart
        var end = outputEdit.selectionEnd
        var selected = start !== end
        syncingText = true
        outputEdit.text = text
        if (selected)
            outputEdit.select(Math.min(start, text.length), Math.min(end, text.length))
        var selectionPreserved = outputEdit.selectionStart !== outputEdit.selectionEnd
        syncingText = false
        if (selectionPreserved)
            followTail = false
        else if (text.length === 0)
            followTail = true
        scheduleFollow()
    }

    function scrollToBottom() {
        settingContentY = true
        outputView.contentY = maximumContentY
        settingContentY = false
        if (!hasSelection)
            followTail = true
    }

    function scheduleFollow() {
        if (!followTail || hasSelection || followScheduled)
            return
        followScheduled = true
        Qt.callLater(function() {
            followScheduled = false
            if (followTail && !hasSelection)
                scrollToBottom()
        })
    }

    onTextChanged: syncText()
    onWidthChanged: scheduleFollow()
    onHeightChanged: scheduleFollow()
    Component.onCompleted: syncText()

    Flickable {
        id: outputView
        objectName: "commandOutputFlickable"
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.topMargin: 9
        anchors.rightMargin: Theme.scrollWidth + 12
        anchors.bottomMargin: 9

        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        contentHeight: outputEdit.height

        onContentHeightChanged: root.scheduleFollow()
        onContentYChanged: {
            if (root.settingContentY)
                return
            if (root.hasSelection)
                root.followTail = false
            else
                root.followTail = Math.max(0, contentHeight - height) - contentY <= 1
        }

        TextEdit {
            id: outputEdit
            objectName: "commandOutputTextEdit"
            width: outputView.width
            height: Math.max(outputView.height, contentHeight)

            text: ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            persistentSelection: true
            activeFocusOnTab: true
            wrapMode: TextEdit.WrapAnywhere
            color: Theme.textSecondary
            selectionColor: Theme.accent
            selectedTextColor: Theme.textOnAccent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontMicro

            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("命令输出")

            onSelectionStartChanged: {
                if (root.syncingText)
                    return
                if (root.hasSelection)
                    root.followTail = false
                else if (root.atBottom)
                    root.followTail = true
            }
            onSelectionEndChanged: {
                if (root.syncingText)
                    return
                if (root.hasSelection)
                    root.followTail = false
                else if (root.atBottom)
                    root.followTail = true
            }
        }
    }

    Rectangle {
        id: scrollThumb
        visible: root.scrollVisible
        width: Theme.scrollWidth
        radius: Theme.scrollRadius
        color: scrollHover.containsMouse ? Theme.scrollThumbHover : Theme.scrollThumb
        anchors.right: parent.right
        anchors.rightMargin: 5
        y: outputView.y + outputView.visibleArea.yPosition * outputView.height
        height: Math.max(24, outputView.visibleArea.heightRatio * outputView.height)

        Behavior on color {
            ColorAnimation { duration: Theme.durFast; easing.type: Theme.easing }
        }

        MouseArea {
            id: scrollHover
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
    }
}
