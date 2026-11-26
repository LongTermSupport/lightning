#include "app/BulkRedactionController.h"

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QDateTime>
#include <algorithm>
#include <QTimer>
#include <QGuiApplication>

namespace {
QString requestKey(const QString &roomId, const QString &eventId)
{
    return roomId + QChar(0x1e) + eventId;
}

bool isStateLike(const TimelineEvent &event)
{
    return event.type == TimelineEvent::StateChange
        || event.type == TimelineEvent::CallEvent;
}

constexpr int kMaxRateLimitRetries = 3;
}

BulkRedactionController::BulkRedactionController(QObject *parent)
    : QObject(parent)
{
    m_requestTimer.setSingleShot(true);
    connect(&m_requestTimer, &QTimer::timeout, this,
            &BulkRedactionController::onRequestTimeout);
}

void BulkRedactionController::abandon(const QString &key, bool alreadyCounted)
{
    m_abandonedRequests.insert(
        key, Abandoned{QDateTime::currentMSecsSinceEpoch(), alreadyCounted});
}

void BulkRedactionController::pruneAbandoned()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_abandonedRequests.begin();
         it != m_abandonedRequests.end();) {
        if (now - it.value().atMs >= m_abandonedExpiryMs)
            it = m_abandonedRequests.erase(it);
        else
            ++it;
    }
}

bool BulkRedactionController::isAbandoned(const QString &key)
{
    pruneAbandoned();
    return m_abandonedRequests.contains(key);
}

void BulkRedactionController::setClient(MatrixClient *client)
{
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    ++m_generation;
    m_client = client;
    m_model = nullptr;
    m_ids.clear();
    m_anchor.clear();
    m_queue.clear();
    m_inFlight.clear();
    m_reason.clear();
    m_selecting = false;
    m_running = false;
    m_cancelled = false;
    m_completed = m_total = m_failed = 0;
    m_error.clear();
    m_abandonedRequests.clear();
    m_requestTimer.stop();
    resetPurge();
    if (client) {
        connect(client, &MatrixClient::redactionFinished, this,
                &BulkRedactionController::onResult);
        connect(client, &MatrixClient::redactionRateLimited, this,
                &BulkRedactionController::onRateLimited);
        connect(client, &MatrixClient::userRecentEventsReceived, this,
                &BulkRedactionController::onUserRecentEvents);
    }
    Q_EMIT changed();
}

void BulkRedactionController::begin(TimelineModel *model)
{
    if (m_running || !model || !m_client || model->roomId().isEmpty())
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    ++m_generation;
    m_model = model;
    connect(model, &TimelineModel::roomIdChanged, this,
            &BulkRedactionController::cancel);
    m_roomId.clear();
    m_timelineId = model->roomId();
    m_ids.clear();
    m_anchor.clear();
    m_queue.clear();
    m_inFlight.clear();
    m_selecting = true;
    m_cancelled = false;
    m_completed = m_total = m_failed = 0;
    m_error.clear();
    resetPurge();
    Q_EMIT changed();
}

void BulkRedactionController::resetPurge()
{
    m_purge = false;
    m_purgeCapped = false;
    m_purgeWalkable = false;
    m_purgeOp = 0;
    m_purgeUserId.clear();
    m_purgeName.clear();
    m_purgeFrom.clear();
    m_purgeNextFrom.clear();
    m_purgeMessages.clear();
    m_purgeState.clear();
    m_retryAfterMs.clear();
    m_rateLimitRetries.clear();
}

bool BulkRedactionController::preparePurge(TimelineModel *model,
                                           const QString &userId,
                                           const QString &displayName,
                                           bool canRedactOthers)
{
    if (m_running || !model || !m_client || !canRedactOthers
        || userId.isEmpty() || userId == model->ownUserId()
        || model->roomId().isEmpty()
        || MatrixClient::isThreadTimelineId(model->roomId())
        || MatrixClient::isContextTimelineId(model->roomId()))
        return false;
    begin(model);
    if (!m_selecting)
        return false;
    m_selecting = false;
    m_purge = true;
    m_purgeUserId = userId;
    m_purgeName = displayName.isEmpty() ? userId : displayName;
    m_roomId = model->roomId();
    for (const TimelineEvent &event : model->events()) {
        if (event.sender != userId || event.redacted || event.isVirtual()
            || event.status != TimelineEvent::Sent
            || !event.eventId.startsWith(QLatin1Char('$')))
            continue;
        (isStateLike(event) ? m_purgeState : m_purgeMessages)
            .append(event.eventId);
    }
    requestWalk(QString());
    Q_EMIT changed();
    return true;
}

