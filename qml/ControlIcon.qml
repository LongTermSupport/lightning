import QtQuick

// An Icon whose em-box centre, the glyph's designed centre, sits on centreIn's
// vertical centre; the Text line box is rounded and placed on whole pixels.
Icon {
    id: root

    // The parent, or the parent's parent (a Row/RowLayout inside the control).
    property Item centreIn: parent

    transform: Translate {
        y: {
            if (!root.centreIn || !root.parent)
                return 0
            const top = root.y + (root.parent === root.centreIn ? 0 : root.parent.y)
            const dpr = root.Screen.devicePixelRatio || 1
            return Math.round((root.centreIn.height / 2 - top - root.baselineOffset
                               + root.font.pixelSize / 2) * dpr) / dpr
        }
    }
}
