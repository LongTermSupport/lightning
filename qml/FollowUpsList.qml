import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The Activity panel's "To do" list: messages the user marked "Needs reply"
// or "Waiting". Only the user changes a mark; a reply only offers "Done".
ListView {
    id: list
    objectName: "followUpsList"

    property var followUps: typeof app !== "undefined" ? app.followUps : null
    property var activity: null
    signal entryOpened()

    function timeLabel(ms) {
        if (!ms || ms <= 0)
            return ""
        var d = new Date(ms)
        return d.toDateString() === new Date().toDateString()
               ? Qt.formatTime(d, app.settings.clockTimeFormat)
               : Qt.locale(AppLocale.name).toString(d, Locale.ShortFormat)
    }
    function fill(template, values) {
        return template.replace(/%([1-9])/g, function(m, d) {
            var i = parseInt(d, 10) - 1
            return i < values.length ? String(values[i]) : m
        })
    }
    function stateLabel(state) {
        return state === "waiting" ? qsTr("Waiting for reply") : qsTr("Needs reply")
    }
    function reminderDue(roomId, eventId) {
        if (typeof app === "undefined" || !app.reminders || !app.reminders.reminderDue)
            return 0
        var revision = app.reminders.revision
        return app.reminders.reminderDue(roomId, eventId)
    }
    function openItem(item) {
        if (!list.activity)
            return
        list.activity.openEvent(item.roomId, item.eventId, item.threadRootId || "")
        list.entryOpened()
    }

    clip: true
    spacing: AppTheme.spacing4
    model: followUps ? followUps.items : []
    activeFocusOnTab: true
    currentIndex: 0
    Accessible.role: Accessible.List
    Accessible.name: qsTr("To do")
    Keys.onReturnPressed: if (currentItem) list.openItem(currentItem.modelData)
    Keys.onEnterPressed: if (currentItem) list.openItem(currentItem.modelData)
    ScrollBar.vertical: ScrollBar { }

    delegate: Rectangle {
        id: row
        required property int index
        required property var modelData
        objectName: "followUpRow"
        width: list.width
        implicitHeight: rowContent.implicitHeight + AppTheme.spacing12 * 2
        radius: AppTheme.radiusTile
        color: rowHover.hovered ? AppTheme.stormSelection : AppTheme.stormInset
        border.width: row.ListView.isCurrentItem && list.activeFocus ? 2 : 1
        border.color: AppTheme.stormBorder
        readonly property real due: list.reminderDue(modelData.roomId, modelData.eventId)
        Accessible.role: Accessible.Button
        Accessible.name: list.fill(qsTr("%1 in %2"),
                                   [list.stateLabel(modelData.state), modelData.roomName])
        Accessible.onPressAction: list.openItem(modelData)
        HoverHandler { id: rowHover }
        TapHandler {
            gesturePolicy: TapHandler.WithinBounds
            onTapped: {
                list.currentIndex = row.index
                list.openItem(row.modelData)
            }
        }
        RowLayout {
            id: rowContent
            anchors.fill: parent
            anchors.margins: AppTheme.spacing12
            spacing: AppTheme.spacing8
            Icon {
                Layout.alignment: Qt.AlignTop
                name: row.modelData.state === "waiting" ? "schedule" : "flag"
                size: 18
                color: AppTheme.stormText
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing6
                    Label {
                        text: row.modelData.roomName
                        textFormat: Text.PlainText
                        color: AppTheme.stormText
                        font.weight: AppTheme.weightStrong
                        elide: Text.ElideRight
                        Layout.maximumWidth: 240
                    }
                    Label {
                        objectName: "followUpState"
                        Layout.fillWidth: true
                        text: list.stateLabel(row.modelData.state)
                              + (row.modelData.threadRootId ? " · " + qsTr("In a thread") : "")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textMeta
                        elide: Text.ElideRight
                    }
                    Label {
                        text: list.timeLabel(row.modelData.atMs)
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textMeta
                    }
                }
                Label {
                    objectName: "followUpReplied"
                    visible: row.modelData.replied === true
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("You replied. Mark it done if that settles it.")
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textMeta
                }
                Label {
                    visible: row.due > 0
                    Layout.fillWidth: true
                    text: list.fill(qsTr("Reminder: %1"), [list.timeLabel(row.due)])
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
            AppButton {
                objectName: "followUpToggle"
                storm: true
                kind: "ghost"
                size: "sm"
                text: row.modelData.state === "waiting" ? qsTr("Needs reply")
                                                         : qsTr("Waiting")
                onClicked: list.followUps.setState(row.modelData.roomId, row.modelData.eventId,
                    row.modelData.threadRootId,
                    row.modelData.state === "waiting" ? "reply" : "waiting")
            }
            AppButton {
                objectName: "followUpDone"
                storm: true
                kind: "secondary"
                size: "sm"
                iconName: "check"
                text: qsTr("Done")
                onClicked: list.followUps.markDone(row.modelData.roomId, row.modelData.eventId)
            }
        }
    }
}