void BulkRedactionController::requestWalk(const QString &from)
{
    m_purgeFrom = from;
    m_purgeNextFrom.clear();
    m_purgeOp = m_client->requestUserRecentEvents(m_roomId, m_purgeUserId,
                                                  from, kPurgeWalkRequests);
    m_purgeWalkable = m_purgeOp != 0;
    m_purgeCapped = !m_purgeWalkable && m_model && m_model->canPaginate();
}

bool BulkRedactionController::continuePurge(bool canRedactOthers)
{
    if (!purgeCanLookFurther() || !canRedactOthers || !m_model || !m_client
        || m_model->roomId() != m_timelineId)
        return false;
    ++m_generation;
    m_queue.clear();
    m_inFlight.clear();
    m_purgeMessages.clear();
    m_purgeState.clear();
    m_retryAfterMs.clear();
    m_rateLimitRetries.clear();
    m_cancelled = false;
    m_completed = m_total = m_failed = 0;
    m_error.clear();
    requestWalk(m_purgeNextFrom);
    Q_EMIT changed();
    return true;
}

void BulkRedactionController::onUserRecentEvents(
    quint64 opId, const QString &roomId, const QString &userId, bool ok,
    const QStringList &messageIds, const QStringList &stateIds,
    const QString &nextFrom)
{
    if (opId == 0 || opId != m_purgeOp || roomId != m_roomId
        || userId != m_purgeUserId)
        return;
    m_purgeOp = 0;
    if (ok) {
        for (const QString &id : messageIds) {
            if (!m_purgeMessages.contains(id) && !m_purgeState.contains(id))
                m_purgeMessages.append(id);
        }
        for (const QString &id : stateIds) {
            if (!m_purgeMessages.contains(id) && !m_purgeState.contains(id))
                m_purgeState.append(id);
        }
        m_purgeNextFrom = nextFrom;
        m_purgeCapped = !nextFrom.isEmpty();
    } else {
        // Nothing walked: a further look starts from the same place.
        m_purgeNextFrom = m_purgeFrom;
        m_purgeCapped = true;
    }
    Q_EMIT changed();
}

void BulkRedactionController::startPurge(const QString &reason,
                                         bool keepStateEvents)
{
    if (!m_purge || m_running || m_purgeOp != 0 || !m_model || !m_client)
        return;
    QStringList queue = m_purgeMessages;
    if (!keepStateEvents)
        queue += m_purgeState;
    if (queue.isEmpty())
        return;
    m_queue = queue;
    m_total = m_queue.size();
    m_completed = m_failed = 0;
    m_reason = reason;
    m_error.clear();
    m_retryAfterMs.clear();
    m_rateLimitRetries.clear();
    m_running = true;
    m_cancelled = false;
    Q_EMIT changed();
    sendNext();
}

void BulkRedactionController::onRateLimited(const QString &roomId,
                                            const QString &eventId,
                                            qint64 retryAfterMs)
{
    if (m_running && roomId == m_roomId && eventId == m_inFlight)
        m_retryAfterMs.insert(eventId, retryAfterMs);
}

void BulkRedactionController::scheduleNext(int delayMs)
{
    const quint64 generation = m_generation;
    QTimer::singleShot(delayMs, this, [this, generation] {
        if (generation == m_generation)
            sendNext();
    });
}

