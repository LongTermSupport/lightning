import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

RowLayout {
    id: root
    required property string labelName
    required property string actionName
    readonly property var search: app.messageSearch
    readonly property var receipt: search.roomCoverage
    readonly property bool here: search.indexingRoomId === search.roomId && search.indexing
    spacing: AppTheme.spacingS

    function refresh() {
        if (visible && search.roomId && search.roomId === app.currentRoomId)
            search.refreshRoomCoverage()
    }
    onVisibleChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: root.search
        function onRoomIdChanged() { root.refresh() }
        function onIndexingChanged() { root.refresh() }
        function onIndexStatsChanged() { root.refresh() }
        function onIndexAllChanged() {
            if (!root.search.indexAllActive) root.refresh()
        }
    }
    Timer {
        running: root.visible && (root.here || root.search.indexAllActive)
        repeat: true
        interval: 2000
        onTriggered: root.refresh()
    }

    Label {
        objectName: root.labelName
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.textMuted
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        text: {
            var s = root.search
            var r = root.receipt
            var lines = []
            if (s.state === "too_short")
                lines.push(qsTr("Type at least %1 characters.").arg(s.minLocalChars))
            if (s.indexing && !root.here)
                lines.push(qsTr("Indexing another room."))
            if (r.messages >= 0)
                lines.push(qsTr("%n indexed message(s) in this room.", "", r.messages))
            if (root.here || r.state === "scanning")
                lines.push(qsTr("Scanning room history… %n page(s) indexed.", "", r.state === "scanning" ? r.pages : 0))
            else if (s.indexWriteFailed || r.state === "storage_failed")
                lines.push(qsTr("The search index could not be updated. Retry this room after checking local storage."))
            else if (r.state === "covered")
                lines.push(qsTr("The last scan reached the start of the room."))
            else if (r.state === "capped")
                lines.push(qsTr("The last scan reached its page limit. Older messages may be missing."))
            else if (r.state === "fetch_failed")
                lines.push(qsTr("Room history could not be read. Retry this room."))
            else if (r.state === "interrupted")
                lines.push(qsTr("The last room scan was interrupted."))
            else if (r.state === "waiting_for_keys" && !r.undecryptable)
                lines.push(qsTr("Some messages are waiting for decryption keys."))
            else if (!r.state || r.state === "unscanned")
                lines.push(qsTr("No detailed room scan is available."))
            if (r.state === "waiting_for_keys" && r.reducer_version > 0 && !r.reached_start)
                lines.push(qsTr("The scan also reached its page limit."))
            if (r.undecryptable > 0)
                lines.push(qsTr("At least %n message(s) could not be decrypted.", "", r.undecryptable))
            if (r.scanned_ms > 0 && r.state !== "scanning")
                lines.push(qsTr("Last scan: %1").arg(new Date(r.scanned_ms).toLocaleString(Qt.locale(AppLocale.name), Locale.ShortFormat)))
            if (r.oldest_ms > 0)
                lines.push(qsTr("Oldest message scanned: %1").arg(new Date(r.oldest_ms).toLocaleDateString(Qt.locale(AppLocale.name), Locale.ShortFormat)))
            return lines.join("\n")
        }
    }
    AppButton {
        objectName: root.actionName
        kind: "ghost"
        text: root.search.indexing ? qsTr("Cancel indexing")
              : root.search.indexWriteFailed || root.receipt.state === "storage_failed"
                || root.receipt.state === "fetch_failed" ? qsTr("Retry this room")
              : qsTr("Index this room")
        enabled: root.search.indexing || (app.currentRoomId !== "" && root.receipt.state !== "scanning")
        onClicked: {
            if (root.search.indexing) root.search.cancelRoomIndex()
            else root.search.indexRoomHistory(app.currentRoomId)
        }
    }
}
