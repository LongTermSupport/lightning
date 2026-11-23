import QtQuick
import MatrixClient
import "IconGlyphs.js" as IconGlyphs

Text {
    textFormat: Text.PlainText
    id: root

    property string name: ""
    property int size: 18

    text: IconGlyphs.codepoints[name] || ""
    font.family: AppTheme.iconFont
    font.pixelSize: size
    color: AppTheme.textSecondary
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignHCenter
    renderType: Text.NativeRendering

    // Set to put the glyph's em-box centre (the designed centre) on this item's
    // vertical centre; this Text's line box is rounded and placed on whole pixels.
    property Item centreIn: null
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
