#include "app/SavedMessagesController.h"

#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <utility>

namespace {
const QString kType = QStringLiteral("org.lightning_matrix.saved_messages");
constexpr int kMaxRefs = 100;
constexpr int kMaxBytes = 16 * 1024;
constexpr qint64 kResolveTimeoutMs = 15000;
constexpr int kPrefetchRows = 8;
}

const QString &SavedMessagesController::eventType() { return kType; }

bool SavedMessagesController::supported() const
{ return m_client && m_client->supportsAccountPrefs(); }

SavedMessagesController::SavedMessagesController(QObject *parent) : QObject(parent)
{
    m_clock.start();
    m_resolveTimeout.setSingleShot(true);
    connect(&m_resolveTimeout, &QTimer::timeout, this,
            &SavedMessagesController::expireResolves);
}

qint64 SavedMessagesController::nowMs() const
{ return m_nowForTest >= 0 ? m_nowForTest : m_clock.elapsed(); }

void SavedMessagesController::setNowForTest(qint64 ms)
{
    m_nowForTest = ms;
    expireResolves();
}

void SavedMessagesController::armResolveTimer()
{
    qint64 earliest = -1;
    for (const Ref &ref : std::as_const(m_refs)) {
        if (m_resolving.contains(key(ref.roomId, ref.eventId))
            && (earliest < 0 || ref.deadlineMs < earliest))
            earliest = ref.deadlineMs;
    }
    if (earliest < 0)
        m_resolveTimeout.stop();
    else
        m_resolveTimeout.start(int(qMax<qint64>(0, earliest - nowMs())));
}

void SavedMessagesController::expireResolves()
{
    const qint64 now = nowMs();
    bool expired = false;
    for (Ref &ref : m_refs) {
        const QString id = key(ref.roomId, ref.eventId);
        if (!m_resolving.contains(id) || ref.deadlineMs > now)
            continue;
        m_resolving.remove(id);
        ref.op = 0;
        ref.resolved = true;
        ref.available = false;
        ref.failed = true;
        ref.preview.clear();
        ref.sender.clear();
        ref.kind.clear();
        expired = true;
    }
    if (expired)
        emitChanged();
    pumpResolve();
}

QString SavedMessagesController::key(const QString &roomId, const QString &eventId)
{ return roomId + QChar(0x1f) + eventId; }

bool SavedMessagesController::validRef(const Ref &ref)
{
    return ref.roomId.startsWith(QLatin1Char('!')) && ref.roomId.size() <= 255
        && !MatrixClient::isThreadTimelineId(ref.roomId)
        && !ref.roomId.contains(QChar(0x1f))
        && ref.eventId.startsWith(QLatin1Char('$')) && ref.eventId.size() <= 255
        && !ref.eventId.contains(QChar(0x1f))
        && (ref.rootId.isEmpty()
            || (ref.rootId.startsWith(QLatin1Char('$')) && ref.rootId.size() <= 255
                && !ref.rootId.contains(QChar(0x1f))))
        && ref.savedAtMs > 0;
}

int SavedMessagesController::indexOf(const QString &roomId, const QString &eventId) const
{
    for (int i = 0; i < m_refs.size(); ++i)
        if (m_refs.at(i).roomId == roomId && m_refs.at(i).eventId == eventId)
            return i;
    return -1;
}

void SavedMessagesController::emitChanged()
{
    if (!m_roomFilter.isEmpty()
        && std::none_of(m_refs.cbegin(), m_refs.cend(), [this](const Ref &ref) {
               return ref.roomId == m_roomFilter;
           }))
        m_roomFilter.clear();
    ++m_revision;
    Q_EMIT changed();
}

void SavedMessagesController::reset()
{
    m_refs.clear();
    m_beforeWrite.clear();
    m_desiredBeforeMerge.clear();
    m_removed.clear();
    m_undo = {};
    m_undoValid = false;
    m_loaded = false;
    m_busy = false;
    m_saving = false;
    m_fetchingForWrite = false;
    m_pendingReads = 0;
    m_reloadOwed = false;
    m_error.clear();
    m_resolving.clear();
    m_attempted.clear();
    m_redacted.clear();
    m_resolveTimeout.stop();
    m_query.clear();
    m_roomFilter.clear();
    m_typeFilter.clear();
    m_searchPaused = false;
    m_visibleFirst = 0;
    m_visibleLast = 0;
    emitChanged();
}

