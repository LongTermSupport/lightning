#include "app/RemindersController.h"

#include "matrix/MatrixClient.h"

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
constexpr int kMaxEntries = 500;
constexpr int kMaxTimerMs = 60 * 1000;
constexpr int kResolveTimeoutMs = 10 * 1000;
constexpr int kSummaryRooms = 3;

QString isolate(const QString &text)
{
    return QChar(0x2068) + text + QChar(0x2069);
}
}

RemindersController::RemindersController(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &RemindersController::processDue);
}

void RemindersController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    if (m_client)
        connect(m_client, &MatrixClient::eventSourceReceived, this,
                &RemindersController::receiveSource);
}

qint64 RemindersController::nowMs() const
{
    return m_nowForTest >= 0 ? m_nowForTest
                             : QDateTime::currentMSecsSinceEpoch();
}

void RemindersController::openFor(const QString &filePath)
{
    close();
    m_path = filePath;
    load();
    bump();
    // A reminder that fell due while the app was closed fires once now.
    QTimer::singleShot(0, this, [this, path = m_path] {
        if (m_path == path && !m_path.isEmpty())
            processDue();
    });
}

void RemindersController::close()
{
    m_timer.stop();
    m_resolving.clear();
    const bool had = !m_path.isEmpty() || !m_reminders.isEmpty()
        || !m_snoozes.isEmpty();
    m_path.clear();
    m_reminders.clear();
    m_snoozes.clear();
    if (had)
        bump();
}

void RemindersController::bump()
{
    ++m_revision;
    Q_EMIT changed();
}

void RemindersController::load()
{
    m_reminders.clear();
    m_snoozes.clear();
    QFile file(m_path);
    if (m_path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("reminders")).toArray()) {
        const QJsonObject o = v.toObject();
        Reminder r{ o.value(QStringLiteral("room")).toString(),
                    o.value(QStringLiteral("event")).toString(),
                    o.value(QStringLiteral("root")).toString(),
                    qint64(o.value(QStringLiteral("due")).toDouble()) };
        if (r.roomId.startsWith(QLatin1Char('!'))
            && r.eventId.startsWith(QLatin1Char('$')) && r.dueMs > 0
            && m_reminders.size() < kMaxEntries)
            m_reminders.append(r);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("snoozes")).toArray()) {
        const QJsonObject o = v.toObject();
        Snooze s{ o.value(QStringLiteral("room")).toString(),
                  qint64(o.value(QStringLiteral("until")).toDouble()) };
        if (s.roomId.startsWith(QLatin1Char('!')) && s.untilMs > 0
            && m_snoozes.size() < kMaxEntries)
            m_snoozes.append(s);
    }
}

void RemindersController::write()
{
    if (m_path.isEmpty())
        return;
    QJsonArray reminders;
    for (const Reminder &r : std::as_const(m_reminders)) {
        QJsonObject o{ { QStringLiteral("room"), r.roomId },
                       { QStringLiteral("event"), r.eventId },
                       { QStringLiteral("due"), double(r.dueMs) } };
        if (!r.rootId.isEmpty())
            o.insert(QStringLiteral("root"), r.rootId);
        reminders.append(o);
    }
    QJsonArray snoozes;
    for (const Snooze &s : std::as_const(m_snoozes))
        snoozes.append(QJsonObject{ { QStringLiteral("room"), s.roomId },
                                    { QStringLiteral("until"), double(s.untilMs) } });
    if (reminders.isEmpty() && snoozes.isEmpty()) {
        QFile::remove(m_path);
        return;
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("reminders"), reminders },
        { QStringLiteral("snoozes"), snoozes } }).toJson(QJsonDocument::Compact));
    file.commit();
}

void RemindersController::arm()
{
    qint64 next = -1;
    for (const Reminder &r : std::as_const(m_reminders))
        next = next < 0 ? r.dueMs : std::min(next, r.dueMs);
    for (const Snooze &s : std::as_const(m_snoozes))
        next = next < 0 ? s.untilMs : std::min(next, s.untilMs);
    if (next < 0) {
        m_timer.stop();
        return;
    }
    // Capped so a suspend or a clock change is noticed within a minute.
    m_timer.start(int(std::clamp<qint64>(next - nowMs(), 0, kMaxTimerMs)));
}

