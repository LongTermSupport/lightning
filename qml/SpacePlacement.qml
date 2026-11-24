import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Where a room or Space sits among the joined Spaces: a Space's primary path,
// then every other Space that lists it directly. Navigation only.
ColumnLayout {
    id: root
    objectName: "spacePlacement"
    spacing: AppTheme.spacingXS

    property string targetId: ""
    signal openSpaceRequested(string spaceId)

    // Assigned by refresh(), never bound: the inputs come from C++ calls.
    property var pathIds: []
    property var otherIds: []
    property var pathLinks: []
    property var otherLinks: []
    readonly property var spaces: typeof app !== "undefined" && app ? app.spaces : null

    visible: pathIds.length + otherIds.length > 0

    function links(ids, separator) {
        return ids.map(function (id, i) {
            return { spaceId: id, name: root.spaces.spaceName(id),
                     text: (i > 0 ? separator : "") + root.spaces.spaceName(id) }
        })
    }
    function refresh() {
        if (!spaces || targetId === "") {
            pathIds = []
            otherIds = []
        } else {
            var path = spaces.ancestorSpaceIds(targetId).slice().reverse()
            var primary = path.length > 0 ? path[path.length - 1] : ""
            pathIds = path
            otherIds = spaces.directParentSpaceIds(targetId).filter(function (id) {
                return id !== primary
            })
        }
        pathLinks = links(pathIds, "› ")
        otherLinks = links(otherIds, "")
    }
    onTargetIdChanged: refresh()
    onSpacesChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: root.spaces
        ignoreUnknownSignals: true
        function onSpacesChanged() { root.refresh() }
    }

    // Space names are remote text: a plain-text label, never AutoText.
    Component {
        id: linkDelegate
        AbstractButton {
            id: link
            required property var modelData
            objectName: "spacePlacementLink"
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            height: AppTheme.scaled(24)
            Accessible.role: Accessible.Link
            Accessible.name: qsTr("Open %1").arg(link.modelData.name)
            contentItem: Label {
                textFormat: Text.PlainText
                text: link.modelData.text
                color: AppTheme.link
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.underline: link.hovered || link.visualFocus
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: root.openSpaceRequested(link.modelData.spaceId)
        }
    }

    Flow {
        objectName: "spacePlacementPath"
        Layout.fillWidth: true
        spacing: AppTheme.spacingXS
        visible: root.pathLinks.length > 0
        Label {
            text: qsTr("In")
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            height: AppTheme.scaled(24)
            verticalAlignment: Text.AlignVCenter
        }
        Repeater {
            model: root.pathLinks
            delegate: linkDelegate
        }
    }
    Flow {
        objectName: "spacePlacementOthers"
        Layout.fillWidth: true
        spacing: AppTheme.spacingXS
        visible: root.otherLinks.length > 0
        Label {
            text: root.pathLinks.length > 0 ? qsTr("Also in") : qsTr("In")
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
            height: AppTheme.scaled(24)
            verticalAlignment: Text.AlignVCenter
        }
        Repeater {
            model: root.otherLinks
            delegate: linkDelegate
        }
    }
}