void SavedMessagesController::setClient(MatrixClient *client)
{
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_userId.clear();
    reset();
    if (!client)
        return;
    connect(client, &MatrixClient::loggedOut, this, [this] {
        m_userId.clear();
        reset();
    });
    connect(client, &MatrixClient::loginSucceeded, this,
            [this](const QString &userId) {
        m_userId = userId;
        reset();
        refresh();
    });
    connect(client, &MatrixClient::initialSyncDoneChanged, this, [this] {
        if (m_client && m_client->initialSyncDone())
            refresh();
    });
    connect(client, &MatrixClient::accountPrefChanged, this,
            [this](const QString &type) {
        if (type != kType)
            return;
        if (m_busy) m_reloadOwed = true;
        else refresh();
    });
    connect(client, &MatrixClient::accountPrefRead, this,
            [this](const QString &type, bool present, const QVariantMap &content) {
        if (type != kType) return;
        if (m_pendingReads > 0) --m_pendingReads;
        if (m_busy && !m_fetchingForWrite) {
            if (m_pendingReads == 0) {
                m_fetchingForWrite = true;
                ++m_pendingReads;
                m_client->readAccountPref(kType);
            }
            return;
        }
        const QVariantMap current = present ? content : QVariantMap{};
        if (!m_fetchingForWrite) {
            if (!m_busy) receive(current, present);
            return;
        }
        m_fetchingForWrite = false;
        QVector<Ref> remote;
        if (!parseRefs(current, present, remote)) {
            failWrite(tr("Saved messages data is unavailable."));
            return;
        }
        // Apply only THIS operation's change to the fresh server list: what
        // it added (in m_refs, not in m_beforeWrite) and what it removed (the
        // reverse). Re-adding the whole local copy would resurrect an item
        // another device removed after our last read.
        m_desiredBeforeMerge = m_refs;
        QSet<QString> before;
        QSet<QString> desired;
        for (const Ref &ref : std::as_const(m_beforeWrite))
            before.insert(key(ref.roomId, ref.eventId));
        for (const Ref &ref : std::as_const(m_refs))
            desired.insert(key(ref.roomId, ref.eventId));
        QVector<Ref> merged;
        QSet<QString> seen;
        for (const Ref &ref : std::as_const(remote)) {
            const QString id = key(ref.roomId, ref.eventId);
            const bool locallyRemoved = before.contains(id) && !desired.contains(id);
            if (!locallyRemoved && !seen.contains(id)) {
                merged.append(ref);
                seen.insert(id);
            }
        }
        for (const Ref &ref : std::as_const(m_refs)) {
            const QString id = key(ref.roomId, ref.eventId);
            const bool locallyAdded = !before.contains(id);
            if (locallyAdded && !seen.contains(id)) {
                merged.append(ref);
                seen.insert(id);
            }
        }
        if (merged.size() > kMaxRefs) {
            m_beforeWrite = remote;
            failWrite(tr("Saved messages are full. Remove an older item first."));
            return;
        }
        std::sort(merged.begin(), merged.end(), [](const Ref &a, const Ref &b) {
            return a.savedAtMs > b.savedAtMs;
        });
        m_refs = std::move(merged);
        sendWrite(remote);
    });
    connect(client, &MatrixClient::accountPrefReadFailed, this,
            [this](const QString &type, const QString &) {
        if (type != kType)
            return;
        if (m_pendingReads > 0) --m_pendingReads;
        if (m_busy && !m_fetchingForWrite) {
            if (m_pendingReads == 0) {
                m_fetchingForWrite = true;
                ++m_pendingReads;
                m_client->readAccountPref(kType);
            }
            return;
        }
        if (m_fetchingForWrite) {
            m_fetchingForWrite = false;
            failWrite(tr("Saved messages could not be updated. Retry."));
            return;
        }
        if (m_busy) return;
        m_error = tr("Saved messages could not be loaded. Retry.");
        emitChanged();
    });
    connect(client, &MatrixClient::accountPrefWritten, this,
            [this](const QString &type, bool ok, const QString &category) {
        if (type != kType || !m_busy)
            return;
        if (!ok && category == QLatin1String("stale_absence") && !m_staleRetried) {
            m_staleRetried = true;
            m_refs = m_desiredBeforeMerge;
            m_fetchingForWrite = true;
            ++m_pendingReads;
            m_client->readAccountPref(kType);
            return;
        }
        m_busy = false;
        if (!ok) {
            m_refs = std::move(m_beforeWrite);
            m_undoValid = false;
            m_error = tr("Saved messages could not be updated. Retry.");
            Q_EMIT actionFailed(m_error);
        } else {
            m_error.clear();
            if (m_undoValid) {
                m_removed.append(qMakePair(key(m_undo.roomId, m_undo.eventId), m_undo.savedAtMs));
                while (m_removed.size() > kMaxRefs)
                    m_removed.removeFirst();
            } else if (!m_saving) {
                m_removed.removeAll(qMakePair(key(m_undo.roomId, m_undo.eventId), m_undo.savedAtMs));
            }
            if (m_saving)
                Q_EMIT saveSucceeded();
        }
        m_saving = false;
        m_beforeWrite.clear();
        emitChanged();
        if (ok || m_reloadOwed) {
            m_reloadOwed = false;
            refresh();
        }
    });
    connect(client, &MatrixClient::eventSourceReceived, this,
            [this](const QString &roomId, const QString &eventId, bool ok,
                   const QString &json, const QVariantMap &meta) {
        receiveSource(roomId, eventId, ok, json, meta);
    });
    connect(client, &MatrixClient::eventEdited, this,
            &SavedMessagesController::invalidatePreview);
    connect(client, &MatrixClient::eventChangedAt, this,
            [this](const QString &timelineId, int, const TimelineEvent &event) {
        if (!event.edited)
            return;
        const int i = indexOf(MatrixClient::threadTimelineRoomId(timelineId),
                              event.eventId);
        if (i < 0 || m_refs.at(i).timelineBody == event.body)
            return;
        m_refs[i].timelineBody = event.body;
        invalidatePreview(m_refs.at(i).roomId, event.eventId);
    });
    connect(client, &MatrixClient::eventRedacted, this,
            [this](const QString &roomId, const QString &eventId) {
        const int i = indexOf(roomId, eventId);
        if (i < 0) return;
        Ref &ref = m_refs[i];
        ref.preview.clear();
        ref.sender.clear();
        ref.available = false;
        ref.resolved = true;
        const QString id = key(roomId, eventId);
        m_resolving.remove(id);
        ref.op = 0;
        ref.failed = false;
        m_attempted.insert(id);
        m_redacted.insert(id);
        emitChanged();
        pumpResolve();
    });
    if (client->isLoggedIn() && !client->currentUserId().isEmpty()) {
        m_userId = client->currentUserId();
        refresh();
    }
}