QVariantList RemindersController::snoozed() const
{
    QVariantList out;
    for (const Snooze &s : std::as_const(m_snoozes)) {
        if (s.untilMs <= nowMs())
            continue;
        const QString name = m_client ? m_client->roomInfo(s.roomId).name
                                      : QString();
        out.append(QVariantMap{ { QStringLiteral("roomId"), s.roomId },
                                { QStringLiteral("roomName"),
                                  name.isEmpty() ? s.roomId : name },
                                { QStringLiteral("until"), s.untilMs } });
    }
    return out;
}

void RemindersController::remind(const QString &roomId, const QString &eventId,
                                 const QString &threadRootId, qint64 dueMs)
{
    if (m_path.isEmpty() || !roomId.startsWith(QLatin1Char('!'))
        || !eventId.startsWith(QLatin1Char('$')) || dueMs <= nowMs())
        return;
    m_reminders.removeIf([&](const Reminder &r) {
        return r.roomId == roomId && r.eventId == eventId;
    });
    if (m_reminders.size() >= kMaxEntries)
        return;
    m_reminders.append(Reminder{ roomId, eventId, threadRootId, dueMs });
    write();
    arm();
    bump();
}

void RemindersController::cancelReminder(const QString &roomId,
                                         const QString &eventId)
{
    if (m_reminders.removeIf([&](const Reminder &r) {
            return r.roomId == roomId && r.eventId == eventId;
        }) == 0)
        return;
    write();
    arm();
    bump();
}

qint64 RemindersController::reminderDue(const QString &roomId,
                                        const QString &eventId) const
{
    for (const Reminder &r : std::as_const(m_reminders))
        if (r.roomId == roomId && r.eventId == eventId)
            return r.dueMs;
    return 0;
}

void RemindersController::snooze(const QString &roomId, qint64 untilMs)
{
    if (m_path.isEmpty() || !roomId.startsWith(QLatin1Char('!'))
        || untilMs <= nowMs())
        return;
    m_snoozes.removeIf([&](const Snooze &s) { return s.roomId == roomId; });
    if (m_snoozes.size() >= kMaxEntries)
        return;
    m_snoozes.append(Snooze{ roomId, untilMs });
    write();
    arm();
    bump();
}

void RemindersController::unsnooze(const QString &roomId)
{
    if (m_snoozes.removeIf([&](const Snooze &s) { return s.roomId == roomId; })
        == 0)
        return;
    write();
    arm();
    bump();
    Q_EMIT snoozeEnded(roomId);
}

qint64 RemindersController::snoozedUntil(const QString &roomId) const
{
    for (const Snooze &s : std::as_const(m_snoozes))
        if (s.roomId == roomId && s.untilMs > nowMs())
            return s.untilMs;
    return 0;
}

qint64 RemindersController::presetTime(const QString &preset) const
{
    const qint64 now = nowMs();
    if (preset == QLatin1String("20m"))
        return now + 20 * 60 * 1000;
    if (preset == QLatin1String("1h"))
        return now + 60 * 60 * 1000;
    if (preset == QLatin1String("3h"))
        return now + 3 * 60 * 60 * 1000;
    if (preset == QLatin1String("tomorrow")) {
        const QDateTime local = QDateTime::fromMSecsSinceEpoch(now);
        return QDateTime(local.date().addDays(1), QTime(9, 0))
            .toMSecsSinceEpoch();
    }
    return 0;
}

void RemindersController::processDue()
{
    if (m_path.isEmpty())
        return;
    const qint64 now = nowMs();
    QVector<Reminder> due;
    m_reminders.removeIf([&](const Reminder &r) {
        if (r.dueMs > now)
            return false;
        due.append(r);
        return true;
    });
    QStringList ended;
    m_snoozes.removeIf([&](const Snooze &s) {
        if (s.untilMs > now)
            return false;
        ended.append(s.roomId);
        return true;
    });
    if (!due.isEmpty() || !ended.isEmpty()) {
        // Written before any card, so a reminder can never fire twice.
        write();
        bump();
    }
    arm();
    for (const QString &roomId : std::as_const(ended))
        Q_EMIT snoozeEnded(roomId);
    if (due.size() == 1) {
        fire(due.constFirst());
    } else if (due.size() > 1) {
        QStringList names;
        for (const Reminder &r : std::as_const(due)) {
            QString name = m_client ? m_client->roomInfo(r.roomId).name
                                    : QString();
            if (name.isEmpty())
                name = tr("a room");
            if (!names.contains(name))
                names.append(name);
        }
        const bool hidden = m_previewFor
            && m_previewFor(due.constFirst().roomId) == Private;
        QString body = tr("%n reminder(s) are due.", nullptr, int(due.size()));
        if (!hidden && names.size() <= kSummaryRooms)
            body += QLatin1Char(' ') + tr("In %1.").arg(names.join(QStringLiteral(", ")));
        Q_EMIT notify(tr("Reminders"), body, QString(), QString(), QString());
    }
}

