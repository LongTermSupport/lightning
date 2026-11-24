import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    objectName: "notificationRulesSection"

    Layout.fillWidth: true
    spacing: AppTheme.spacing12

    readonly property var rules: (typeof app !== "undefined" && app)
                                 ? app.pushRules : null
    property string shootNote: ""
    readonly property int shootProblems: countStatus(2)
    readonly property int shootWarnings: countStatus(1)
    function countStatus(status) {
        var list = root.shoot ? root.shoot.checks : []
        var n = 0
        for (var i = 0; i < list.length; ++i)
            if (list[i].status === status)
                ++n
        return n
    }
    readonly property var shoot: (typeof app !== "undefined" && app)
                                 ? app.notificationTroubleshooter : null
    readonly property bool supported: root.rules ? root.rules.supported === true
                                                 : false

    // The rule sections are shown only once this account's rules have really
    // arrived: before that the controller holds placeholders (nothing muted,
    // everything "All messages", no keywords) that read as the account's state.
    readonly property bool ready: root.supported && root.rules.loaded === true

    Connections {
        target: root.rules
        enabled: root.rules !== null
        function onStateChanged() {
            if (!root.shoot)
                return
            if (!root.rules.loaded)
                root.shoot.reset()
            else if (root.shoot.hasRun && !root.rules.busy)
                root.shoot.run(app.currentRoomId)
        }
        function onWriteFailed() {
            if (root.shoot && root.shoot.hasRun)
                root.shoot.run(app.currentRoomId)
        }
    }

    onVisibleChanged: if (visible && root.rules) root.rules.refresh()
    Component.onCompleted: if (root.rules) root.rules.refresh()

    component RulesCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    component SubTitle: Label {
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }

    component LevelRow: RowLayout {
        id: levelRow
        property string title
        property var options: [qsTr("Off"), qsTr("Notify"), qsTr("Loud")]
        property int level: 0
        property string pickerName
        property int comboWidth: 180
        signal picked(int index)
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        Label {
            Layout.fillWidth: true
            text: levelRow.title
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            elide: Label.ElideRight
        }
        AppComboBox {
            storm: true
            objectName: levelRow.pickerName
            Layout.preferredWidth: levelRow.comboWidth
            enabled: root.ready
            model: levelRow.options
            Accessible.name: levelRow.title
            currentIndex: Math.min(Math.max(levelRow.level, 0),
                                   levelRow.options.length - 1)
            onActivated: (index) => levelRow.picked(index)
        }
    }

    Label {
        objectName: "pushRulesLoading"
        visible: root.supported && !root.ready
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Loading your account's notification rules…")
    }

    Label {
        text: qsTr("Account notification rules")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.ready
    }

    RulesCard {
        visible: root.ready
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing12

            Hint {
                text: qsTr("These are saved to your Matrix account, so they "
                           + "apply on every device and in every client you "
                           + "use. A room you have set up yourself keeps its "
                           + "own level whatever is chosen here.")
            }
            Label {
                objectName: "pushRulesError"
                visible: root.rules.errorText.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
                text: root.rules.errorText
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("Mute all notifications")
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightMedium
                    }
                    Hint {
                        text: qsTr("Silences every room on every device until "
                                   + "you turn it off.")
                    }
                }
                AppSwitch {
                    objectName: "pushMasterMuteSwitch"
                    checked: root.rules.masterMute
                    Accessible.name: qsTr("Mute all notifications")
                    onToggled: root.rules.setMasterMute(!root.rules.masterMute)
                }
            }
        }
    }

    Label {
        text: qsTr("Default alerts")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.ready
    }
    RulesCard {
        visible: root.ready
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            LevelRow {
                title: qsTr("Direct messages")
                pickerName: "pushDmDefaultCombo"
                comboWidth: 230
                options: [qsTr("All messages"), qsTr("Mentions and keywords")]
                level: root.rules.dmAll ? 0 : 1
                onPicked: (i) => root.rules.setDefaultAll(true, i === 0)
            }
            LevelRow {
                title: qsTr("Rooms")
                pickerName: "pushRoomsDefaultCombo"
                comboWidth: 230
                options: [qsTr("All messages"), qsTr("Mentions and keywords")]
                level: root.rules.roomsAll ? 0 : 1
                onPicked: (i) => root.rules.setDefaultAll(false, i === 0)
            }
            Hint {
                objectName: "pushDriftNote"
                visible: root.rules.dmDrift || root.rules.roomsDrift
                color: AppTheme.warning
                text: qsTr("Encrypted and unencrypted rooms were set to "
                           + "different levels, probably by another client. "
                           + "Choosing a level here sets both.")
            }
            Hint {
                text: qsTr("This is the level for rooms that have no setting "
                           + "of their own. Set a single room from its Room "
                           + "information.")
            }
        }
    }

    Label {
        text: qsTr("Mentions")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.ready
    }
    RulesCard {
        visible: root.ready
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            LevelRow {
                visible: root.rules.mentionUserId >= 0
                title: qsTr("Your user ID")
                pickerName: "pushMentionUserIdCombo"
                level: root.rules.mentionUserId
                onPicked: (i) => root.rules.setMention("userId", i)
            }
            LevelRow {
                visible: root.rules.mentionDisplayName >= 0
                title: qsTr("Your display name")
                pickerName: "pushMentionDisplayNameCombo"
                level: root.rules.mentionDisplayName
                onPicked: (i) => root.rules.setMention("displayName", i)
            }
            LevelRow {
                visible: root.rules.mentionUsername >= 0
                title: qsTr("Your username")
                pickerName: "pushMentionUsernameCombo"
                level: root.rules.mentionUsername
                onPicked: (i) => root.rules.setMention("username", i)
            }
            LevelRow {
                visible: root.rules.mentionRoom >= 0
                title: qsTr("@room")
                pickerName: "pushMentionRoomCombo"
                level: root.rules.mentionRoom
                onPicked: (i) => root.rules.setMention("room", i)
            }
            Hint {
                text: qsTr("Notify alerts you. Loud alerts you with a sound. "
                           + "Off ignores that kind of mention. These apply "
                           + "in every room that is not muted.")
            }
        }
    }

    Label {
        text: qsTr("Keywords")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.ready
    }
    RulesCard {
        visible: root.ready
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: keywordField
                    objectName: "pushKeywordField"
                    storm: true
                    Layout.fillWidth: true
                    placeholderText: qsTr("Add a word or phrase")
                    maximumLength: 120
                    Accessible.name: qsTr("New keyword")
                    onAccepted: addButton.add()
                    onTextChanged: keywordNote.text = ""
                }
                AppComboBox {
                    id: keywordLevel
                    storm: true
                    objectName: "pushKeywordLevelCombo"
                    // Wide enough for the longest translated level.
                    Layout.preferredWidth: Math.max(
                        120, Math.ceil(Math.max(...model.map(t => keywordLevelMetrics.advanceWidth(t))))
                             + leftPadding + rightPadding
                             + contentItem.leftPadding + contentItem.rightPadding + 2)
                    FontMetrics {
                        id: keywordLevelMetrics
                        font: keywordLevel.font
                    }
                    model: [qsTr("Off"), qsTr("Notify"), qsTr("Loud")]
                    currentIndex: 1
                    Accessible.name: qsTr("Level for the new keyword")
                }
                AppButton {
                    id: addButton
                    objectName: "pushKeywordAddButton"
                    storm: true
                    text: qsTr("Add")
                    enabled: keywordField.text.trim().length > 0
                    function add() {
                        if (!enabled)
                            return
                        if (root.rules.addKeyword(keywordField.text,
                                                  keywordLevel.currentIndex))
                            keywordField.clear()
                        else
                            keywordNote.text = root.rules.errorText
                    }
                    onClicked: add()
                }
            }
            Label {
                id: keywordNote
                objectName: "pushKeywordNote"
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
            Hint {
                visible: root.rules.keywords.length === 0
                text: qsTr("No keywords. Add a word and you are notified "
                           + "whenever someone says it.")
            }
            Repeater {
                model: root.rules.keywords
                delegate: LevelRow {
                    required property var modelData
                    title: modelData.keyword
                    pickerName: "pushKeywordRow"
                    comboWidth: 110
                    level: modelData.level
                    onPicked: (i) =>
                        root.rules.setKeywordLevel(modelData.keyword, i)
                    AppButton {
                        storm: true
                        size: "sm"
                        text: qsTr("Remove")
                        Accessible.name:
                            qsTr("Remove keyword %1").arg(modelData.keyword)
                        objectName: "pushKeywordRemoveButton"
                        onClicked: {
                            removeKeywordConfirm.keyword = modelData.keyword
                            removeKeywordConfirm.open()
                        }
                    }
                }
            }
            Hint {
                text: qsTr("Your homeserver cannot read encrypted messages, so "
                           + "in encrypted rooms a keyword matches only on "
                           + "devices that can decrypt them.")
            }
        }
    }

    Label {
        text: qsTr("Other events")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.ready
              && (root.rules.eventReactions >= 0
                  || root.rules.eventInvites >= 0
                  || root.rules.eventNotices >= 0)
    }
    RulesCard {
        visible: root.ready
              && (root.rules.eventReactions >= 0
                  || root.rules.eventInvites >= 0
                  || root.rules.eventNotices >= 0)
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            LevelRow {
                visible: root.rules.eventReactions >= 0
                title: qsTr("Reactions to your messages")
                pickerName: "pushEventReactionsCombo"
                options: [qsTr("Off"), qsTr("Notify")]
                level: root.rules.eventReactions
                onPicked: (i) => root.rules.setEventRule("reactions", i)
            }
            Hint {
                objectName: "pushReactionsNote"
                visible: root.rules.eventReactions >= 0
                text: qsTr("Off also hides reactions from Lightning's Activity "
                           + "feed, including the Reactions tab and its badge. "
                           + "This rule is saved to your account for other "
                           + "devices and clients. Lightning does not show "
                           + "desktop notifications for reactions.")
            }
            LevelRow {
                visible: root.rules.eventInvites >= 0
                title: qsTr("Invitations to you")
                pickerName: "pushEventInvitesCombo"
                options: [qsTr("Off"), qsTr("Notify")]
                level: root.rules.eventInvites
                onPicked: (i) => root.rules.setEventRule("invites", i)
            }
            LevelRow {
                visible: root.rules.eventNotices >= 0
                title: qsTr("Messages from bots")
                pickerName: "pushEventNoticesCombo"
                options: [qsTr("Off"), qsTr("Notify")]
                level: root.rules.eventNotices
                onPicked: (i) => root.rules.setEventRule("notices", i)
            }
            Hint {
                text: qsTr("Bot messages are the automated notices some "
                           + "bridges and bots post. Off keeps them quiet; "
                           + "Notify treats them like any other message in "
                           + "that room.")
            }
        }
    }

    component SwitchRow: RowLayout {
        id: switchRow
        property string title
        property bool checked: false
        property string switchName
        signal toggled(bool on)
        Layout.fillWidth: true
        spacing: AppTheme.spacing12
        Label {
            Layout.fillWidth: true
            text: switchRow.title
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            wrapMode: Text.WordWrap
        }
        AppSwitch {
            objectName: switchRow.switchName
            checked: switchRow.checked
            Accessible.name: switchRow.title
            onToggled: switchRow.toggled(!switchRow.checked)
        }
    }

    function timeLabel(minute) {
        return Qt.formatTime(new Date(2000, 0, 1, Math.floor(minute / 60),
                                      minute % 60),
                             app.settings.clockTimeFormat)
    }
    readonly property var halfHours: {
        var out = []
        for (var m = 0; m < 24 * 60; m += 30)
            out.push(root.timeLabel(m))
        return out
    }

    Label {
        text: qsTr("Quiet hours")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: typeof app !== "undefined" && !!app && !!app.settings
    }
    RulesCard {
        objectName: "quietHoursCard"
        visible: typeof app !== "undefined" && !!app && !!app.settings
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            Hint {
                text: qsTr("On this device only, notifications are held "
                           + "during these hours and summed up when they end. "
                           + "Activity and unread counts still update, and "
                           + "calls still ring.")
            }
            SwitchRow {
                title: qsTr("Quiet hours")
                switchName: "quietHoursSwitch"
                checked: app.settings.quietHoursEnabled
                onToggled: (on) => app.settings.quietHoursEnabled = on
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                enabled: app.settings.quietHoursEnabled
                Label {
                    text: qsTr("From")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                }
                AppComboBox {
                    storm: true
                    objectName: "quietHoursStartCombo"
                    Layout.preferredWidth: 120
                    model: root.halfHours
                    Accessible.name: qsTr("Quiet hours start")
                    currentIndex: Math.floor(app.settings.quietHoursStart / 30)
                    onActivated: (index) => app.settings.quietHoursStart = index * 30
                }
                Label {
                    text: qsTr("to")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                }
                AppComboBox {
                    storm: true
                    objectName: "quietHoursEndCombo"
                    Layout.preferredWidth: 120
                    model: root.halfHours
                    Accessible.name: qsTr("Quiet hours end")
                    currentIndex: Math.floor(app.settings.quietHoursEnd / 30)
                    onActivated: (index) => app.settings.quietHoursEnd = index * 30
                }
                Item { Layout.fillWidth: true }
            }
            SubTitle {
                text: qsTr("Still notify for")
                enabled: app.settings.quietHoursEnabled
            }
            SwitchRow {
                title: qsTr("Mentions of you")
                switchName: "quietHoursMentionsSwitch"
                enabled: app.settings.quietHoursEnabled
                checked: app.settings.quietHoursAllowMentions
                onToggled: (on) => app.settings.quietHoursAllowMentions = on
            }
            SwitchRow {
                title: qsTr("Direct messages")
                switchName: "quietHoursDirectSwitch"
                enabled: app.settings.quietHoursEnabled
                checked: app.settings.quietHoursAllowDirect
                onToggled: (on) => app.settings.quietHoursAllowDirect = on
            }
            Hint {
                objectName: "quietHoursRoomsHint"
                text: app.settings.quietHoursRooms.length === 0
                      ? qsTr("Choose rooms that may still notify from a "
                             + "room's menu.")
                      : qsTr("%n chosen room(s) may still notify. Change them "
                             + "from a room's menu.", "",
                             app.settings.quietHoursRooms.length)
            }
            Label {
                objectName: "quietHoursPreview"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: app.settings.quietHoursEnabled
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                text: (app.quietHoursActive
                       ? qsTr("Quiet now, until %1.")
                       : qsTr("Next quiet hours start at %1."))
                      .arg(root.timeLabel(app.quietHoursActive
                                          ? app.settings.quietHoursEnd
                                          : app.settings.quietHoursStart))
                      + " " + qsTr("Who can interrupt: %1.").arg(
                          [qsTr("calls")]
                          .concat(app.settings.quietHoursAllowMentions
                                  ? [qsTr("mentions")] : [])
                          .concat(app.settings.quietHoursAllowDirect
                                  ? [qsTr("direct messages")] : [])
                          .concat(app.settings.quietHoursRooms.length > 0
                                  ? [qsTr("chosen rooms")] : [])
                          .join(", "))
            }
            SubTitle {
                objectName: "snoozedTitle"
                visible: !!app.reminders && app.reminders.snoozed.length > 0
                text: qsTr("Snoozed conversations")
            }
            Repeater {
                model: app.reminders ? app.reminders.snoozed : []
                delegate: RowLayout {
                    id: snoozedRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1, until %2").arg(snoozedRow.modelData.roomName)
                              .arg(Qt.formatDateTime(new Date(snoozedRow.modelData.until),
                                                     app.settings.clockTimeFormat))
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        elide: Label.ElideRight
                    }
                    AppButton {
                        storm: true
                        size: "sm"
                        text: qsTr("Unsnooze")
                        onClicked: app.reminders.unsnooze(snoozedRow.modelData.roomId)
                    }
                }
            }
        }
    }

    Label {
        text: qsTr("Not getting notifications?")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textTitle
        font.weight: AppTheme.weightStrong
        Layout.topMargin: AppTheme.spacing8
        visible: root.shoot !== null
    }
    RulesCard {
        visible: root.shoot !== null
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8

            Hint {
                text: qsTr("Checks the things Lightning can see: whether "
                           + "something can show a notification, the "
                           + "switches here, your account, and the room you "
                           + "have open. The test notification covers the "
                           + "rest.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    objectName: "troubleshooterRunButton"
                    storm: true
                    text: qsTr("Run checks")
                    onClicked: {
                        root.shootNote = "checks"
                        root.shoot.run(app.currentRoomId)
                    }
                }
                AppButton {
                    objectName: "troubleshooterTestButton"
                    storm: true
                    text: qsTr("Send a test notification")
                    onClicked: {
                        root.shootNote = "test"
                        root.shoot.sendTest()
                    }
                }
                Label {
                    objectName: "troubleshooterStatus"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    wrapMode: Text.WordWrap
                    color: root.shootProblems > 0 ? AppTheme.stormDanger
                         : root.shootWarnings > 0 ? AppTheme.warning
                         : AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textBody
                    text: root.shootNote === "test"
                          ? qsTr("Test sent. If no notification appeared, "
                                 + "your desktop is not showing them.")
                          : root.shootNote === "checks" && root.shoot.hasRun
                            ? (root.shootProblems > 0
                               ? (root.shootProblems === 1
                                  ? qsTr("1 problem found. Details below.")
                                  : qsTr("%1 problems found. Details below.")
                                      .arg(root.shootProblems))
                               : root.shootWarnings > 0
                                 ? (root.shootWarnings === 1
                                    ? qsTr("1 thing to check. Details below.")
                                    : qsTr("%1 things to check. Details below.")
                                        .arg(root.shootWarnings))
                                 : qsTr("Nothing wrong that Lightning can see. "
                                        + "Send a test notification to be sure."))
                            : ""
                }
            }
            SubTitle {
                text: qsTr("Test a message against your rules")
                Layout.topMargin: AppTheme.spacing8
            }
            Hint {
                text: qsTr("Asks your account's rules what they would do with "
                           + "a message in the room you have open. Nothing is "
                           + "sent and no rule is changed.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: sampleField
                    objectName: "ruleSimulatorText"
                    storm: true
                    Layout.fillWidth: true
                    text: qsTr("Hello")
                    maximumLength: 500
                    Accessible.name: qsTr("Sample message")
                    onAccepted: simulateButton.clicked()
                }
                AppSwitch {
                    id: sampleMention
                    objectName: "ruleSimulatorMention"
                    Accessible.name: qsTr("Mentions me")
                }
                Label {
                    text: qsTr("Mentions me")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                }
                AppButton {
                    id: simulateButton
                    objectName: "ruleSimulatorButton"
                    storm: true
                    text: qsTr("Check")
                    onClicked: root.shoot.simulate(app.currentRoomId, sampleField.text,
                                                   sampleMention.checked)
                }
            }
            Label {
                objectName: "ruleSimulatorResult"
                readonly property var sim: root.shoot ? root.shoot.simulation : ({})
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
                text: sim.state === "noRoom"
                      ? qsTr("Open a room first.")
                      : sim.state === "unsupported"
                        ? qsTr("This account cannot check its rules here.")
                        : sim.state === "pending" ? qsTr("Checking…")
                        : sim.state === "failed"
                          ? qsTr("The rules could not be read. Try again.")
                        : sim.state === "done"
                          ? ((sim.matched
                              ? qsTr("Rule: %1.").arg(sim.ruleName)
                              : qsTr("No rule matches.")) + " "
                             + (!sim.notify ? qsTr("It would not notify.")
                                : sim.highlight ? qsTr("It would notify and highlight.")
                                : qsTr("It would notify."))
                             + (sim.notify
                                ? " " + (sim.local.length > 0 ? sim.local
                                         : qsTr("Lightning would show it."))
                                : ""))
                        : ""
            }
            Repeater {
                model: root.shoot ? root.shoot.checks : []
                delegate: ColumnLayout {
                    id: checkRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 2
                    readonly property int status: modelData.status
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Label {
                            text: checkRow.status === 0 ? qsTr("OK")
                                : checkRow.status === 1 ? qsTr("Check")
                                : checkRow.status === 2 ? qsTr("Problem")
                                : qsTr("Unknown")
                            color: checkRow.status === 0 ? AppTheme.stormSuccess
                                 : checkRow.status === 2 ? AppTheme.stormDanger
                                 : checkRow.status === 1 ? AppTheme.warning
                                 : AppTheme.stormTextSecondary
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                            Layout.preferredWidth: 64
                        }
                        Label {
                            Layout.fillWidth: true
                            text: checkRow.modelData.title
                            color: AppTheme.stormText
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                        }
                        AppButton {
                            visible: checkRow.modelData.fix === "enableApp"
                                  || checkRow.modelData.fix === "unmute"
                            storm: true
                            size: "sm"
                            text: checkRow.modelData.fix === "enableApp"
                                  ? qsTr("Turn on") : qsTr("Unmute")
                            onClicked: {
                                if (checkRow.modelData.fix === "enableApp") {
                                    app.settings.notificationsEnabled = true
                                    root.shoot.run(app.currentRoomId)
                                } else {
                                    root.rules.setMasterMute(false)
                                }
                            }
                        }
                    }
                    Hint {
                        Layout.leftMargin: 64 + AppTheme.spacing8
                        text: checkRow.modelData.detail
                    }
                }
            }
        }
    }

    Dialog {
        id: removeKeywordConfirm
        objectName: "pushKeywordRemoveConfirm"
        property string keyword: ""
        title: qsTr("Remove this keyword?")
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 360
        padding: AppTheme.spacing20
        closePolicy: Popup.CloseOnEscape
        // Default to the safe choice: a lone Space must not confirm a removal.
        onOpened: removeCancelButton.forceActiveFocus(Qt.TabFocusReason)
        onAccepted: {
            if (root.rules && removeKeywordConfirm.keyword.length > 0)
                root.rules.removeKeyword(removeKeywordConfirm.keyword)
        }
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            border.width: 1
            radius: AppTheme.radiusLg
        }
        header: Label {
            text: removeKeywordConfirm.title
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing20
            rightPadding: AppTheme.spacing20
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }
        footer: Item {
            implicitHeight: removeKeywordFooter.implicitHeight
                            + AppTheme.spacing20 * 2
            RowLayout {
                id: removeKeywordFooter
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacing20
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacing8
                AppButton {
                    id: removeCancelButton
                    objectName: "pushKeywordRemoveCancel"
                    storm: true
                    text: qsTr("Cancel")
                    onClicked: removeKeywordConfirm.reject()
                }
                AppButton {
                    objectName: "pushKeywordRemoveConfirmButton"
                    storm: true
                    kind: "dangerPrimary"
                    text: qsTr("Remove keyword")
                    onClicked: removeKeywordConfirm.accept()
                }
            }
        }
        Label {
            objectName: "pushKeywordRemoveBody"
            width: 300
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            maximumLineCount: 6
            elide: Text.ElideRight
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("You will stop being notified when someone says "
                       + "“%1”, on every device signed in to this "
                       + "account.").arg(removeKeywordConfirm.keyword)
        }
    }
}
