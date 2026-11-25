import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Hands ONE message to a task or calendar app. Nothing leaves until the user
// copies or saves, and the preview is exactly what leaves.
Dialog {
    id: root
    objectName: "handoffDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string kind: "task"
    property string permalink: ""
    property string uidSeed: ""
    property bool includeLink: false
    property int durationMinutes: 60
    property string status: ""

    function openFor(text, permalink, uidSeed) {
        var firstLine = (text || "").split("\n")[0].trim()
        titleField.text = firstLine.length > 80 ? firstLine.slice(0, 80) + "…" : firstLine
        notesField.text = text || ""
        root.permalink = permalink || ""
        root.uidSeed = uidSeed || ""
        root.includeLink = false
        root.kind = "task"
        root.durationMinutes = 60
        root.status = ""
        var suggested = new Date(Date.now() + 24 * 60 * 60 * 1000)
        dateField.text = Qt.formatDate(suggested, "yyyy-MM-dd")
        timeField.text = "10:00"
        open()
    }

    readonly property var fields: ({
        kind: root.kind,
        title: titleField.text,
        notes: notesField.text,
        link: root.permalink,
        includeLink: root.includeLink,
        uidSeed: root.uidSeed,
        date: dateField.text,
        time: timeField.text,
        durationMinutes: root.durationMinutes
    })
    readonly property string previewText: exporter.preview(root.fields, false)
    readonly property bool fileReady: exporter.preview(root.fields, true).length > 0

    HandoffExporter { id: exporter }

    NativeFileDialog {
        id: saveDialog
        objectName: "handoffSaveDialog"
        purpose: "export"
        fileMode: "save"
        title: root.kind === "event" ? qsTr("Save calendar event") : qsTr("Save task")
        currentName: (root.kind === "event" ? "event" : "task") + ".ics"
        nameFilters: [ qsTr("Calendar file (*.ics)"), qsTr("All files (*)") ]
        onAccepted: root.status = exporter.saveFile(selectedFile, root.fields)
                    ? qsTr("Saved. Open the file with your task or calendar app.")
                    : qsTr("The file could not be saved.")
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12
        Label {
            text: qsTr("Export message")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }
        SegmentedControl {
            objectName: "handoffKind"
            storm: true
            dense: true
            fitWidth: true
            Layout.fillWidth: true
            model: [
                { value: "task", label: qsTr("Task") },
                { value: "event", label: qsTr("Calendar event") }
            ]
            current: root.kind
            onActivated: (value) => root.kind = value
        }
        AppTextField {
            id: titleField
            objectName: "handoffTitle"
            storm: true
            Layout.fillWidth: true
            placeholderText: qsTr("Title")
            Accessible.name: qsTr("Title")
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.kind === "event"
            spacing: AppTheme.spacing8
            AppTextField {
                id: dateField
                objectName: "handoffDate"
                storm: true
                Layout.fillWidth: true
                placeholderText: qsTr("yyyy-mm-dd")
                inputMask: "9999-99-99"
                Accessible.name: qsTr("Date")
            }
            AppTextField {
                id: timeField
                objectName: "handoffTime"
                storm: true
                Layout.preferredWidth: 90
                placeholderText: qsTr("hh:mm")
                inputMask: "99:99"
                Accessible.name: qsTr("Start time")
            }
            SegmentedControl {
                storm: true
                dense: true
                model: [
                    { value: 30, label: qsTr("30 min") },
                    { value: 60, label: qsTr("1 hour") },
                    { value: 120, label: qsTr("2 hours") }
                ]
                current: root.durationMinutes
                onActivated: (value) => root.durationMinutes = value
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.kind === "event" && !root.fileReady
            text: qsTr("Enter a valid date and time.")
            color: AppTheme.stormDanger
            font.pixelSize: AppTheme.textMeta
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 110
            TextArea {
                id: notesField
                objectName: "handoffNotes"
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("Notes")
                Accessible.name: qsTr("Notes")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.permalink.length > 0
            spacing: AppTheme.spacing8
            AppSwitch {
                objectName: "handoffIncludeLink"
                checked: root.includeLink
                onToggled: root.includeLink = !root.includeLink
                Accessible.name: qsTr("Include a link to this message")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Include a link to this message. Only people in the room can open it.")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textMeta
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Exactly this leaves Lightning. The app you hand it to is outside the room's encryption.")
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            objectName: "handoffPreview"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 8
            elide: Text.ElideRight
            text: root.previewText
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            objectName: "handoffStatus"
            Layout.fillWidth: true
            visible: root.status.length > 0
            wrapMode: Text.WordWrap
            text: root.status
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textMeta
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "handoffCopy"
                text: qsTr("Copy as text")
                kind: "secondary"
                enabled: root.previewText.length > 0
                onClicked: {
                    exporter.copyText(root.fields)
                    root.status = qsTr("Copied.")
                }
            }
            AppButton {
                objectName: "handoffSave"
                text: qsTr("Save .ics…")
                kind: "primary"
                enabled: root.fileReady && !saveDialog.busy
                onClicked: saveDialog.open()
            }
        }
    }
}
