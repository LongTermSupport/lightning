import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The Activity panel's catch-up summary: unread mentions, replies, thread
// activity and polls, each opening its exact event, and what it cannot see.
Rectangle {
    id: card
    objectName: "catchUpCard"

    property var activity: null
    property int maxVisibleRows: 4
    signal entryOpened()

    readonly property var digest: {
        if (!activity || !activity.catchUp)
            return ({})
        var revision = activity.threadActivityRevision + activity.count
        return activity.catchUp()
    }
    readonly property int total: digest.total || 0
    readonly property var items: digest.items || []
    readonly property var gaps: digest.gaps || []
    readonly property bool hasContent: total > 0

    implicitHeight: content.implicitHeight + AppTheme.spacing8 * 2
    radius: AppTheme.radiusTile
    color: AppTheme.stormInset
    border.width: 1
    border.color: AppTheme.stormBorderStrong

    function fill(template, values) {
        return template.replace(/%([1-9])/g, function(m, d) {
            var i = parseInt(d, 10) - 1
            return i < values.length ? String(values[i]) : m
        })
    }
    function summary() {
        var parts = []
        if (digest.mentions > 0)
            parts.push(qsTr("%n mention(s)", "", digest.mentions))
        if (digest.replies > 0)
            parts.push(qsTr("%n reply(s)", "", digest.replies))
        if (digest.threads > 0)
            parts.push(qsTr("%n thread(s) with new replies", "", digest.threads))
        if (digest.polls > 0)
            parts.push(qsTr("%n poll(s)", "", digest.polls))
        return parts.join(" · ")
    }
    function gapText(code) {
        switch (code) {
        case "history_unavailable":
            return qsTr("Older activity is not available from this server.")
        case "older_not_loaded":
            return qsTr("Only loaded activity is counted. Older activity may be missing.")
        case "capped":
            return qsTr("Activity is at its limit, so the oldest items are not counted.")
        }
        return ""
    }
    function kindLabel(kind, count) {
        switch (kind) {
        case "mention": return qsTr("Mention")
        case "reply": return qsTr("Reply")
        case "thread": return qsTr("%n new thread reply(s)", "", count)
        case "poll": return qsTr("Poll")
        }
        return ""
    }
    function kindIcon(kind) {
        switch (kind) {
        case "mention": return "alternate_email"
        case "reply": return "reply"
        case "thread": return "forum"
        case "poll": return "format_list_bulleted"
        }
        return "notifications"
    }
    function open(item) {
        if (!card.activity)
            return
        card.activity.openEvent(item.roomId, item.eventId, item.threadRootId || "")
        card.entryOpened()
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: AppTheme.spacing8
        spacing: AppTheme.spacing4

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            Icon { name: "done_all"; size: 16; color: AppTheme.stormText }
            Label {
                objectName: "catchUpTitle"
                text: qsTr("Catch up")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                objectName: "catchUpSummary"
                Layout.fillWidth: true
                text: card.summary()
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                elide: Text.ElideRight
            }
        }
        Label {
            objectName: "catchUpUndecryptable"
            visible: (card.digest.undecryptable || 0) > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("%n message(s) could not be decrypted yet.", "",
                       card.digest.undecryptable || 0)
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
        Repeater {
            model: card.gaps
            delegate: Label {
                required property var modelData
                objectName: "catchUpGap"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: card.gapText(modelData)
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
        }
        ListView {
            id: itemList
            objectName: "catchUpItems"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, card.maxVisibleRows * 30)
            clip: true
            interactive: contentHeight > height
            model: card.items
            boundsBehavior: Flickable.StopAtBounds
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Catch up")
            delegate: Rectangle {
                id: itemRow
                required property var modelData
                objectName: "catchUpItem"
                width: ListView.view.width
                implicitHeight: 30
                radius: AppTheme.radiusTile
                color: itemHover.hovered ? AppTheme.stormSelection : "transparent"
                Accessible.role: Accessible.Button
                Accessible.name: card.fill(qsTr("%1 in %2"),
                    [card.kindLabel(modelData.kind, modelData.count), modelData.roomName])
                Accessible.onPressAction: card.open(modelData)
                HoverHandler { id: itemHover }
                TapHandler {
                    gesturePolicy: TapHandler.WithinBounds
                    onTapped: card.open(itemRow.modelData)
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacing4
                    anchors.rightMargin: AppTheme.spacing4
                    spacing: AppTheme.spacing6
                    Icon {
                        name: card.kindIcon(itemRow.modelData.kind)
                        size: 14
                        color: AppTheme.stormTextMuted
                    }
                    Label {
                        text: itemRow.modelData.roomName
                        textFormat: Text.PlainText
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                        elide: Text.ElideRight
                        Layout.maximumWidth: 220
                    }
                    Label {
                        Layout.fillWidth: true
                        text: card.kindLabel(itemRow.modelData.kind, itemRow.modelData.count)
                              + (itemRow.modelData.encrypted ? " · " + qsTr("Encrypted") : "")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textMeta
                        elide: Text.ElideRight
                    }
                }
            }
        }
        Label {
            objectName: "catchUpMore"
            visible: card.total > card.items.length
            text: qsTr("%n more not listed.", "", card.total - card.items.length)
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
        }
    }
}