void SavedMessagesController::refresh()
{
    if (m_busy) { m_reloadOwed = true; return; }
    if (!m_client || !m_client->supportsAccountPrefs()
        || m_userId.isEmpty() || m_client->currentUserId() != m_userId)
        return;
    ++m_pendingReads;
    m_client->readAccountPref(kType);
    emitChanged();
}

void SavedMessagesController::receive(const QVariantMap &content, bool present)
{
    if (!m_client || m_userId.isEmpty()
        || m_client->currentUserId() != m_userId)
        return;
    QVector<Ref> next;
    if (!parseRefs(content, present, next)) {
        m_loaded = false;
        m_error = tr("Saved messages data is unavailable.");
        emitChanged();
        return;
    }
    m_refs = std::move(next);
    m_loaded = true;
    m_error.clear();
    for (const Ref &ref : std::as_const(m_refs)) {
        if (m_removed.contains(qMakePair(key(ref.roomId, ref.eventId), ref.savedAtMs))) {
            m_error = tr("Another device restored a removed saved message. Review the list and remove it again if needed.");
            break;
        }
    }
    m_resolving.clear();
    m_attempted.clear();
    for (Ref &ref : m_refs) {
        if (m_redacted.contains(key(ref.roomId, ref.eventId))) {
            ref.resolved = true;
            ref.available = false;
            m_attempted.insert(key(ref.roomId, ref.eventId));
        }
    }
    m_resolveTimeout.stop();
    emitChanged();
}

