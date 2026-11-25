import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One room's moderation cases from the policy lists this account follows.
// Nothing happens on its own: each ban is confirmed for one person, and is
// re-checked against the lists and the current power levels before it is sent.
Dialog {
    id: root
    objectName: "moderationQueueDialog"

    readonly property var queue: app.moderationQueue
    property string roomName: ""
    property string showing: "open"
    readonly property var shownCases: {
        var rows = []
        for (var c of (root.queue ? root.queue.cases : []))
            if (root.showing === "all" || c.status === root.showing)
                rows.push(c)
        return rows
    }

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(600, parent ? parent.width - AppTheme.spacing24 * 2 : 600)
    height: Math.min(560, parent ? parent.height - AppTheme.spacing24 * 2 : 560)
    padding: AppTheme.spacing16
    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }
    onClosed: if (root.queue) root.queue.close()

    function openFor(id, name) {
        root.roomName = name
        root.showing = "open"
        root.queue.open(id)
        open()
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: root.roomName.length > 0
                  ? qsTr("Moderation queue for %1").arg(root.roomName)
                  : qsTr("Moderation queue")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
            elide: Label.ElideRight
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("People in this room who are covered by a moderation list you follow. "
                       + "A match is someone else's judgement: nothing is done until you "
                       + "confirm a ban for one person.")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppComboBox {
                id: showBox
                objectName: "moderationQueueFilter"
                textRole: "label"
                valueRole: "value"
                model: [
                    { label: qsTr("Open cases"), value: "open" },
                    { label: qsTr("Dismissed"), value: "dismissed" },
                    { label: qsTr("Banned"), value: "banned" },
                    { label: qsTr("All cases"), value: "all" }
                ]
                Component.onCompleted: currentIndex = 0
                onActivated: root.showing = currentValue
            }
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideRight
                text: !root.queue ? ""
                      : root.queue.loading ? qsTr("Checking members…")
                      : root.queue.message.length > 0 ? root.queue.message
                      : root.queue.listCount === 0 ? qsTr("You do not follow any moderation list.")
                      : root.queue.truncated ? qsTr("Only part of the room or the lists could be checked.")
                      : ""
            }
            AppButton {
                kind: "ghost"
                size: "sm"
                text: qsTr("Refresh")
                enabled: root.queue && !root.queue.loading
                onClicked: root.queue.refresh()
            }
        }

        ListView {
            id: caseList
            objectName: "moderationCaseList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: AppTheme.spacing8
            model: root.shownCases
            ScrollBar.vertical: AppScrollBar {}
            delegate: Rectangle {
                id: caseRow
                required property var modelData
                width: ListView.view.width
                implicitHeight: caseColumn.implicitHeight + AppTheme.spacing12 * 2
                radius: AppTheme.radiusMd
                color: AppTheme.surfaceAlt
                border.color: AppTheme.border
                ColumnLayout {
                    id: caseColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing4
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: caseRow.modelData.user
                            color: AppTheme.textPrimary
                            font.weight: AppTheme.weightStrong
                            elide: Label.ElideRight
                        }
                        Label {
                            textFormat: Text.PlainText
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                            text: caseRow.modelData.status === "dismissed" ? qsTr("Dismissed")
                                  : caseRow.modelData.status === "banned" ? qsTr("Banned")
                                  : qsTr("Open")
                        }
                    }
                    Repeater {
                        model: caseRow.modelData.evidence || []
                        delegate: Label {
                            required property var modelData
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: AppTheme.textSecondary
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("List %1: %2 rule \"%3\"%4")
                                  .arg(modelData.list).arg(modelData.kind).arg(modelData.entity)
                                  .arg(modelData.reason ? " — " + modelData.reason : "")
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.textMeta
                        text: caseRow.modelData.result || ""
                    }
                    RowLayout {
                        spacing: AppTheme.spacing8
                        AppButton {
                            objectName: "moderationBanButton"
                            size: "sm"
                            kind: "danger"
                            text: qsTr("Ban…")
                            visible: caseRow.modelData.status !== "banned"
                            enabled: root.queue.canBan && !root.queue.busy
                            onClicked: if (root.queue.requestBan(caseRow.modelData.user)) confirm.open()
                        }
                        AppButton {
                            size: "sm"
                            kind: "ghost"
                            text: caseRow.modelData.status === "dismissed" ? qsTr("Reopen") : qsTr("Dismiss")
                            visible: caseRow.modelData.status !== "banned"
                            enabled: !root.queue.busy
                            onClicked: root.queue.setDecision(caseRow.modelData.user,
                                caseRow.modelData.status === "dismissed" ? "open" : "dismissed")
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: caseList.count === 0 && root.queue && !root.queue.loading
                text: qsTr("No cases to show.")
                color: AppTheme.textMuted
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }

    Dialog {
        id: confirm
        objectName: "moderationBanConfirm"
        modal: true
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(460, parent ? parent.width - AppTheme.spacing24 * 2 : 460)
        padding: AppTheme.spacing16
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }
        readonly property var pending: root.queue ? root.queue.pending : ({})
        onOpened: reasonField.text = (pending.evidence && pending.evidence.length > 0)
                                     ? (pending.evidence[0].reason || "") : ""
        onClosed: if (root.queue && root.queue.pending.user) root.queue.cancelRequest()
        onPendingChanged: if (!pending.user && visible) close()
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: AppTheme.textPrimary
                font.weight: AppTheme.weightStrong
                text: qsTr("Ban %1 from this room?").arg(confirm.pending.user || "")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: AppTheme.textSecondary
                font.pixelSize: AppTheme.textMeta
                text: qsTr("They are removed and cannot rejoin this room until unbanned. "
                           + "Other rooms are not affected. The lists and your permission "
                           + "are checked again before the ban is sent.")
            }
            AppTextField {
                id: reasonField
                Layout.fillWidth: true
                placeholderText: qsTr("Reason (optional, visible to the room)")
                maximumLength: 500
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Cancel")
                    kind: "ghost"
                    onClicked: confirm.close()
                }
                AppButton {
                    objectName: "moderationBanConfirmButton"
                    text: qsTr("Ban")
                    kind: "danger"
                    onClicked: {
                        root.queue.confirmBan(confirm.pending.user || "", reasonField.text)
                        confirm.close()
                    }
                }
            }
        }
    }
}
