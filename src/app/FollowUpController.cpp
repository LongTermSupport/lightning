#include "app/FollowUpController.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVariantMap>

#include <algorithm>

namespace {
QString key(const QString &roomId, const QString &eventId)
{
    return roomId + QChar(0x1f) + eventId;
}

bool validState(const QString &state)
{
    return state == QLatin1String("reply") || state == QLatin1String("waiting");
}
}

FollowUpController::FollowUpController(QObject *parent)
    : QObject(parent)
{
}

bool FollowUpController::validId(const QString &id, QChar sigil)
{
    // A composite timeline id (room + 0x1f + ...) must never be stored (§8).
    return id.size() > 1 && id.at(0) == sigil && !id.contains(QChar(0x1f));
}

qint64 FollowUpController::nowMs() const
{
    return m_now ? m_now() : QDateTime::currentMSecsSinceEpoch();
}

void FollowUpController::openFor(const QString &filePath)
{
    close();
    m_path = filePath;
    load();
    bump();
}

void FollowUpController::close()
{
    const bool had = !m_path.isEmpty() || !m_items.isEmpty() || !m_replied.isEmpty();
    m_path.clear();
    m_items.clear();
    m_replied.clear();
    if (had)
        bump();
}

void FollowUpController::bump()
{
    ++m_revision;
    Q_EMIT changed();
}

void FollowUpController::load()
{
    m_items.clear();
    QFile file(m_path);
    if (m_path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject o = v.toObject();
        Item item{ o.value(QStringLiteral("room")).toString(),
                   o.value(QStringLiteral("event")).toString(),
                   o.value(QStringLiteral("root")).toString(),
                   o.value(QStringLiteral("state")).toString(),
                   qint64(o.value(QStringLiteral("at")).toDouble()) };
        if (validId(item.roomId, QLatin1Char('!')) && validId(item.eventId, QLatin1Char('$'))
            && (item.rootId.isEmpty() || validId(item.rootId, QLatin1Char('$')))
            && validState(item.state) && m_items.size() < kMaxEntries
            && stateOf(item.roomId, item.eventId).isEmpty())
            m_items.append(item);
    }
}

void FollowUpController::write()
{
    if (m_path.isEmpty())
        return;
    if (m_items.isEmpty()) {
        QFile::remove(m_path);
        return;
    }
    QJsonArray items;
    for (const Item &item : std::as_const(m_items)) {
        QJsonObject o{ { QStringLiteral("room"), item.roomId },
                       { QStringLiteral("event"), item.eventId },
                       { QStringLiteral("state"), item.state },
                       { QStringLiteral("at"), double(item.atMs) } };
        if (!item.rootId.isEmpty())
            o.insert(QStringLiteral("root"), item.rootId);
        items.append(o);
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{ { QStringLiteral("version"), 1 },
                                          { QStringLiteral("items"), items } })
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

QVariantList FollowUpController::items() const
{
    QVector<Item> sorted = m_items;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Item &a, const Item &b) { return a.atMs > b.atMs; });
    QVariantList out;
    for (const Item &item : std::as_const(sorted)) {
        const QString name = m_nameOf ? m_nameOf(item.roomId) : QString();
        out.append(QVariantMap{
            { QStringLiteral("roomId"), item.roomId },
            { QStringLiteral("roomName"), name.isEmpty() ? item.roomId : name },
            { QStringLiteral("eventId"), item.eventId },
            { QStringLiteral("threadRootId"), item.rootId },
            { QStringLiteral("state"), item.state },
            { QStringLiteral("atMs"), item.atMs },
            { QStringLiteral("replied"), m_replied.contains(key(item.roomId, item.eventId)) },
        });
    }
    return out;
}

QString FollowUpController::stateOf(const QString &roomId, const QString &eventId) const
{
    for (const Item &item : m_items)
        if (item.roomId == roomId && item.eventId == eventId)
            return item.state;
    return {};
}

void FollowUpController::setState(const QString &roomId, const QString &eventId,
                                  const QString &threadRootId, const QString &state)
{
    if (m_path.isEmpty() || !validState(state) || !validId(roomId, QLatin1Char('!'))
        || !validId(eventId, QLatin1Char('$'))
        || (!threadRootId.isEmpty() && !validId(threadRootId, QLatin1Char('$'))))
        return;
    for (Item &item : m_items) {
        if (item.roomId != roomId || item.eventId != eventId)
            continue;
        if (item.state == state)
            return;
        item.state = state;
        item.atMs = nowMs();
        m_replied.remove(key(roomId, eventId));
        write();
        bump();
        return;
    }
    if (m_items.size() >= kMaxEntries)
        return;
    m_items.append(Item{ roomId, eventId, threadRootId, state, nowMs() });
    write();
    bump();
}

void FollowUpController::markDone(const QString &roomId, const QString &eventId)
{
    if (m_items.removeIf([&](const Item &item) {
            return item.roomId == roomId && item.eventId == eventId;
        }) == 0)
        return;
    m_replied.remove(key(roomId, eventId));
    write();
    bump();
}

bool FollowUpController::replied(const QString &roomId, const QString &eventId) const
{
    return m_replied.contains(key(roomId, eventId));
}

void FollowUpController::noteOwnMessage(const QString &roomId,
                                        const QString &replyToEventId,
                                        const QString &threadRootId)
{
    if (roomId.isEmpty() || (replyToEventId.isEmpty() && threadRootId.isEmpty()))
        return;
    bool offered = false;
    for (const Item &item : std::as_const(m_items)) {
        if (item.roomId != roomId || item.state != QLatin1String("reply"))
            continue;
        const bool answers = (!replyToEventId.isEmpty() && item.eventId == replyToEventId)
            || (!threadRootId.isEmpty()
                && (item.rootId == threadRootId || item.eventId == threadRootId));
        if (!answers || m_replied.contains(key(item.roomId, item.eventId)))
            continue;
        m_replied.insert(key(item.roomId, item.eventId));
        offered = true;
        Q_EMIT completionOffered(item.roomId, item.eventId);
    }
    if (offered)
        bump();
}
