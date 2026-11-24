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
    readonly property var community: manager && manager.communityRoomId === roomId
                                     ? manager.communityState : ({})
    readonly property var guide: community.guide || ({})
    readonly property bool hasGuide: !!guide.welcome || !!guide.rules
                                     || (guide.rooms || []).length > 0 || (guide.links || []).length > 0
    readonly property bool busy: manager ? manager.communityBusy : false
    property bool ready: false
    readonly property var recommended: {
        var dependency = community
        return manager ? manager.guideRooms(sections) : []
    }
    signal openRoomRequested(string roomId)
    signal openSpaceRequested(string spaceId)
    signal joinRequested(string roomId, var via, bool isSpace)
    signal knockRequested(string roomId, var via)
    visible: roomId.startsWith("!") && (hasGuide || community.can_edit_guide === true
                                        || community.can_propose === true || community.can_review === true)
    spacing: AppTheme.spacing8
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    function clearDrafts() {
        guideEditor.close()
        announcements.close()
        proposalBody.text = ""
        reviewBody.text = ""
    }
    function reset() {
        clearDrafts()
        if (manager && roomId.startsWith("!")) manager.loadCommunity(roomId)
    }
    onRoomIdChanged: if (ready) reset()
    Component.onCompleted: { ready = true; reset() }
    Connections {
        target: root.manager
        ignoreUnknownSignals: true
        function onCommunityChanged() {
            if (root.ready && (!root.manager || root.manager.communityRoomId === ""))
                root.clearDrafts()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label { textFormat: Text.PlainText; text: qsTr("Start here"); color: AppTheme.textPrimary; font.bold: true }
        Item { Layout.fillWidth: true }
        AppButton {
            objectName: "spaceGuideEdit"
            text: qsTr("Edit guide")
            visible: root.community.can_edit_guide === true
            enabled: !root.busy
            onClicked: {
                welcomeField.text = root.guide.welcome || ""
                rulesField.text = root.guide.rules || ""
                roomsField.text = (root.guide.rooms || []).join("\n")
                linksField.text = (root.guide.links || []).join("\n")
                guideEditor.open()
            }
        }
        AppButton {
            objectName: "spaceAnnouncementsOpen"
            text: qsTr("Announcements")
            visible: root.community.can_propose === true || root.community.can_review === true
                     || root.community.can_enable === true
            enabled: !root.busy
            onClicked: {
                root.manager.communityAction("proposals", {})
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
        RowLayout {
            id: guideRoom
            required property var modelData
            Layout.fillWidth: true
            readonly property bool knock: modelData.joinRule === "knock" || modelData.joinRule === "knock_restricted"
            Label {
                objectName: "spaceGuideRoomName"
                Layout.fillWidth: true
                text: guideRoom.modelData.name || guideRoom.modelData.roomId
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: AppTheme.textPrimary
            }
            AppButton {
                objectName: "spaceGuideRoom"
                text: guideRoom.modelData.joined ? qsTr("Open")
                      : guideRoom.knock ? qsTr("Ask to join") : qsTr("Join")
                Accessible.name: text + " " + (guideRoom.modelData.name || guideRoom.modelData.roomId)
                onClicked: guideRoom.activate()
            }
            function activate() {
                if (modelData.joined) {
                    if (modelData.isSpace) root.openSpaceRequested(modelData.roomId)
                    else root.openRoomRequested(modelData.roomId)
                } else if (knock) {
                    root.knockRequested(modelData.roomId, modelData.via || [])
                } else {
                    root.joinRequested(modelData.roomId, modelData.via || [], modelData.isSpace === true)
                }
            }
        }
    }
    Repeater {
        model: root.guide.links || []
        RowLayout {
            id: guideLink
            required property string modelData
            Layout.fillWidth: true
            Label {
                objectName: "spaceGuideLinkText"
                Layout.fillWidth: true
                text: guideLink.modelData
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: AppTheme.textSecondary
            }
            AppButton {
                objectName: "spaceGuideLink"
                text: qsTr("Open link")
                Accessible.name: text + " " + guideLink.modelData
                // Guide links are other members' state: only https, and through the app's refusal checks.
                enabled: guideLink.modelData.indexOf("https://") === 0
                onClicked: app.media.openWebUrl(guideLink.modelData)
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
                Label { textFormat: Text.PlainText; text: qsTr("Welcome (up to 4000 characters)"); color: AppTheme.stormText }
                TextArea {
                    id: welcomeField; objectName: "spaceGuideWelcome"
                    Layout.fillWidth: true; implicitHeight: 90
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Welcome")
                }
                Label { textFormat: Text.PlainText; text: qsTr("Rules (up to 4000 characters)"); color: AppTheme.stormText }
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
                Label { textFormat: Text.PlainText; text: qsTr("HTTPS links (one per line, up to 8)"); color: AppTheme.stormText }
                TextArea {
                    id: linksField; Layout.fillWidth: true; implicitHeight: 70
                    textFormat: TextEdit.PlainText; Accessible.name: qsTr("Important links")
                }
                RowLayout {
                    AppButton {
                        text: qsTr("Save guide"); objectName: "spaceGuideSave"
                        enabled: !root.busy && root.community.can_edit_guide === true
                                 && welcomeField.text.length <= 4000 && rulesField.text.length <= 4000
                        onClicked: {
                            root.manager.communityAction("guide", { welcome: welcomeField.text,
                                rules: rulesField.text, rooms: root.lines(roomsField.text),
                                links: root.lines(linksField.text) })
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
                    visible: root.community.can_enable === true && root.community.proposals_open !== true
                    enabled: !root.busy
                    onClicked: enableProposals.open()
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: root.community.can_propose !== true
                    text: qsTr("A room administrator must enable proposals for members first.")
                    wrapMode: Text.Wrap; color: AppTheme.stormTextSecondary
                }
                Label { textFormat: Text.PlainText; text: qsTr("Destination room"); color: AppTheme.stormText }
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
                    enabled: !root.busy && root.community.can_propose === true
                             && proposalBody.text.trim().length > 0 && proposalBody.text.length <= 4000
                             && targetRoom.currentIndex >= 0
                    onClicked: root.manager.communityAction("propose", {
                        body: proposalBody.text, target: targetRoom.targets[targetRoom.currentIndex].roomId })
                }
                Label {
                    textFormat: Text.PlainText
                    visible: root.community.truncated === true
                    Layout.fillWidth: true
                    text: qsTr("Only the first 100 proposals are shown.")
                    wrapMode: Text.Wrap; color: AppTheme.stormTextSecondary
                }
                Repeater {
                    model: root.community.proposals || []
                    RowLayout {
                        id: proposalRow
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            objectName: "spaceAnnouncementProposalAuthor"
                            Layout.fillWidth: true
                            text: proposalRow.modelData.published ? qsTr("Published proposal from %1").arg(proposalRow.modelData.author)
                                  : proposalRow.modelData.authorized === false ? qsTr("Proposal from %1 (author can no longer propose)").arg(proposalRow.modelData.author)
                                  : qsTr("Proposal from %1").arg(proposalRow.modelData.author)
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: AppTheme.stormText
                        }
                        AppButton {
                            objectName: "spaceAnnouncementProposal"
                            text: qsTr("Review")
                            enabled: !root.busy && root.community.can_review === true
                                     && !proposalRow.modelData.published && proposalRow.modelData.authorized === true
                            onClicked: {
                                announcements.proposalId = proposalRow.modelData.id
                                reviewBody.text = proposalRow.modelData.body
                                root.manager.invalidateCommunityReview()
                            }
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
                        return qsTr("Publish as %1\nProposed by %2\nDestination: %3 (%4)\nCurrent joined members: %5\n%6")
                            .arg(r.publisher).arg(r.author).arg(a.name || a.room_id).arg(a.room_id).arg(a.joined_count)
                            .arg(root.manager.audiencePreview(a.join_rule || "", a.history || "", a.allow || []))
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