bool SavedMessagesController::parseRefs(const QVariantMap &content, bool present,
                                        QVector<Ref> &next) const
{
    const QVariantList raw = content.value(QStringLiteral("entries")).toList();
    if (!content.isEmpty()
        && (content.size() != 2
            || !content.value(QStringLiteral("entries")).canConvert<QVariantList>()
            || content.value(QStringLiteral("version")).toInt() != 1
            || QJsonDocument(QJsonObject::fromVariantMap(content)).toJson(QJsonDocument::Compact).size() > kMaxBytes
            || raw.size() > kMaxRefs)) {
        return false;
    }
    QSet<QString> seen;
    for (const QVariant &v : raw) {
        const QVariantMap item = v.toMap();
        if (item.size() != 4) {
            return false;
        }
        Ref ref;
        ref.roomId = item.value(QStringLiteral("room_id")).toString();
        ref.eventId = item.value(QStringLiteral("event_id")).toString();
        ref.rootId = item.value(QStringLiteral("thread_root_id")).toString();
        ref.savedAtMs = item.value(QStringLiteral("saved_at_ms")).toLongLong();
        if (!validRef(ref)) {
            return false;
        }
        const QString id = key(ref.roomId, ref.eventId);
        if (!seen.contains(id)) {
            next.append(ref);
            seen.insert(id);
        }
    }
    std::sort(next.begin(), next.end(), [](const Ref &a, const Ref &b) {
        return a.savedAtMs > b.savedAtMs;
    });
    return true;
}

bool SavedMessagesController::isSaved(const QString &roomId, const QString &eventId) const
{ return indexOf(roomId, eventId) >= 0; }

void SavedMessagesController::write()
{
    if (!m_client || m_userId.isEmpty()
        || m_client->currentUserId() != m_userId)
        return;
    m_busy = true;
    m_staleRetried = false;
    m_fetchingForWrite = m_pendingReads == 0;
    m_error.clear();
    emitChanged();
    if (m_fetchingForWrite) {
        ++m_pendingReads;
        m_client->readAccountPref(kType);
    }
}

void SavedMessagesController::failWrite(const QString &message)
{
    m_busy = false;
    m_saving = false;
    m_fetchingForWrite = false;
    m_refs = std::move(m_beforeWrite);
    m_beforeWrite.clear();
    m_undoValid = false;
    m_error = message;
    Q_EMIT actionFailed(m_error);
    emitChanged();
}

QVariantMap SavedMessagesController::referenceContent() const
{
    QVariantList entries;
    for (const Ref &ref : std::as_const(m_refs))
        entries.append(QVariantMap{
            { QStringLiteral("room_id"), ref.roomId },
            { QStringLiteral("event_id"), ref.eventId },
            { QStringLiteral("thread_root_id"), ref.rootId },
            { QStringLiteral("saved_at_ms"), ref.savedAtMs },
        });
    return {
        { QStringLiteral("version"), 1 },
        { QStringLiteral("entries"), entries },
    };
}

QString SavedMessagesController::capacityNotice() const
{
    if (!m_loaded)
        return {};
    const int slots = kMaxRefs - count();
    const auto bytes = kMaxBytes - QJsonDocument(QJsonObject::fromVariantMap(referenceContent()))
                                       .toJson(QJsonDocument::Compact).size();
    if (slots > 10 && bytes > kMaxBytes / 10)
        return {};
    return tr("Saved message capacity remaining: %n slot(s), %1 bytes.", "", slots)
        .arg(qMax<qsizetype>(0, bytes));
}

void SavedMessagesController::sendWrite(const QVector<Ref> &remote)
{
    const QVariantMap content = referenceContent();
    if (QJsonDocument(QJsonObject::fromVariantMap(content))
            .toJson(QJsonDocument::Compact).size() > kMaxBytes) {
        m_beforeWrite = remote;
        failWrite(tr("Saved messages are full. Remove an older item first."));
        return;
    }
    m_client->writeAccountPref(kType, content);
}

