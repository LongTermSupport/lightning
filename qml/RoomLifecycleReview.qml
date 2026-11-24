import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    objectName: "roomLifecycleReview"
    property string roomId: ""
    property string action: ""
    property var manager: typeof app !== "undefined" && app ? app.spaces : null
    property int revision: 0
    property bool expanded: false
    onRoomIdChanged: expanded = false
    readonly property var review: {
        var tick = revision
        return manager && roomId ? manager.lifecycleReview(roomId) : ({})
    }
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    Connections {
        target: root.manager
        ignoreUnknownSignals: true
        function onSpacesChanged() { root.revision++ }
    }
    AppButton {
        objectName: "roomLifecycleExpand"
        text: root.expanded ? qsTr("Hide lifecycle review") : qsTr("Review room activity and links")
        onClicked: root.expanded = !root.expanded
    }
    Label {
        visible: root.expanded
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.stormTextSecondary
        text: qsTr("Archive hides a room for your account; it does not close it for other members. Leaving removes only you. Closing changes server access and may remove other members. No rooms are cleaned up automatically.")
    }
    Label {
        objectName: "roomLifecycleActivity"
        visible: root.expanded
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.stormTextSecondary
        text: root.review.activityKnown
            ? qsTr("Last known activity: %1. This device may not have all activity.")
                .arg(Qt.formatDateTime(root.review.lastActivity, Qt.DefaultLocaleShortDate))
            : qsTr("Last activity is not known. Missing history does not prove inactivity.")
    }
    Label {
        visible: root.expanded
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.stormTextSecondary
        text: root.review.roomId
            ? qsTr("Joined members: %1. Closure's room-by-room plan shows who can be removed and who stays. Roles and ownership are separate in every room.")
                .arg(root.review.joinedMembers)
            : qsTr("Membership and ownership are not known. Check them before continuing.")
    }
    Label {
        objectName: "roomLifecycleScope"
        visible: root.expanded
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.stormTextSecondary
        text: root.action === "close" || root.action === "delete"
            ? qsTr("The plan below lists the exact selected rooms and their outcomes. A Space's child rooms are separate rooms; shared rooms may belong to other Spaces.")
            : qsTr("This action affects only %1. Rooms inside a Space are not left or upgraded with it.").arg(root.roomId)
    }
    Label {
        objectName: "roomLifecycleSuccessor"
        Layout.fillWidth: true
        visible: root.expanded && (!!root.review.successor || !!root.review.predecessor)
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.stormText
        text: qsTr("Predecessor: %1\nSuccessor: %2\nThese links remain in room state. Open the successor from the room's replacement notice before leaving if you need to continue there.")
            .arg(root.review.predecessor || qsTr("None known"))
            .arg(root.review.successor || qsTr("None known"))
    }
}
