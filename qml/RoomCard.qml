import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A shared checklist or community event (RoomCardsController). Everything in
// it was written by room members, so it renders as plain text.
Rectangle {
    id: card
    objectName: "roomCard"

    property string roomId: ""
    property string eventId: ""
    property string kind: ""
    property bool readOnly: false
    readonly property var info: {
        void app.roomCards.revision
        return app.roomCards.card(card.roomId, card.eventId)
    }
    readonly property bool loaded: info.kind === card.kind
    readonly property bool shown: loaded && info.deleted !== true
    readonly property bool busy: { void app.roomCards.revision; return app.roomCards.busy(card.roomId, card.eventId) }
    property string failure: ""

    implicitWidth: 380
    implicitHeight: shown ? body.implicitHeight + AppTheme.spacing12 * 2 : 0
    visible: shown
    radius: AppTheme.radiusMd
    color: AppTheme.surfaceAlt
    border.color: AppTheme.border

    Component.onCompleted: if (card.eventId !== "") app.roomCards.load(card.roomId, card.eventId, true)
    Timer {
        // Updates are hidden events, so a visible card re-reads the local event cache.
        interval: 15000
        repeat: true
        running: card.visible && card.Window.active === true
        onTriggered: app.roomCards.load(card.roomId, card.eventId, false)
    }
    Connections {
        target: app.roomCards
        function onActionFailed(roomId, eventId, message) {
            if (roomId === card.roomId && eventId === card.eventId)
                card.failure = message
        }
    }

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: AppTheme.spacing12
        spacing: AppTheme.spacing6

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            Icon {
                name: card.kind === "event" ? "schedule" : "format_list_bulleted"
                size: 18
                color: AppTheme.textMuted
            }
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                text: card.info.title || (card.kind === "checklist" ? qsTr("Checklist") : "")
                font.weight: AppTheme.weightStrong
                font.strikeout: card.info.cancelled === true
                color: AppTheme.textPrimary
            }
            Label {
                visible: card.info.cancelled === true
                text: qsTr("Cancelled")
                color: AppTheme.danger
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
            }
            IconButton {
                objectName: "roomCardEditButton"
                visible: card.info.can_edit === true && !card.readOnly
                iconName: "edit_square"
                Accessible.name: qsTr("Edit")
                ToolTip.text: Accessible.name
                ToolTip.visible: hovered
                enabled: !card.busy
                onClicked: {
                    editLoader.active = true
                    editLoader.item.openEdit(card.roomId, card.eventId, card.kind, card.info)
                }
            }
        }

        // Checklist
        Label {
            visible: card.kind === "checklist"
            text: qsTr("%1 of %n done", "", (card.info.items || []).length).arg(card.info.done_count || 0)
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
        }
        Repeater {
            model: card.kind === "checklist" ? (card.info.items || []) : []
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: AppTheme.spacing4
                CheckBox {
                    objectName: "roomCardItemCheck"
                    checked: modelData.done === true
                    enabled: card.info.can_update === true && !card.readOnly && !card.busy
                    Accessible.name: modelData.text
                    onToggled: {
                        app.roomCards.update(card.roomId, card.eventId,
                                             checked ? "done" : "undone", modelData.id, "")
                        checked = Qt.binding(function() { return modelData.done === true })
                    }
                }
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    text: modelData.text
                    color: modelData.done ? AppTheme.textMuted : AppTheme.textPrimary
                    font.strikeout: modelData.done === true
                    HoverHandler { id: itemHover }
                    ToolTip.visible: itemHover.hovered && modelData.done === true
                    ToolTip.text: qsTr("Done by %1").arg(modelData.by || "")
                }
                IconButton {
                    visible: card.info.can_update === true && !card.readOnly
                    iconName: "close"
                    enabled: !card.busy
                    Accessible.name: qsTr("Remove item")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                    onClicked: app.roomCards.update(card.roomId, card.eventId, "remove", modelData.id, "")
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: card.kind === "checklist" && card.info.can_update === true && !card.readOnly
            AppTextField {
                id: addField
                objectName: "roomCardAddField"
                Layout.fillWidth: true
                placeholderText: qsTr("Add an item")
                maximumLength: 300
                onAccepted: addButton.clicked()
            }
            AppButton {
                id: addButton
                size: "sm"
                text: qsTr("Add")
                enabled: addField.text.trim().length > 0 && !card.busy
                onClicked: if (app.roomCards.update(card.roomId, card.eventId, "add", "", addField.text))
                    addField.text = ""
            }
        }

        // Event
        Label {
            Layout.fillWidth: true
            visible: card.kind === "event"
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AppTheme.textSecondary
            text: card.kind === "event" && card.loaded
                  ? app.roomCards.formatWhen(card.info.start || 0, card.info.end || 0, card.info.tz || "")
                  : ""
        }
        Label {
            Layout.fillWidth: true
            visible: card.kind === "event" && (card.info.location || "").length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AppTheme.textSecondary
            text: qsTr("Where: %1").arg(card.info.location || "")
        }
        AppButton {
            visible: card.kind === "event" && (card.info.link || "").length > 0
            kind: "ghost"
            size: "sm"
            text: qsTr("Open link")
            ToolTip.text: card.info.link || ""
            ToolTip.visible: hovered
            onClicked: app.media.openWebUrl(card.info.link)
        }
        Label {
            visible: card.kind === "event"
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("%1 going · %2 maybe · %3 can't go")
                  .arg(card.info.yes || 0).arg(card.info.maybe || 0).arg(card.info.no || 0)
        }
        Flow {
            Layout.fillWidth: true
            visible: card.kind === "event"
            spacing: AppTheme.spacing6
            Repeater {
                model: [
                    { status: "yes", label: qsTr("Going") },
                    { status: "maybe", label: qsTr("Maybe") },
                    { status: "no", label: qsTr("Can't go") }
                ]
                delegate: AppButton {
                    required property var modelData
                    objectName: "roomCardRsvp_" + modelData.status
                    size: "sm"
                    kind: card.info.own === modelData.status ? "primary" : "ghost"
                    text: modelData.label
                    visible: card.info.cancelled !== true && card.info.can_rsvp === true && !card.readOnly
                    enabled: !card.busy && card.info.own !== modelData.status
                    onClicked: app.roomCards.rsvp(card.roomId, card.eventId, modelData.status)
                }
            }
            AppButton {
                size: "sm"
                kind: "ghost"
                text: qsTr("Save to calendar…")
                onClicked: icsDialog.open()
            }
            AppButton {
                size: "sm"
                kind: "danger"
                visible: card.info.can_edit === true && card.info.cancelled !== true && !card.readOnly
                enabled: !card.busy
                text: qsTr("Cancel event")
                onClicked: {
                    var d = {
                        title: card.info.title, start: card.info.start, end: card.info.end || 0,
                        tz: card.info.tz, location: card.info.location || "",
                        link: card.info.link || "", cancelled: true
                    }
                    app.roomCards.edit(card.roomId, card.eventId, d)
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: card.failure.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            text: card.failure
        }
    }

    NativeFileDialog {
        id: icsDialog
        purpose: "export"
        currentFolder: app.defaultFileDialogFolder()
        fileMode: "save"
        title: qsTr("Save calendar entry")
        currentName: "event.ics"
        nameFilters: [ qsTr("Calendar (*.ics)"), qsTr("All files (*)") ]
        onAccepted: if (!app.roomCards.saveIcs(card.info, card.roomId, selectedFile))
            card.failure = qsTr("The calendar file could not be saved.")
    }
    Loader {
        id: editLoader
        active: false
        sourceComponent: Component { CreateCardDialog {} }
    }
}