void SavedMessagesController::save(const QString &roomId,
                                   const QString &eventId,
                                   const QString &threadRootId)
{
    if (!m_loaded || m_busy || isSaved(roomId, eventId)) return;
    Ref ref{roomId, eventId, threadRootId,
            QDateTime::currentMSecsSinceEpoch()};
    if (!validRef(ref)) return;
    if (m_refs.size() >= kMaxRefs) {
        m_error = tr("Saved messages are full. Remove an older item first.");
        Q_EMIT actionFailed(m_error);
        emitChanged();
        return;
    }
    m_beforeWrite = m_refs;
    m_refs.prepend(ref);
    m_saving = true;
    m_undoValid = false;
    write();
}

void SavedMessagesController::remove(const QString &roomId,
                                     const QString &eventId)
{
    if (!m_loaded || m_busy) return;
    const int i = indexOf(roomId, eventId);
    if (i < 0) return;
    m_beforeWrite = m_refs;
    m_undo = m_refs.takeAt(i);
    m_saving = false;
    m_undoValid = true;
    write();
}

void SavedMessagesController::undoRemove()
{
    if (!m_loaded || m_busy || !m_undoValid
        || isSaved(m_undo.roomId, m_undo.eventId)) return;
    m_beforeWrite = m_refs;
    m_refs.append(m_undo);
    m_saving = false;
    std::sort(m_refs.begin(), m_refs.end(), [](const Ref &a, const Ref &b) {
        return a.savedAtMs > b.savedAtMs;
    });
    m_undoValid = false;
    write();
}

void SavedMessagesController::clearFilters()
{
    if (m_query.isEmpty() && m_roomFilter.isEmpty() && m_typeFilter.isEmpty())
        return;
    m_query.clear();
    m_roomFilter.clear();
    m_typeFilter.clear();
    m_searchPaused = false;
    emitChanged();
    pumpResolve();
}

void SavedMessagesController::setQuery(const QString &value)
{
    if (value == m_query) return;
    m_query = value;
    m_searchPaused = false;
    emitChanged();
    pumpResolve();
}
void SavedMessagesController::setRoomFilter(const QString &value)
{ if (value != m_roomFilter) { m_roomFilter = value; emitChanged(); pumpResolve(); } }
void SavedMessagesController::setTypeFilter(const QString &value)
{
    if (value == m_typeFilter) return;
    m_typeFilter = value;
    m_searchPaused = false;
    emitChanged();
    pumpResolve();
}

void SavedMessagesController::setSearchPaused(bool paused)
{
    if (paused == m_searchPaused) return;
    m_searchPaused = paused;
    emitChanged();
    pumpResolve();
}

bool SavedMessagesController::searching() const
{ return !m_query.isEmpty() || !m_typeFilter.isEmpty(); }

QString SavedMessagesController::roomNameOf(const Ref &ref) const
{
    const RoomInfo room = m_client ? m_client->roomInfo(ref.roomId) : RoomInfo{};
    return room.name.isEmpty() ? ref.roomId : room.name;
}

bool SavedMessagesController::shown(const Ref &ref, const QString &roomName) const
{
    if (!m_roomFilter.isEmpty() && ref.roomId != m_roomFilter) return false;
    if (m_typeFilter == QLatin1String("text")
        && ref.kind != QLatin1String("m.text")
        && ref.kind != QLatin1String("m.notice")
        && ref.kind != QLatin1String("m.emote")) return false;
    if (m_typeFilter == QLatin1String("media")
        && (ref.kind.isEmpty() || ref.kind == QLatin1String("m.text")
            || ref.kind == QLatin1String("m.notice")
            || ref.kind == QLatin1String("m.emote"))) return false;
    return m_query.isEmpty()
        || roomName.contains(m_query, Qt::CaseInsensitive)
        || ref.preview.contains(m_query, Qt::CaseInsensitive)
        || ref.sender.contains(m_query, Qt::CaseInsensitive);
}

int SavedMessagesController::unsearchedCount() const
{
    return int(std::count_if(m_refs.cbegin(), m_refs.cend(), [this](const Ref &ref) {
        return (m_roomFilter.isEmpty() || ref.roomId == m_roomFilter)
            && (!ref.resolved || ref.failed);
    }));
}

bool SavedMessagesController::searchLoading() const
{
    if (!m_active || m_searchPaused || !searching()) return false;
    return std::any_of(m_refs.cbegin(), m_refs.cend(), [this](const Ref &ref) {
        return (m_roomFilter.isEmpty() || ref.roomId == m_roomFilter) && !ref.resolved;
    });
}