void BulkRedactionController::add(const QString &eventId,
                                   bool canRedactOthers)
{
    if (!m_model || eventId.isEmpty())
        return;
    const QString roomId = m_model->realRoomIdForEvent(eventId);
    if (roomId.isEmpty() || (!m_roomId.isEmpty() && roomId != m_roomId))
        return;
    if (isAbandoned(requestKey(roomId, eventId))) {
        m_error = tr("A previous deletion of this message is still in progress.");
        return;
    }
    if (!m_model->canRedactEvent(eventId, canRedactOthers)) {
        m_error = tr("That message cannot be deleted with your current permissions.");
        return;
    }
    if (m_ids.size() >= 50) {
        m_error = tr("Select up to 50 messages at once.");
        return;
    }
    m_roomId = roomId;
    m_ids.append(eventId);
    m_error.clear();
}

bool BulkRedactionController::adopt(TimelineModel *model,
                                    const QStringList &ids,
                                    bool canRedactOthers)
{
    begin(model);
    if (!m_selecting)
        return false;
    QString refusal;
    for (const QString &id : ids) {
        add(id, canRedactOthers);
        if (refusal.isEmpty())
            refusal = m_error;
    }
    if (!ids.isEmpty())
        m_anchor = ids.last();
    const bool complete = m_ids.size() == ids.size();
    if (!complete)
        m_error = refusal.isEmpty()
            ? tr("That message cannot be deleted with your current permissions.")
            : refusal;
    if (m_ids.isEmpty()) {
        const QString error = m_error;
        cancel();
        m_error = error;
    }
    Q_EMIT changed();
    return complete;
}

void BulkRedactionController::toggle(const QString &eventId,
                                      bool canRedactOthers)
{
    if (!m_selecting || m_running)
        return;
    if (m_ids.removeOne(eventId)) {
        m_error.clear();
    } else {
        add(eventId, canRedactOthers);
    }
    m_anchor = eventId;
    Q_EMIT changed();
}

void BulkRedactionController::toggleFromPointer(const QString &eventId,
                                                bool canRedactOthers)
{
    if ((QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)
        && !m_anchor.isEmpty()) {
        selectRange(m_anchor, eventId, canRedactOthers);
        return;
    }
    toggle(eventId, canRedactOthers);
}

void BulkRedactionController::selectRange(const QString &first,
                                           const QString &last,
                                           bool canRedactOthers)
{
    if (!m_selecting || m_running || !m_model)
        return;
    int from = -1, to = -1;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QString id = m_model->eventIdAt(row);
        if (id == first) from = row;
        if (id == last) to = row;
    }
    if (from < 0 || to < 0)
        return;
    int skipped = 0;
    for (int row = qMin(from, to); row <= qMax(from, to); ++row) {
        const QString id = m_model->eventIdAt(row);
        if (m_ids.contains(id))
            continue;
        const QModelIndex index = m_model->index(row, 0);
        if (id.isEmpty()
            || m_model->data(index, TimelineModel::IsVirtualRole).toBool()
            || m_model->data(index, TimelineModel::IsStateActivityRole).toBool()
            || m_model->data(index, TimelineModel::IsCallEventRole).toBool())
            continue;
        if (!m_model->canRedactEvent(id, canRedactOthers)) {
            ++skipped;
            continue;
        }
        add(id, canRedactOthers);
        if (m_ids.size() == 50)
            break;
    }
    if (skipped)
        m_error = tr("%n message(s) in the range cannot be deleted.", nullptr,
                     skipped);
    Q_EMIT changed();
}

bool BulkRedactionController::isSelected(const QString &eventId) const
{
    return m_ids.contains(eventId);
}

void BulkRedactionController::start(const QString &reason)
{
    if (!m_selecting || m_running || m_ids.isEmpty() || !m_model || !m_client)
        return;
    m_queue = m_ids;
    m_total = m_queue.size();
    m_completed = m_failed = 0;
    m_reason = reason;
    m_error.clear();
    m_running = true;
    m_selecting = false;
    m_cancelled = false;
    Q_EMIT changed();
    sendNext();
}

