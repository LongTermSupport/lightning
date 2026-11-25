import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A file another app shared with Lightning. Picking a room only stages it in
// that room's composer; nothing is sent until the user presses Send there.
Dialog {
    id: root
    objectName: "shareTargetDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(440, parent ? parent.width - AppTheme.spacing24 * 2 : 440)
    height: Math.min(520, parent ? parent.height - AppTheme.spacing24 * 2 : 520)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property var file: ({})
    property string searchText: ""
    readonly property bool ready: root.file.ok === true

    function openFor(link) {
        root.file = exporter.inspectSharedFile(link)
        root.searchText = ""
        open()
    }

    function stageIn(roomId) {
        if (!root.ready)
            return
        var url = root.file.url
        app.openRoom(roomId)
        app.composer.addDroppedAttachments([url], "")
        root.close()
    }

    onClosed: root.file = ({})

    HandoffExporter { id: exporter }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            text: qsTr("Share to a room")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        Label {
            objectName: "shareTargetFile"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            text: root.ready ? root.file.name : (root.file.reason || "")
            color: root.ready ? AppTheme.stormText : AppTheme.stormDanger
            font.pixelSize: AppTheme.textBody
        }
        Label {
            Layout.fillWidth: true
            visible: root.ready
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            text: qsTr("As %1. The file is added to the room's message box and is not sent until you press Send.")
                  .arg(app.accounts ? app.accounts.activeUserId : "")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        AppTextField {
            id: searchField
            Layout.fillWidth: true
            visible: root.ready
            storm: true
            searchIcon: true
            clearButton: true
            placeholderText: qsTr("Search rooms…")
            text: root.searchText
            onTextChanged: root.searchText = text
            Accessible.name: qsTr("Search rooms")
        }
        ListView {
            id: targetList
            objectName: "shareTargetRoomList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.ready
            clip: true
            spacing: AppTheme.spacing4
            model: root.ready ? app.allRooms : null
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: row
                required property string roomId
                required property string name
                required property string avatarUrl
                required property bool isSpace
                required property string membership
                readonly property string visibleName: name.length > 0 ? name : roomId
                visible: !isSpace && membership === "joined"
                         && (root.searchText.trim().length === 0
                             || visibleName.toLowerCase().indexOf(
                                    root.searchText.trim().toLowerCase()) !== -1)
                width: targetList.width
                height: visible ? 52 : 0
                radius: AppTheme.radiusMd
                color: rowHover.hovered ? AppTheme.stormSelection : "transparent"
                HoverHandler { id: rowHover }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing8
                    spacing: AppTheme.spacing8
                    Avatar {
                        mxc: row.avatarUrl
                        name: row.visibleName
                        size: 36
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: row.visibleName
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        elide: Label.ElideRight
                    }
                    AppButton {
                        storm: true
                        text: qsTr("Attach")
                        Accessible.name: qsTr("Attach in %1").arg(row.visibleName)
                        onClicked: root.stageIn(row.roomId)
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "shareTargetCancel"
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
        }
    }
}