QVariantList SavedMessagesController::entries() const
{
    QVariantList out;
    for (const Ref &ref : m_refs) {
        const QString roomName = roomNameOf(ref);
        if (!shown(ref, roomName)) continue;
        out.append(QVariantMap{
            { QStringLiteral("roomId"), ref.roomId },
            { QStringLiteral("roomName"), roomName },
            { QStringLiteral("eventId"), ref.eventId },
            { QStringLiteral("threadRootId"), ref.rootId },
            { QStringLiteral("savedAtMs"), ref.savedAtMs },
            { QStringLiteral("sender"), ref.sender },
            { QStringLiteral("senderName"),
              m_client && !ref.sender.isEmpty()
                  ? m_client->displayNameFor(ref.roomId, ref.sender) : QString() },
            { QStringLiteral("preview"), ref.preview },
            { QStringLiteral("kind"), ref.kind },
            { QStringLiteral("resolved"), ref.resolved },
            { QStringLiteral("available"), ref.available },
            { QStringLiteral("failed"), ref.failed },
        });
    }
    return out;
}

QVariantList SavedMessagesController::rooms() const
{
    QVariantList out;
    QSet<QString> seen;
    for (const Ref &ref : m_refs) {
        if (seen.contains(ref.roomId)) continue;
        seen.insert(ref.roomId);
        const RoomInfo room = m_client ? m_client->roomInfo(ref.roomId) : RoomInfo{};
        out.append(QVariantMap{
            { QStringLiteral("roomId"), ref.roomId },
            { QStringLiteral("name"), room.name.isEmpty() ? ref.roomId : room.name },
        });
    }
    return out;
}

void SavedMessagesController::resolveVisible(int first, int last)
{
    m_active = true;
    m_visibleFirst = qMax(0, first);
    m_visibleLast = qMax(m_visibleFirst, last);
    pumpResolve();
}

void SavedMessagesController::pauseResolving()
{
    // Requests already sent retire through their answer or their deadline.
    m_active = false;
}

void SavedMessagesController::refreshPreviews()
{
    const QVector<Ref> refs = m_refs;
    for (const Ref &ref : refs)
        if (ref.resolved)
            invalidatePreview(ref.roomId, ref.eventId);
    pumpResolve();
}

void SavedMessagesController::retryPreview(const QString &roomId,
                                           const QString &eventId)
{
    const int i = indexOf(roomId, eventId);
    if (i >= 0 && m_refs.at(i).failed)
        invalidatePreview(roomId, eventId);
}

void SavedMessagesController::invalidatePreview(const QString &roomId,
                                                const QString &eventId)
{
    const int i = indexOf(roomId, eventId);
    if (i < 0)
        return;
    const QString id = key(roomId, eventId);
    Ref &ref = m_refs[i];
    if (m_redacted.contains(id) || (!ref.resolved && !m_resolving.contains(id)))
        return;
    // Dropping the in-flight op makes its late answer stale; the text already
    // shown stays until the new answer replaces it.
    m_resolving.remove(id);
    m_attempted.remove(id);
    ref.op = 0;
    ref.resolved = false;
    if (ref.failed) {
        ref.failed = false;
        emitChanged();
    }
    pumpResolve();
}

void SavedMessagesController::pumpResolve()
{
    if (!m_client || !m_loaded || m_userId != m_client->currentUserId()) return;
    QSet<QString> present;
    for (const Ref &ref : std::as_const(m_refs))
        present.insert(key(ref.roomId, ref.eventId));
    m_resolving.intersect(present);
    const bool everything = searching() && !m_searchPaused;
    const int windowEnd = m_visibleLast + kPrefetchRows;
    while (m_active && m_resolving.size() < 4) {
        int next = -1;
        int row = 0;
        for (int i = 0; i < m_refs.size() && (everything || row <= windowEnd); ++i) {
            const Ref &ref = m_refs.at(i);
            bool wanted = everything
                && (m_roomFilter.isEmpty() || ref.roomId == m_roomFilter);
            if (shown(ref, roomNameOf(ref)))
                wanted = (row++ >= m_visibleFirst) || wanted;
            const QString id = key(ref.roomId, ref.eventId);
            if (wanted && !ref.resolved && !m_attempted.contains(id)
                && !m_redacted.contains(id)) {
                next = i;
                break;
            }
        }
        if (next < 0) break;
        const QString roomId = m_refs.at(next).roomId;
        const QString eventId = m_refs.at(next).eventId;
        const QString id = key(roomId, eventId);
        m_attempted.insert(id);
        m_resolving.insert(id);
        m_refs[next].deadlineMs = nowMs() + kResolveTimeoutMs;
        const quint64 op = m_client->requestEventSource(roomId, eventId);
        const int i = indexOf(roomId, eventId);
        if (i >= 0 && m_resolving.contains(id))
            m_refs[i].op = op;
    }
    armResolveTimer();
}