void BulkRedactionController::sendNext()
{
    if (!m_running || !m_client)
        return;
    if (m_purge && m_model) {
        // Already redacted (by someone else, or before this run): skipped,
        // and not counted either way.
        const auto redacted = [this](const QString &id) {
            for (const TimelineEvent &event : m_model->events()) {
                if (event.eventId == id)
                    return event.redacted;
            }
            return false;
        };
        while (!m_queue.isEmpty() && redacted(m_queue.first())) {
            m_queue.removeFirst();
            --m_total;
        }
    }
    if (m_cancelled || m_queue.isEmpty()) {
        finish();
        return;
    }
    m_inFlight = m_queue.takeFirst();
    // Bounded: a result the client never delivers (dropped by a reset, lost
    // to a refusal) must not hold the run and its "running" state for ever.
    // Armed before the call, which may answer synchronously and stop it.
    m_requestTimer.start(m_requestTimeoutMs);
    if (m_purge)
        m_client->redactOthersEvent(m_timelineId, m_inFlight, m_reason);
    else
        m_client->redactEvent(m_timelineId, m_inFlight, m_reason);
}

void BulkRedactionController::onRequestTimeout()
{
    if (!m_running || m_inFlight.isEmpty())
        return;
    abandon(requestKey(m_roomId, m_inFlight),true);
    m_inFlight.clear();
    ++m_completed;
    ++m_failed;
    m_error = tr("A deletion got no answer from the server; its result is unknown.");
    Q_EMIT changed();
    scheduleNext(300);
}

void BulkRedactionController::onResult(const QString &roomId,
                                       const QString &eventId, bool ok,
                                       const QString &category)
{
    pruneAbandoned();
    const auto abandoned = m_abandonedRequests.constFind(requestKey(roomId, eventId));
    if (abandoned != m_abandonedRequests.cend()) {
        const bool counted = abandoned->counted;
        m_abandonedRequests.erase(abandoned);
        if (!counted && m_cancelled && !m_running && roomId == m_roomId) {
            ++m_completed;
            if (!ok) {
                ++m_failed;
                m_error = MatrixClient::redactionFailureText(category);
            } else {
                m_error.clear();
            }
            Q_EMIT changed();
        }
        return;
    }
    if (!m_running || roomId != m_roomId || eventId != m_inFlight)
        return;
    m_requestTimer.stop();
    m_inFlight.clear();
    const bool rateLimited = category == QLatin1String("rate_limited");
    if (!ok && rateLimited && m_purge && !m_cancelled
        && ++m_rateLimitRetries[eventId] <= kMaxRateLimitRetries) {
        const qint64 asked = m_retryAfterMs.take(eventId);
        const int wait = int(std::clamp<qint64>(asked > 0 ? asked : 5000,
                                                1000, 120000));
        m_queue.prepend(eventId);
        m_error = tr("The server asked to slow down; continuing shortly.");
        Q_EMIT changed();
        scheduleNext(wait);
        return;
    }
    ++m_completed;
    if (!ok) {
        ++m_failed;
        m_error = MatrixClient::redactionFailureText(category);
        if (rateLimited) {
            m_cancelled = true;
            m_queue.clear();
        }
    } else if (m_purge) {
        m_error.clear();
    }
    Q_EMIT changed();
    scheduleNext(300);
}

void BulkRedactionController::finish()
{
    m_requestTimer.stop();
    m_running = false;
    m_queue.clear();
    m_inFlight.clear();
    m_ids.clear();
    m_reason.clear();
    ++m_generation;
    Q_EMIT changed();
}

void BulkRedactionController::cancel()
{
    if (m_running && m_purge && !m_inFlight.isEmpty()) {
        // Stops after the request already on the wire, whose result counts.
        m_cancelled = true;
        m_queue.clear();
        Q_EMIT changed();
        return;
    }
    if (m_running) {
        m_cancelled = true;
        m_queue.clear();
        if (!m_inFlight.isEmpty()) {
            abandon(requestKey(m_roomId, m_inFlight),false);
            m_error = tr("One redaction was already in progress; its result is unknown.");
        }
        finish();
        return;
    }
    ++m_generation;
    m_selecting = false;
    m_ids.clear();
    m_error.clear();
    m_reason.clear();
    m_completed = m_total = m_failed = 0;
    m_cancelled = false;
    resetPurge();
    Q_EMIT changed();
}
