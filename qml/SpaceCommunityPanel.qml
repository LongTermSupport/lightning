import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root
    objectName: "spaceCommunityPanel"
    property var manager: null
    property var sections: []
    readonly property string roomId: manager ? manager.activeSpaceId : ""
    readonly property var state: manager && manager.communityRoomId === roomId
                                 ? manager.communityState : ({})
    readonly property var guide: state.guide || ({})
    readonly property bool busy: manager ? manager.communityBusy : false
    property var interests: ({})
    property bool ready: false
    readonly property var recommended: {
        var dependency = state
        return manager ? manager.guideRooms(sections) : []
    }
    signal openRoomRequested(string roomId)
    signal openSpaceRequested(string spaceId)
    signal joinRequested(string roomId, var via, bool isSpace)
    signal knockRequested(string roomId, var via)
    visible: roomId.startsWith("!")
    spacing: AppTheme.spacing8
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    function reset() {
        interests = ({})
        guideEditor.close()
        announcements.close()
        proposalBody.text = ""
        reviewBody.text = ""
        if (manager && roomId.startsWith("!")) manager.loadCommunity(roomId)
    }
    onRoomIdChanged: if (ready) reset()
    Component.onCompleted: { ready = true; reset() }
    Connections {
        target: root.manager
        ignoreUnknownSignals: true
        function onCommunityChanged() {
            if (root.ready && (!root.manager || root.manager.communityRoomId === "")) {
                root.interests = ({})
                guideEditor.close()
                announcements.close()
                proposalBody.text = ""
                reviewBody.text = ""
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label { text: qsTr("Start here"); color: AppTheme.textPrimary; font.bold: true }
        Item { Layout.fillWidth: true }
        AppButton {
            objectName: "spaceGuideEdit"
            text: qsTr("Edit guide")
            visible: root.state.can_edit_guide === true
            enabled: !root.busy
            onClicked: {
                welcomeField.text = root.guide.welcome || ""
                rulesField.text = root.guide.rules || ""
                roomsField.text = (root.guide.rooms || []).join("\n")
                linksField.text = (root.guide.links || []).join("\n")
                interestsField.text = (root.guide.interests || []).join("\n")
                guideEditor.open()
            }
        }
        AppButton {
            objectName: "spaceAnnouncementsOpen"
            text: qsTr("Announcements")
            enabled: !root.busy
            onClicked: {
                root.manager.loadCommunity(root.roomId)
                announcements.open()
            }
        }
        AppButton {
            text: qsTr("Refresh")
            enabled: !root.busy
            onClicked: root.manager.loadCommunity(root.roomId)
        }
    }
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.manager ? root.manager.communityMessage : ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.textSecondary
    }
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.guide.welcome || ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.textPrimary
    }
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.guide.rules ? qsTr("Rules\n%1").arg(root.guide.rules) : ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: AppTheme.textSecondary
    }
    Repeater {
        model: root.recommended
        AppButton {
            required property var modelData
            objectName: "spaceGuideRoom"
            Layout.fillWidth: true
            text: modelData.joined ? qsTr("Open %1").arg(modelData.name || modelData.roomId)
                  : qsTr("Join %1…").arg(modelData.name || modelData.roomId)
            onClicked: {
                if (modelData.joined) {
                    if (modelData.isSpace) root.openSpaceRequested(modelData.roomId)
                    else root.openRoomRequested(modelData.roomId)
                } else if (modelData.joinRule === "knock" || modelData.joinRule === "knock_restricted") {
                    root.knockRequested(modelData.roomId, modelData.via || [])
                } else {
                    root.joinRequested(modelData.roomId, modelData.via || [], modelData.isSpace === true)
                }
            }
        }
    }
    Repeater {
        model: root.guide.links || []
        AppButton {
            required property string modelData
            Layout.fillWidth: true
            text: modelData
            onClicked: Qt.openUrlExternally(modelData)
        }
    }
    Label {
        Layout.fillWidth: true
        visible: (root.guide.interests || []).length > 0
        text: qsTr("Your interests (optional; kept only until you leave this Space view)")
        color: AppTheme.textSecondary
        wrapMode: Text.Wrap
    }
    Repeater {
        model: root.guide.interests || []
        CheckBox {
            required property string modelData
            text: modelData
            checked: root.interests[modelData] === true
            onToggled: {
                var next = Object.assign({}, root.interests)
                next[modelData] = checked
                root.interests = next
            }
        }
    }

    function lines(value) {
        return value.split("\n").map(s => s.trim()).filter(s => s.length > 0)
    }

    AppDialog {
        id: guideEditor
        objectName: "spaceGuideEditor"
        parent: Overlay.overlay
        anchors.centerIn: parent
        title: qsTr("Room guide")
        width: Math.min(580, parent ? parent.width - 40 : 580)
        height: Math.min(640, parent ? parent.height - 40 : 640)
        standardButtons: Dialog.NoButton
        onOpened: welcomeField.forceActiveFocus()
        contentItem: ScrollView {
            clip: true
            ColumnLayout {
                width: parent.width
                Label {
                    Layout.fillWidth: true
                    text: qsTr("The guide is shared room state. Servers and people allowed to read this Space's state can read it.")
                    textFormat: Text.PlainText; wrapMode: Text.Wrap
                    color: AppTheme.stormTextSecondary
                }
                Label { text: qsTr("Welcome (up to 4000 characters)"); color: AppTheme.stormText }
                TextArea {
                    id: welcomeField; objectName: "spaceGuideWelcome"
                    Layout.fillWidth: true; implicitHeight: 90
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Welcome")
                }
                Label { text: qsTr("Rules (up to 4000 characters)"); color: AppTheme.stormText }
                TextArea {
                    id: rulesField; Layout.fillWidth: true; implicitHeight: 90
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Rules")
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Recommended room IDs in order (one per line, up to 12). Readers see only rooms already visible in their lobby.")
                    textFormat: Text.PlainText; wrapMode: Text.Wrap; color: AppTheme.stormText
                }
                TextArea {
                    id: roomsField; Layout.fillWidth: true; implicitHeight: 70
                    textFormat: TextEdit.PlainText; Accessible.name: qsTr("Recommended rooms")
                }
                Label { text: qsTr("HTTPS links (one per line, up to 8)"); color: AppTheme.stormText }
                TextArea {
                    id: linksField; Layout.fillWidth: true; implicitHeight: 70
                    textFormat: TextEdit.PlainText; Accessible.name: qsTr("Important links")
                }
                Label { text: qsTr("Optional interests (one per line, up to 12)"); color: AppTheme.stormText }
                TextArea {
                    id: interestsField; Layout.fillWidth: true; implicitHeight: 70
                    textFormat: TextEdit.PlainText; Accessible.name: qsTr("Optional interests")
                }
                RowLayout {
                    AppButton {
                        text: qsTr("Save guide"); objectName: "spaceGuideSave"
                        enabled: !root.busy && root.state.can_edit_guide === true
                                 && welcomeField.text.length <= 4000 && rulesField.text.length <= 4000
                        onClicked: {
                            root.manager.communityAction("guide", { welcome: welcomeField.text,
                                rules: rulesField.text, rooms: root.lines(roomsField.text),
                                links: root.lines(linksField.text), interests: root.lines(interestsField.text) })
                            guideEditor.close()
                        }
                    }
                    AppButton { text: qsTr("Cancel"); onClicked: guideEditor.close() }
                }
            }
        }
    }

    AppDialog {
        id: announcements
        objectName: "spaceAnnouncementDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        title: qsTr("Announcement review")
        width: Math.min(600, parent ? parent.width - 40 : 600)
        height: Math.min(680, parent ? parent.height - 40 : 680)
        standardButtons: Dialog.NoButton
        property string proposalId: ""
        readonly property var review: root.manager ? root.manager.communityReview : ({})
        onOpened: proposalBody.forceActiveFocus()
        onClosed: {
            proposalId = ""
            proposalBody.text = ""
            reviewBody.text = ""
            if (root.manager) root.manager.invalidateCommunityReview()
        }
        contentItem: ScrollView {
            clip: true
            ColumnLayout {
                width: parent.width
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Submitting shares your proposal as unencrypted Space state with the server and authorized readers. Only a reviewer can publish through this workflow, using their own identity. Keep private drafts out of shared state.")
                    textFormat: Text.PlainText; wrapMode: Text.Wrap; color: AppTheme.stormTextSecondary
                }
                AppButton {
                    text: qsTr("Enable member proposals…")
                    visible: root.state.can_enable === true
                    enabled: !root.busy
                    onClicked: enableProposals.open()
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.state.can_propose !== true
                    text: qsTr("A room administrator must enable proposals for members first.")
                    wrapMode: Text.Wrap; color: AppTheme.stormTextSecondary
                }
                Label { text: qsTr("Destination room"); color: AppTheme.stormText }
                AppComboBox {
                    id: targetRoom
                    objectName: "spaceAnnouncementTarget"
                    Layout.fillWidth: true
                    property var targets: {
                        var rows = []
                        for (var section of root.sections)
                            for (var row of (section.rows || []))
                                if (row.joined && !row.isSpace && !rows.some(r => r.roomId === row.roomId)) rows.push(row)
                        return rows
                    }
                    model: targets
                    textRole: "name"
                }
                TextArea {
                    id: proposalBody; objectName: "spaceAnnouncementDraft"
                    Layout.fillWidth: true; implicitHeight: 100
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Announcement proposal")
                }
                AppButton {
                    text: qsTr("Submit or replace my proposal")
                    enabled: !root.busy && root.state.can_propose === true
                             && proposalBody.text.trim().length > 0 && proposalBody.text.length <= 4000
                             && targetRoom.currentIndex >= 0
                    onClicked: root.manager.communityAction("propose", {
                        body: proposalBody.text, target: targetRoom.targets[targetRoom.currentIndex].roomId })
                }
                Label {
                    visible: root.state.truncated === true
                    Layout.fillWidth: true
                    text: qsTr("Only the first 100 proposals are shown.")
                    wrapMode: Text.Wrap; color: AppTheme.stormTextSecondary
                }
                Repeater {
                    model: root.state.proposals || []
                    AppButton {
                        required property var modelData
                        objectName: "spaceAnnouncementProposal"
                        Layout.fillWidth: true
                        text: modelData.published ? qsTr("Published proposal from %1").arg(modelData.author)
                              : qsTr("Review proposal from %1").arg(modelData.author)
                        enabled: !root.busy && root.state.can_review === true && !modelData.published
                        onClicked: {
                            announcements.proposalId = modelData.id
                            reviewBody.text = modelData.body
                            root.manager.invalidateCommunityReview()
                        }
                    }
                }
                TextArea {
                    id: reviewBody; objectName: "spaceAnnouncementFinalText"
                    Layout.fillWidth: true; implicitHeight: 100
                    visible: announcements.proposalId.length > 0
                    enabled: !root.busy && !announcements.review.approval
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Final announcement text")
                    onTextChanged: if (root.manager) root.manager.invalidateCommunityReview()
                }
                AppButton {
                    text: qsTr("Preview exact text and audience")
                    visible: announcements.proposalId.length > 0
                    enabled: !root.busy && reviewBody.text.trim().length > 0 && reviewBody.text.length <= 4000
                    onClicked: root.manager.communityAction("preview", {
                        id: announcements.proposalId, body: reviewBody.text })
                }
                Label {
                    objectName: "spaceAnnouncementReviewText"
                    Layout.fillWidth: true
                    visible: !!announcements.review.approval
                    text: announcements.review.body || ""
                    textFormat: Text.PlainText; wrapMode: Text.Wrap; color: AppTheme.stormText
                }
                Label {
                    objectName: "spaceAnnouncementAudience"
                    Layout.fillWidth: true
                    visible: !!announcements.review.approval
                    textFormat: Text.PlainText; wrapMode: Text.Wrap; color: AppTheme.stormTextSecondary
                    text: {
                        var r = announcements.review
                        if (!r.audience) return ""
                        var a = r.audience
                        var allowed = (a.join_rules.allow || []).filter(r => r.type === "m.room_membership").map(r => r.room_id)
                        return qsTr("Publish as %1\nProposed by %2\nDestination: %3 (%4)\nCurrent joined members: %5\n%6")
                            .arg(r.publisher).arg(r.author).arg(a.name || a.room_id).arg(a.room_id).arg(a.joined_count)
                            .arg(root.manager.audiencePreview(a.join_rules.join_rule || "", a.history.history_visibility || "", allowed))
                    }
                }
                RowLayout {
                    AppButton {
                        objectName: "spaceAnnouncementPublish"
                        text: qsTr("Approve and publish")
                        visible: !!announcements.review.approval
                        enabled: !root.busy
                        onClicked: root.manager.communityAction("publish", {})
                    }
                    AppButton {
                        text: qsTr("Edit final text")
                        visible: !!announcements.review.approval
                        enabled: !root.busy
                        onClicked: root.manager.invalidateCommunityReview()
                    }
                    AppButton { text: qsTr("Close"); onClicked: announcements.close() }
                }
            }
        }
    }
    AppDialog {
        id: enableProposals
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(440, parent ? parent.width - 40 : 440)
        title: qsTr("Enable member proposals?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            width: parent.width
            text: qsTr("This sets the announcement proposal event permission to 0. Members at that level can submit shared proposals. The reviewer permission and ordinary message permissions stay as they are.")
            textFormat: Text.PlainText; wrapMode: Text.Wrap; color: AppTheme.stormText
        }
        onAccepted: root.manager.communityAction("enable", {})
    }
}
