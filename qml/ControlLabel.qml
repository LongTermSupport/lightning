import QtQuick
import QtQuick.Controls

// A single-line control label whose cap-height ink sits on centreIn's vertical
// centre. Centring the line box leaves the ink high. Not for wrapping text.
Label {
    id: root

    // The parent, or the parent's parent (a Row/RowLayout inside the control).
    property Item centreIn: parent

    // A property, not FontMetrics.tightBoundingRect(): a method call is not
    // re-evaluated when the font changes.
    readonly property rect capInk: capMetrics.tightBoundingRect
    readonly property real inkCentreY: baselineOffset + capInk.y + capInk.height / 2

    TextMetrics { id: capMetrics; font: root.font; text: "H" }

    transform: Translate {
        y: {
            if (!root.centreIn || !root.parent)
                return 0
            const top = root.y + (root.parent === root.centreIn ? 0 : root.parent.y)
            const dpr = root.Screen.devicePixelRatio || 1
            return Math.round((root.centreIn.height / 2 - top - root.inkCentreY) * dpr) / dpr
        }
    }
}