void RemindersController::fire(const Reminder &reminder)
{
    const quint64 op = m_client
        ? m_client->requestEventSource(reminder.roomId, reminder.eventId)
        : 0;
    if (op == 0) {
        post(reminder, false, false, QString(), QString());
        return;
    }
    m_resolving.insert(op, reminder);
    QTimer::singleShot(kResolveTimeoutMs, this, [this, op] {
        if (!m_resolving.contains(op))
            return;
        post(m_resolving.take(op), false, false, QString(), QString());
    });
}

void RemindersController::receiveSource(const QString &roomId,
                                        const QString &eventId, bool ok,
                                        const QString &json,
                                        const QVariantMap &meta)
{
    const quint64 op = meta.value(QStringLiteral("opId")).toULongLong();
    const auto it = m_resolving.constFind(op);
    if (it == m_resolving.constEnd() || it->roomId != roomId
        || it->eventId != eventId)
        return;
    const Reminder reminder = m_resolving.take(op);
    if (!ok) {
        post(reminder, false, false, QString(), QString());
        return;
    }
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    const bool redacted = object.value(QStringLiteral("unsigned")).toObject()
                              .value(QStringLiteral("redacted_because")).isObject();
    QJsonObject content = object.value(QStringLiteral("content")).toObject();
    const QVariantMap edited = meta.value(QStringLiteral("effectiveContent")).toMap();
    if (!edited.isEmpty())
        content = QJsonObject::fromVariantMap(edited);
    const QString type = object.value(QStringLiteral("type")).toString();
    QString text;
    if (!redacted && type == QLatin1String("m.room.message")) {
        const QString msgtype = content.value(QStringLiteral("msgtype")).toString();
        text = (msgtype == QLatin1String("m.text")
                || msgtype == QLatin1String("m.notice")
                || msgtype == QLatin1String("m.emote"))
            ? content.value(QStringLiteral("body")).toString().simplified().left(160)
            : tr("An attachment");
    } else if (!redacted && type == QLatin1String("m.sticker")) {
        text = tr("A sticker");
    }
    post(reminder, true, redacted || content.isEmpty(),
         object.value(QStringLiteral("sender")).toString(), text);
}

void RemindersController::post(const Reminder &reminder, bool found,
                               bool redacted, const QString &sender,
                               const QString &text)
{
    const int preview = m_previewFor ? m_previewFor(reminder.roomId) : SenderOnly;
    if (redacted) {
        Q_EMIT notify(tr("Reminder"),
                      tr("A message you asked to be reminded about was removed."),
                      reminder.roomId, QString(), QString());
        return;
    }
    if (preview == Private) {
        Q_EMIT notify(QStringLiteral("Lightning"), tr("Reminder"),
                      reminder.roomId, reminder.eventId, reminder.rootId);
        return;
    }
    QString room = m_client ? m_client->roomInfo(reminder.roomId).name : QString();
    if (room.isEmpty())
        room = tr("a room");
    const QString name = found && !sender.isEmpty() && m_client
        ? m_client->displayNameFor(reminder.roomId, sender) : QString();
    QString body;
    if (preview == SenderAndMessage && !text.isEmpty() && !name.isEmpty())
        body = tr("%1: %2").arg(isolate(name), text);
    else if (!name.isEmpty())
        body = tr("A message from %1").arg(isolate(name));
    else
        body = tr("A message in this room");
    Q_EMIT notify(tr("Reminder: %1").arg(isolate(room)), body, reminder.roomId,
                  reminder.eventId, reminder.rootId);
}
