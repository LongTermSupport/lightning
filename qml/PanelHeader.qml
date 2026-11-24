import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A side panel's header band: the panel's own content, then the close button
// on the same rail as the room header's last action.
Rectangle {
    id: root

    default property alias content: contentRow.data
    property alias closeButton: closeButton
    property string closeLabel: ""
    property int leadingInset: AppTheme.spacing16
    signal closeClicked()

    Layout.fillWidth: true
    implicitHeight: AppTheme.headerBandHeight
    color: "transparent"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.leadingInset
        anchors.rightMargin: AppTheme.headerActionGutter
        spacing: AppTheme.spacing8

        RowLayout {
            id: contentRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8
        }
        IconButton {
            id: closeButton
            iconName: "close"
            Accessible.name: root.closeLabel
            ToolTip.text: root.closeLabel
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: root.closeClicked()
        }
    }
}
