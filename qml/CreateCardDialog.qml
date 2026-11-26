import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Creates or edits a shared checklist or community event in the open room.
Dialog {
    id: root
    objectName: "createCardDialog"

    property string kind: "checklist"
    property string roomId: ""
    property string editEventId: ""
    property var original: ({})
    property string failure: ""
    property bool sending: false
    readonly property bool editing: editEventId !== ""

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    padding: AppTheme.spacing16
    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    function pad(n) { return (n < 10 ? "0" : "") + n }
    function isoDate(d) { return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) }

    function resetFields() {
        failure = ""
        sending = false
        titleField.text = ""
        itemsArea.text = ""
        anyoneSwitch.checked = true
        var tomorrow = new Date(Date.now() + 24 * 3600 * 1000)
        dateField.text = isoDate(tomorrow)
        startField.text = "18:00"
        endField.text = ""
        locationField.text = ""
        linkField.text = ""
        tzBox.currentIndex = Math.max(0, tzBox.model.indexOf(app.roomCards.systemTimeZone()))
    }
    function openCreate(room, cardKind) {
        roomId = room
        kind = cardKind
        editEventId = ""
        original = ({})
        resetFields()
        open()
        titleField.forceActiveFocus()
    }
    function openEdit(room, eventId, cardKind, info) {
        roomId = room
        kind = cardKind
        editEventId = eventId
        original = info
        resetFields()
        titleField.text = info.title || ""
        if (cardKind === "checklist") {
            anyoneSwitch.checked = info.anyone_edits !== false
            itemsArea.text = rootItems().map(function(i) { return i.text }).join("\n")
        } else {
            var tz = info.tz || app.roomCards.systemTimeZone()
            tzBox.currentIndex = Math.max(0, tzBox.model.indexOf(tz))
            var when = app.roomCards.wallClock(info.start || 0, tz)
            dateField.text = when.date
            startField.text = when.time
            endField.text = info.end ? app.roomCards.wallClock(info.end, tz).time : ""
            locationField.text = info.location || ""
            linkField.text = info.link || ""
        }
        open()
    }
    // Items added later by members are not part of the original message.
    function rootItems() {
        return (original.items || []).filter(function(i) { return String(i.id).charAt(0) !== "$" })
    }
    function checklistData() {
        var lines = itemsArea.text.split("\n").map(function(l) { return l.trim() })
                                 .filter(function(l) { return l.length > 0 })
        var unused = rootItems().slice()
        var items = []
        for (var i = 0; i < lines.length; ++i) {
            var reuse = unused.findIndex(function(o) { return o.text === lines[i] })
            var id = reuse >= 0 ? unused.splice(reuse, 1)[0].id : "i" + Date.now().toString(36) + i
            items.push({ id: id, text: lines[i] })
        }
        return { title: titleField.text.trim(), anyone_edits: anyoneSwitch.checked, items: items }
    }
    readonly property string tz: tzBox.currentText
    readonly property real startMs: app.roomCards.toUtcMs(dateField.text.trim(), startField.text.trim(), tz)
    readonly property real endMs: endField.text.trim().length === 0 ? 0
        : app.roomCards.toUtcMs(dateField.text.trim(), endField.text.trim(), tz)
    function eventData() {
        return { title: titleField.text.trim(), start: startMs, end: endMs, tz: tz,
                 location: locationField.text.trim(), link: linkField.text.trim(),
                 cancelled: original.cancelled === true }
    }
    readonly property bool valid: kind === "checklist"
        ? itemsArea.text.trim().length > 0
        : titleField.text.trim().length > 0 && startMs > 0
          && (endField.text.trim().length === 0 || endMs > startMs)
          && (linkField.text.trim().length === 0 || linkField.text.trim().indexOf("https://") === 0)

    function submit() {
        if (!valid || sending)
            return
        failure = ""
        var data = kind === "checklist" ? checklistData() : eventData()
        var ok = editing ? app.roomCards.edit(roomId, editEventId, data)
                         : app.roomCards.create(roomId, kind, data)
        if (!ok) {
            failure = qsTr("This could not be sent. Try again.")
            return
        }
        if (editing)
            close()
        else
            sending = true
    }
    Connections {
        target: app.roomCards
        function onCreateFinished(ok, message) {
            if (!root.sending)
                return
            root.sending = false
            if (ok)
                root.close()
            else
                root.failure = message
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing8
        Label {
            Layout.fillWidth: true
            text: root.kind === "checklist"
                  ? (root.editing ? qsTr("Edit checklist") : qsTr("New checklist"))
                  : (root.editing ? qsTr("Edit event") : qsTr("New event"))
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }
        AppTextField {
            id: titleField
            objectName: "createCardTitle"
            Layout.fillWidth: true
            maximumLength: 200
            placeholderText: root.kind === "checklist" ? qsTr("Title (optional)") : qsTr("Title")
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.kind === "checklist"
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Items, one per line")
                color: AppTheme.textSecondary
                font.pixelSize: AppTheme.textMeta
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                TextArea {
                    id: itemsArea
                    objectName: "createCardItems"
                    wrapMode: TextArea.Wrap
                    textFormat: TextEdit.PlainText
                    color: AppTheme.textPrimary
                }
            }
            CheckBox {
                id: anyoneSwitch
                text: qsTr("Anyone in the room can tick and add items")
            }
        }

        GridLayout {
            Layout.fillWidth: true
            visible: root.kind === "event"
            columns: 2
            columnSpacing: AppTheme.spacing8
            rowSpacing: AppTheme.spacing8
            Label { text: qsTr("Date"); color: AppTheme.textSecondary }
            AppTextField {
                id: dateField
                objectName: "createCardDate"
                Layout.fillWidth: true
                //: The date format this field accepts.
                placeholderText: qsTr("YYYY-MM-DD")
                inputMethodHints: Qt.ImhDate
            }
            Label { text: qsTr("Starts"); color: AppTheme.textSecondary }
            AppTextField {
                id: startField
                objectName: "createCardStart"
                Layout.fillWidth: true
                //: The time format this field accepts (24-hour).
                placeholderText: qsTr("HH:MM")
            }
            Label { text: qsTr("Ends (optional)"); color: AppTheme.textSecondary }
            AppTextField {
                id: endField
                Layout.fillWidth: true
                //: The time format this field accepts (24-hour).
                placeholderText: qsTr("HH:MM")
            }
            Label { text: qsTr("Time zone"); color: AppTheme.textSecondary }
            AppComboBox {
                id: tzBox
                Layout.fillWidth: true
                filterable: true
                filterPlaceholder: qsTr("Search time zones")
                model: app.roomCards.timeZones()
            }
            Label { text: qsTr("Where"); color: AppTheme.textSecondary }
            AppTextField {
                id: locationField
                Layout.fillWidth: true
                maximumLength: 300
                placeholderText: qsTr("Place or room (optional)")
            }
            Label { text: qsTr("Link"); color: AppTheme.textSecondary }
            AppTextField {
                id: linkField
                Layout.fillWidth: true
                maximumLength: 512
                placeholderText: qsTr("https:// call or page (optional)")
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.kind === "event"
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: root.startMs > 0 ? AppTheme.textMuted : AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            text: root.startMs > 0
                  ? app.roomCards.formatWhen(root.startMs, root.endMs, root.tz)
                  : qsTr("Enter a date and a time that exists in this time zone.")
        }
        Label {
            Layout.fillWidth: true
            visible: root.failure.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: AppTheme.danger
            text: root.failure
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                kind: "ghost"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            AppButton {
                objectName: "createCardSubmit"
                kind: "primary"
                text: root.editing ? qsTr("Save") : qsTr("Send")
                enabled: root.valid && !root.sending
                onClicked: root.submit()
            }
        }
    }
}