void SavedMessagesController::receiveSource(const QString &roomId,
                                            const QString &eventId, bool ok,
                                            const QString &json,
                                            const QVariantMap &meta)
{
    const QString id = key(roomId, eventId);
    if (m_redacted.contains(id) || !m_resolving.contains(id)) return;
    const int i = indexOf(roomId, eventId);
    if (i >= 0 && meta.value(QStringLiteral("opId")).toULongLong() != m_refs.at(i).op)
        return;
    m_resolving.remove(id);
    if (i >= 0) {
        Ref &ref = m_refs[i];
        ref.op = 0;
        ref.resolved = true;
        ref.failed = false;
        ref.available = false;
        ref.preview.clear();
        ref.sender.clear();
        ref.kind.clear();
        if (ok) {
            const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
            QJsonObject content = object.value(QStringLiteral("content")).toObject();
            const bool redacted = object.value(QStringLiteral("unsigned"))
                .toObject().value(QStringLiteral("redacted_because")).isObject();
            if (!redacted && object.value(QStringLiteral("type")).toString()
                    == QLatin1String("m.sticker") && !content.isEmpty()) {
                ref.sender = object.value(QStringLiteral("sender")).toString();
                ref.kind = QStringLiteral("m.sticker");
                ref.preview = tr("Sticker");
                ref.available = true;
            }
            if (!redacted && object.value(QStringLiteral("type")).toString()
                    == QLatin1String("m.room.message") && !content.isEmpty()) {
                // The backend already applied the replacement validity rules.
                const QVariantMap edited =
                    meta.value(QStringLiteral("effectiveContent")).toMap();
                if (!edited.isEmpty())
                    content = QJsonObject::fromVariantMap(edited);
                const QString msgtype = content.value(QStringLiteral("msgtype")).toString();
                ref.sender = object.value(QStringLiteral("sender")).toString();
                ref.kind = msgtype;
                ref.available = true;
                if (meta.value(QStringLiteral("editUnreadable")).toBool())
                    ref.preview = tr("Edited message. The latest version cannot be decrypted.");
                else if (msgtype == QLatin1String("m.image")) ref.preview = tr("Image");
                else if (msgtype == QLatin1String("m.video")) ref.preview = tr("Video");
                else if (msgtype == QLatin1String("m.audio")) ref.preview = tr("Audio");
                else if (msgtype == QLatin1String("m.file")) ref.preview = tr("File");
                else ref.preview = content.value(QStringLiteral("body"))
                    .toString().simplified().left(140);
            }
        }
        emitChanged();
    }
    pumpResolve();
}

void SavedMessagesController::open(const QString &roomId,
                                   const QString &eventId)
{
    const int i = indexOf(roomId, eventId);
    if (i < 0) return;
    const Ref &ref = m_refs.at(i);
    Q_EMIT openRequested(ref.roomId, ref.eventId, ref.rootId);
}

void SavedMessagesController::openRoot(const QString &roomId,
                                       const QString &eventId)
{
    const int i = indexOf(roomId, eventId);
    if (i < 0 || m_refs.at(i).rootId.isEmpty()) return;
    Q_EMIT openRequested(roomId, m_refs.at(i).rootId, QString());
}

void SavedMessagesController::openRoom(const QString &roomId)
{
    for (const Ref &ref : std::as_const(m_refs)) {
        if (ref.roomId == roomId) {
            Q_EMIT openRequested(roomId, QString(), QString());
            return;
        }
    }
}
