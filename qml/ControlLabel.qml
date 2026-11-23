import QtQuick
import QtQuick.Controls

// A single-line control label whose cap-height ink sits on centreIn's vertical
// centre. Centring the line box leaves the ink high. Not for wrapping text.
Label {
    id: root

    // The parent, or the parent's parent (a Row/RowLayout inside the control).
    property Item centreIn: parent

    readonly property rect capInk: inkMetrics.font, inkMetrics.tightBoundingRect("H")
    readonly property real inkCentreY: baselineOffset + capInk.y + capInk.height / 2

    FontMetrics { id: inkMetrics; font: root.font }

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
