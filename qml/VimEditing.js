.pragma library

// Key routing for the composers' Vim mode; the keymap itself is VimKeymap (C++).

function shortcutOverride(normal, event) {
    if (!normal)
        return false
    var chord = event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                   | Qt.MetaModifier)
    if (chord === Qt.ControlModifier && event.key === Qt.Key_R)
        return true
    return chord === 0 && event.text.length > 0
}

function enterNormal(vim, editor) {
    var pos = vim.enterNormal(editor.getText(0, editor.length),
                              editor.cursorPosition)
    editor.cursorPosition = Math.min(pos, editor.length)
}

// Null when the key is not the keymap's; otherwise whether it asked to send.
function handleKey(vim, event, editor, rich) {
    var r = vim.handleKey(event.key, event.modifiers, event.text,
                          editor.getText(0, editor.length),
                          editor.cursorPosition)
    if (!r.accepted)
        return null
    event.accepted = true
    if (r.hasEdit) {
        if (r.editEnd > r.editStart)
            editor.remove(r.editStart, r.editEnd)
        if (r.editText.length > 0)
            editor.insert(r.editStart, rich ? vim.richInsertHtml(r.editText)
                                            : r.editText)
    }
    for (var u = 0; u < r.undo; ++u)
        editor.undo()
    for (var d = 0; d < r.redo; ++d)
        editor.redo()
    var pos = r.cursor
    if (r.clampAfter)
        pos = vim.clampNormal(editor.getText(0, editor.length),
                              editor.cursorPosition)
    if (pos >= 0)
        editor.cursorPosition = Math.min(pos, editor.length)
    return { send: r.send }
}

// Shift+K / Shift+J in Normal mode move the message selection; Shift alone,
// with no count or operator pending.
function selectionStepAction(vim, event) {
    if (event.modifiers !== Qt.ShiftModifier || vim.pending.length > 0)
        return ""
    if (event.key === Qt.Key_K)
        return "timeline.olderMessage"
    if (event.key === Qt.Key_J)
        return "timeline.newerMessage"
    return ""
}
