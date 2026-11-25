import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    signal archiveStaged()
    function chooseFolder() {
        folderPicker.room = app.currentRoomId
        folderPicker.open()
    }
    function inspect(row) {
        photoDialog.summary = ({})
        photoDialog.request = app.composer.attachments.inspectPhoto(row, app.composer.imageMetadataMode)
        if (photoDialog.request) photoDialog.open()
    }
    AppDialog {
        id: photoDialog
        title: qsTr("Photo metadata")
        parent: Overlay.overlay
        width: Math.min(440, parent ? parent.width - 32 : 440)
        standardButtons: Dialog.Close
        property var request: 0
        property var summary: ({})
        Connections {
            target: app.composer.attachments
            ignoreUnknownSignals: true
            function onPhotoInspected(request, result) {
                if (photoDialog.request === request) photoDialog.summary = result
            }
            function onModelAboutToBeReset() { photoDialog.close() }
        }
        Connections {
            target: app.composer
            ignoreUnknownSignals: true
            function onImageMetadataModeChanged() { photoDialog.close() }
        }
        contentItem: Label {
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: photoDialog.summary.error || (photoDialog.summary.location
                ? qsTr("EXIF GPS: %1\nEXIF timestamp: %2\nCamera make or model: %3\nEXIF orientation: %4")
                    .arg(photoDialog.summary.location).arg(photoDialog.summary.timestamp)
                    .arg(photoDialog.summary.device).arg(photoDialog.summary.orientation)
                : qsTr("Inspecting photo…"))
        }
    }
    NativeFileDialog {
        id: folderPicker
        property string room
        purpose: "attach"
        fileMode: "folder"
        title: qsTr("Choose a folder to archive")
        currentFolder: app.defaultFileDialogFolder()
        onAccepted: {
            if (room !== app.currentRoomId || !app.composer.attachmentsSupported)
                return
            archiveDialog.files = []
            archiveDialog.omissions = []
            archiveDialog.selected = []
            archiveDialog.reviewFailed = false
            archiveDialog.errorText = ""
            archiveDialog.busy = true
            archiveDialog.open()
            app.composer.attachments.reviewFolder(selectedFile)
        }
    }
    AppDialog {
        id: archiveDialog
        title: qsTr("Review folder archive")
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(520, parent ? parent.width - 32 : 520)
        property var files: []
        property var omissions: []
        property var selected: []
        property bool busy: false
        property string errorText: ""
        property bool reviewFailed: false
        onClosed: app.composer.attachments.cancelFolderReview()
        Connections {
            target: app
            function onCurrentRoomIdChanged() {
                folderPicker.room = ""
                archiveDialog.close()
            }
        }
        Connections {
            target: app.composer.attachments
            ignoreUnknownSignals: true
            function onFolderReviewed(files, omissions, error) {
                archiveDialog.files = files
                archiveDialog.omissions = omissions
                archiveDialog.selected = files.map(f => f.name)
                archiveDialog.reviewFailed = !!error
                archiveDialog.errorText = error
                archiveDialog.busy = false
            }
            function onArchiveCreated(error) {
                archiveDialog.busy = false
                if (!error) {
                    archiveDialog.close()
                    root.archiveStaged()
                } else {
                    archiveDialog.errorText = error
                }
            }
        }
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: archiveDialog.errorText || (archiveDialog.busy ? qsTr("Preparing archive…")
                    : qsTr("%1 files selected (%2 bytes)").arg(archiveDialog.selected.length)
                        .arg(archiveDialog.files.reduce((sum, f) => sum
                            + (archiveDialog.selected.indexOf(f.name) >= 0 ? f.bytes : 0), 0)))
            }
            ScrollView {
                id: archiveScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(320, archiveList.implicitHeight)
                clip: true
                Column {
                    id: archiveList
                    width: archiveScroll.availableWidth
                    Repeater {
                        model: archiveDialog.files
                        CheckBox {
                            required property var modelData
                            width: archiveList.width
                            checked: archiveDialog.selected.indexOf(modelData.name) >= 0
                            contentItem: Label {
                                leftPadding: parent.indicator.width + parent.spacing
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.WrapAnywhere
                                textFormat: Text.PlainText
                                text: parent.modelData.name
                            }
                            Accessible.name: modelData.name
                            onClicked: {
                                const updated = archiveDialog.selected.filter(n => n !== modelData.name)
                                if (checked) updated.push(modelData.name)
                                archiveDialog.selected = updated
                                if (!archiveDialog.reviewFailed) archiveDialog.errorText = ""
                            }
                        }
                    }
                    Repeater {
                        model: archiveDialog.omissions
                        Label {
                            required property string modelData
                            width: archiveList.width
                            wrapMode: Text.WrapAnywhere
                            textFormat: Text.PlainText
                            text: qsTr("Excluded: %1").arg(modelData)
                        }
                    }
                }
            }
            RowLayout {
                Button { text: qsTr("Cancel"); onClicked: archiveDialog.close() }
                Button {
                    text: qsTr("Create archive")
                    enabled: !archiveDialog.busy && !archiveDialog.reviewFailed
                             && archiveDialog.selected.length > 0
                    onClicked: {
                        archiveDialog.busy = true
                        app.composer.attachments.createFolderArchive(archiveDialog.selected)
                    }
                }
            }
        }
    }
}
