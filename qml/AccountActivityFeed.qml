import QtQuick
import MatrixClient

QtObject {
    id: feed

    property var source: null
    property var userIds: []

    property var activity: null

    function entryFor(uid) {
        const a = feed.activity && uid ? feed.activity[uid] : undefined
        return {
            mentionCount: a && a.mentionCount > 0 ? a.mentionCount : 0,
            hasUnread: !!(a && a.hasUnread === true),
            mentionsLowerBound: !!(a && a.mentionsLowerBound === true),
            liveState: a && a.liveState !== undefined ? a.liveState : 0
        }
    }

    function sameAccountsIgnoringLiveState(a, b) {
        if (!a || !b)
            return a === b
        if (a.length !== b.length)
            return false
        const ignored = { liveState: true, section: true, lastLiveAt: true }
        const byId = {}
        for (let i = 0; i < a.length; ++i)
            byId[a[i].userId || ""] = a[i]
        for (let j = 0; j < b.length; ++j) {
            const y = b[j]
            const x = byId[y.userId || ""]
            if (!x)
                return false
            const keys = {}
            for (const k in x) keys[k] = true
            for (const k in y) keys[k] = true
            for (const k in keys) {
                if (!ignored[k] && x[k] !== y[k])
                    return false
            }
        }
        return true
    }

    function _read(uid) {
        return feed.source && uid ? feed.source.activityFor(uid) : undefined
    }

    function refresh(uid) {
        if (!feed.source || !uid)
            return
        const next = {}
        const current = feed.activity || {}
        for (const k in current)
            next[k] = current[k]
        next[uid] = feed._read(uid)
        feed.activity = next
    }

    function refreshAll() {
        if (!feed.source) {
            if (feed.activity !== null)
                feed.activity = null
            return
        }
        const next = {}
        const ids = feed.userIds || []
        for (let i = 0; i < ids.length; ++i) {
            if (ids[i])
                next[ids[i]] = feed._read(ids[i])
        }
        feed.activity = next
    }

    // Deferred: a change handler on a bound property can run inside another
    // binding's evaluation (the HomePane binding loop, §16), so the write
    // happens on the next turn of the event loop instead. A Timer, not
    // Qt.callLater: that call still runs once the feed's context is gone.
    readonly property Timer _refreshLater: Timer {
        interval: 0
        onTriggered: feed.refreshAll()
    }
    onSourceChanged: feed._refreshLater.restart()
    onUserIdsChanged: feed._refreshLater.restart()
    Component.onCompleted: feed.refreshAll()
    Component.onDestruction: feed._refreshLater.stop()

    readonly property Connections _watch: Connections {
        target: feed.source
        function onSummaryChanged(userId) { feed.refresh(userId) }
        function onLiveStateChanged(userId) { feed.refresh(userId) }
    }
}
