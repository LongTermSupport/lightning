import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// "Remove recent messages" for a moderator. Counting happens on open; nothing
// is redacted until Remove is pressed.
AppDialog {
    id: root
    objectName: "removeRecentDialog"
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(440, parent ? parent.width - 64 : 440)
    modal: true
    destructive: true
    title: qsTr("Remove recent messages")
    standardButtons: Dialog.NoButton

    readonly property bool canRedactOthers:
        !!app.openRoomPermissions
        && app.openRoomPermissions.roomId === app.currentRoomId
        && app.openRoomPermissions.redactKnown === true
            ? app.openRoomPermissions.canRedactOthers === true
            : (!!app.roomInfo && app.roomInfo.roomId === app.currentRoomId
               && app.roomInfo.canRedactOthers === true)
    readonly property string name: app.bulkRedaction.purgeName
    property bool keepStateEvents: true
    property bool starting: false
    readonly property bool counting: app.bulkRedaction.purgeCounting
    readonly property int count: app.bulkRedaction.purgeMessageCount
        + (keepStateEvents ? 0 : app.bulkRedaction.purgeStateCount)

    function openFor(member) {
        if (!member || !member.userId || app.bulkRedaction.running)
            return false
        if (!app.bulkRedaction.preparePurge(app.timeline, member.userId,
                                            member.displayName || "",
                                            root.canRedactOthers))
            return false
        return _show()
    }
    // Repeats the walk from where the last one stopped, under the same
    // confirmation.
    function openFurther() {
        if (!app.bulkRedaction.continuePurge(root.canRedactOthers))
            return false
        return _show()
    }
    function _show() {
        keepStateEvents = true
        keepBox.checked = true
        starting = false
        reasonField.text = ""
        open()
        return true
    }

    onClosed: {
        if (!starting && !app.bulkRedaction.running)
            app.bulkRedaction.cancel()
        starting = false
    }

    ColumnLayout {
        width: parent ? parent.width : 0
        spacing: AppTheme.spacing12
        Label {
            objectName: "removeRecentQuestion"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.stormText
            text: root.counting
                  ? qsTr("Counting recent messages from %1…").arg(root.name)
                  : root.count === 0
                    ? (app.bulkRedaction.purgeCapped
                       ? qsTr("No messages from %1 in this part of the history.")
                       : qsTr("No recent messages from %1 to remove.")).arg(root.name)
                    : app.bulkRedaction.purgeCapped
                      ? qsTr("Remove at least %n recent message(s) from %1?", "",
                             root.count).arg(root.name)
                      : qsTr("Remove %n recent message(s) from %1?", "",
                             root.count).arg(root.name)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.stormTextSecondary
            text: qsTr("They will be removed for everyone in this room. This cannot be undone.")
        }
        AppTextField {
            id: reasonField
            objectName: "removeRecentReason"
            storm: true
            Layout.fillWidth: true
            placeholderText: qsTr("Reason (optional)")
            Accessible.name: qsTr("Reason for removing messages")
        }
        CheckBox {
            id: keepBox
            objectName: "removeRecentKeepState"
            Layout.fillWidth: true
            palette.windowText: AppTheme.stormText
            text: qsTr("Keep state events (name/avatar changes, joins)")
            checked: root.keepStateEvents
            onToggled: root.keepStateEvents = checked
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "removeRecentLookFurther"
                visible: !root.counting && root.count === 0
                         && app.bulkRedaction.purgeCanLookFurther
                text: qsTr("Look further back")
                kind: "secondary"
                onClicked: app.bulkRedaction.continuePurge(root.canRedactOthers)
            }
            AppButton {
                objectName: "removeRecentCancel"
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "removeRecentConfirm"
                text: qsTr("Remove")
                kind: "danger"
                enabled: !root.counting && root.count > 0
                onClicked: {
                    root.starting = true
                    app.bulkRedaction.startPurge(reasonField.text.trim(),
                                                 root.keepStateEvents)
                    root.close()
                }
            }
        }
    }
}
