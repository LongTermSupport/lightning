#include "app/RoomCardsController.h"

#include "app/RemindersController.h"
#include "matrix/MatrixClient.h"

#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QSaveFile>
#include <QTimeZone>
#include <QUuid>

#include <cstdlib>

namespace {

QTimeZone zoneFor(const QString &id)
{
    const QTimeZone zone(id.toUtf8());
    return zone.isValid() ? zone : QTimeZone(QTimeZone::UTC);
}

QString offsetText(const QDateTime &dt)
{
    const int secs = dt.offsetFromUtc();
    const QChar sign = secs < 0 ? QLatin1Char('-') : QLatin1Char('+');
    const int abs = std::abs(secs) / 60;
    return QStringLiteral("UTC%1%2:%3").arg(sign)
        .arg(abs / 60, 2, 10, QLatin1Char('0')).arg(abs % 60, 2, 10, QLatin1Char('0'));
}

QString neutral(qint64 ms, const QTimeZone &zone)
{
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms, zone);
    return QStringLiteral("%1 (%2, %3)")
        .arg(dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             QString::fromUtf8(zone.id()), offsetText(dt));
}

// RFC 5545 TEXT escaping; folding is left to the reader (lines stay short).
QString icsEscape(QString s)
{
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    s.replace(QLatin1Char(','), QStringLiteral("\\,"));
    s.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    s.remove(QLatin1Char('\r'));
    return s;
}

QString icsTime(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)
        .toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

QVariantMap normalised(QVariantMap data)
{
    // JS numbers arrive as doubles and would be written as 1.7e+12.
    for (const char *key : {"start", "end"})
        if (data.contains(QLatin1String(key)))
            data.insert(QLatin1String(key), data.value(QLatin1String(key)).toLongLong());
    return data;
}

} // namespace

RoomCardsController::RoomCardsController(QObject *parent)
    : QObject(parent)
{
}

void RoomCardsController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_cards.clear();
    m_pending.clear();
    m_pendingRsvp.clear();
    if (m_client) {
        connect(m_client, &MatrixClient::spaceCommunityFinished, this,
                [this](quint64 opId, const QString &roomId, const QString &action, bool ok,
                       const QVariantMap &result) { onFinished(opId, roomId, action, ok, result); });
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            m_cards.clear();
            m_pending.clear();
            m_pendingRsvp.clear();
            ++m_revision;
            Q_EMIT changed();
        });
    }
    ++m_revision;
    Q_EMIT changed();
}

void RoomCardsController::setReminders(RemindersController *reminders)
{
    m_reminders = reminders;
}

qint64 RoomCardsController::now() const
{
    return m_nowForTest ? m_nowForTest : QDateTime::currentMSecsSinceEpoch();
}

QVariantMap RoomCardsController::card(const QString &roomId, const QString &eventId) const
{
    return m_cards.value(key(roomId, eventId));
}

bool RoomCardsController::busy(const QString &roomId, const QString &eventId) const
{
    for (const Pending &p : m_pending)
        if (p.roomId == roomId && p.eventId == eventId && p.action != QLatin1String("card_load"))
            return true;
    return false;
}

quint64 RoomCardsController::send(const QString &roomId, const QString &eventId,
                                  const QString &action, const QVariantMap &payload)
{
    if (!m_client || !roomId.startsWith(QLatin1Char('!')))
        return 0;
    const quint64 op = m_client->spaceCommunityAction(roomId, action, payload);
    if (op) {
        m_pending.insert(op, {roomId, eventId, action});
        ++m_revision;
        Q_EMIT changed();
    }
    return op;
}

void RoomCardsController::load(const QString &roomId, const QString &eventId, bool server)
{
    if (!eventId.startsWith(QLatin1Char('$')))
        return;
    for (const Pending &p : std::as_const(m_pending))
        if (p.roomId == roomId && p.eventId == eventId && p.action == QLatin1String("card_load"))
            return;
    send(roomId, eventId, QStringLiteral("card_load"),
         {{QStringLiteral("root"), eventId}, {QStringLiteral("server"), server}});
}

bool RoomCardsController::create(const QString &roomId, const QString &kind, const QVariantMap &data)
{
    const QString msgtype = kind == QLatin1String("checklist")
        ? QStringLiteral("org.lightning_matrix.checklist")
        : kind == QLatin1String("event") ? QStringLiteral("org.lightning_matrix.event") : QString();
    if (msgtype.isEmpty())
        return false;
    for (const Pending &p : std::as_const(m_pending))
        if (p.action == QLatin1String("card_create"))
            return false;
    const QVariantMap clean = normalised(data);
    return send(roomId, {}, QStringLiteral("card_create"),
                {{QStringLiteral("kind"), msgtype}, {QStringLiteral("data"), clean},
                 {QStringLiteral("body"), fallbackBody(kind, clean)},
                 {QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces)}}) != 0;
}

bool RoomCardsController::edit(const QString &roomId, const QString &eventId, const QVariantMap &data)
{
    const QString kind = card(roomId, eventId).value(QStringLiteral("kind")).toString();
    if (kind.isEmpty() || busy(roomId, eventId))
        return false;
    const QVariantMap clean = normalised(data);
    return send(roomId, eventId, QStringLiteral("card_edit"),
                {{QStringLiteral("root"), eventId}, {QStringLiteral("data"), clean},
                 {QStringLiteral("body"), fallbackBody(kind, clean)}}) != 0;
}

bool RoomCardsController::update(const QString &roomId, const QString &eventId, const QString &op,
                                 const QString &item, const QString &text)
{
    if (busy(roomId, eventId))
        return false;
    return send(roomId, eventId, QStringLiteral("card_update"),
                {{QStringLiteral("root"), eventId}, {QStringLiteral("op"), op},
                 {QStringLiteral("item"), item}, {QStringLiteral("text"), text.trimmed()}}) != 0;
}

bool RoomCardsController::rsvp(const QString &roomId, const QString &eventId, const QString &status)
{
    if (busy(roomId, eventId))
        return false;
    const quint64 op = send(roomId, eventId, QStringLiteral("card_rsvp"),
                            {{QStringLiteral("root"), eventId}, {QStringLiteral("status"), status}});
    if (op)
        m_pendingRsvp.insert(op, status);
    return op != 0;
}

void RoomCardsController::onFinished(quint64 opId, const QString &roomId, const QString &action,
                                     bool ok, const QVariantMap &result)
{
    const auto it = m_pending.find(opId);
    if (it == m_pending.end() || it->roomId != roomId || it->action != action)
        return;
    const Pending done = *it;
    m_pending.erase(it);
    const QString category = result.value(QStringLiteral("category")).toString();
    QString message;
    if (!ok) {
        message = category == QLatin1String("forbidden")
            ? tr("You cannot change this here. Your permissions may have changed.")
            : category == QLatin1String("stale")
                ? tr("This item changed or was removed. The card has been refreshed.")
                : category == QLatin1String("invalid")
                    ? tr("This was not accepted. Check the fields and their limits.")
                    : tr("The server did not accept this. Try again.");
    }
    if (action == QLatin1String("card_create")) {
        Q_EMIT createFinished(ok, message);
    } else if (action == QLatin1String("card_load")) {
        if (ok) {
            m_cards.insert(key(roomId, done.eventId), result);
            syncReminder(roomId, done.eventId, result);
        }
    } else {
        const QString status = m_pendingRsvp.take(opId);
        if (!ok)
            Q_EMIT actionFailed(roomId, done.eventId, message);
        if (ok && action == QLatin1String("card_rsvp") && m_reminders) {
            const QVariantMap c = card(roomId, done.eventId);
            const qint64 start = c.value(QStringLiteral("start")).toLongLong();
            if (status == QLatin1String("no"))
                m_reminders->cancelReminder(roomId, done.eventId);
            else if (start - kReminderLeadMs > now())
                m_reminders->remind(roomId, done.eventId, {}, start - kReminderLeadMs);
        }
        load(roomId, done.eventId, false);
    }
    ++m_revision;
    Q_EMIT changed();
}

void RoomCardsController::syncReminder(const QString &roomId, const QString &eventId,
                                       const QVariantMap &card)
{
    if (!m_reminders || card.value(QStringLiteral("kind")).toString() != QLatin1String("event"))
        return;
    const qint64 due = m_reminders->reminderDue(roomId, eventId);
    if (!due)
        return;
    const QString own = card.value(QStringLiteral("own")).toString();
    if (card.value(QStringLiteral("deleted")).toBool()
        || card.value(QStringLiteral("cancelled")).toBool() || own == QLatin1String("no")) {
        m_reminders->cancelReminder(roomId, eventId);
        return;
    }
    const qint64 want = card.value(QStringLiteral("start")).toLongLong() - kReminderLeadMs;
    if (want != due && want > now())
        m_reminders->remind(roomId, eventId, {}, want);
}

qint64 RoomCardsController::toUtcMs(const QString &isoDate, const QString &hhmm,
                                    const QString &timeZone) const
{
    const QDate date = QDate::fromString(isoDate, Qt::ISODate);
    const QTime time = QTime::fromString(hhmm, QStringLiteral("HH:mm"));
    const QTimeZone zone(timeZone.toUtf8());
    if (!date.isValid() || !time.isValid() || !zone.isValid())
        return 0;
    const QDateTime dt(date, time, zone);
    // A wall time skipped by a DST change is moved by Qt; refuse it instead.
    if (!dt.isValid() || dt.date() != date || dt.time() != time)
        return 0;
    return dt.toMSecsSinceEpoch();
}

QVariantMap RoomCardsController::wallClock(qint64 ms, const QString &timeZone) const
{
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms, zoneFor(timeZone));
    return {{QStringLiteral("date"), dt.toString(QStringLiteral("yyyy-MM-dd"))},
            {QStringLiteral("time"), dt.toString(QStringLiteral("HH:mm"))}};
}

QString RoomCardsController::systemTimeZone() const
{
    return QString::fromUtf8(QTimeZone::systemTimeZoneId());
}

QStringList RoomCardsController::timeZones() const
{
    QStringList out;
    for (const QByteArray &id : QTimeZone::availableTimeZoneIds())
        out.append(QString::fromUtf8(id));
    return out;
}

QString RoomCardsController::formatWhen(qint64 startMs, qint64 endMs, const QString &timeZone) const
{
    const QTimeZone zone = zoneFor(timeZone);
    QString out = neutral(startMs, zone);
    if (endMs > startMs)
        out += QStringLiteral(" – ") + QDateTime::fromMSecsSinceEpoch(endMs, zone)
                   .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QTimeZone local = QTimeZone::systemTimeZone();
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(startMs, zone);
    if (local.offsetFromUtc(start) != start.offsetFromUtc())
        out += QLatin1Char('\n') + tr("Your time: %1").arg(
            QLocale().toString(QDateTime::fromMSecsSinceEpoch(startMs, local), QLocale::ShortFormat));
    return out;
}

QString RoomCardsController::fallbackBody(const QString &kind, const QVariantMap &data)
{
    // Sent to every reader, so it is not translated or localised.
    QStringList lines;
    if (kind == QLatin1String("checklist")) {
        const QString title = data.value(QStringLiteral("title")).toString().trimmed();
        lines.append(QStringLiteral("Checklist: %1").arg(title.isEmpty() ? QStringLiteral("(untitled)") : title));
        for (const QVariant &v : data.value(QStringLiteral("items")).toList())
            lines.append(QStringLiteral("☐ ") + v.toMap().value(QStringLiteral("text")).toString());
    } else if (kind == QLatin1String("event")) {
        const QTimeZone zone = zoneFor(data.value(QStringLiteral("tz")).toString());
        const qint64 start = data.value(QStringLiteral("start")).toLongLong();
        const qint64 end = data.value(QStringLiteral("end")).toLongLong();
        lines.append((data.value(QStringLiteral("cancelled")).toBool() ? QStringLiteral("Cancelled event: %1")
                                                                       : QStringLiteral("Event: %1"))
                         .arg(data.value(QStringLiteral("title")).toString()));
        QString when = QStringLiteral("When: ") + neutral(start, zone);
        if (end > start)
            when += QStringLiteral(" to ") + neutral(end, zone);
        lines.append(when);
        lines.append(QStringLiteral("UTC: ") + QDateTime::fromMSecsSinceEpoch(start, QTimeZone::UTC)
                         .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        const QString where = data.value(QStringLiteral("location")).toString();
        if (!where.isEmpty())
            lines.append(QStringLiteral("Where: ") + where);
        const QString link = data.value(QStringLiteral("link")).toString();
        if (!link.isEmpty())
            lines.append(QStringLiteral("Link: ") + link);
    }
    return lines.join(QLatin1Char('\n'));
}

QString RoomCardsController::icsText(const QVariantMap &card, const QString &roomId)
{
    const qint64 start = card.value(QStringLiteral("start")).toLongLong();
    if (card.value(QStringLiteral("kind")).toString() != QLatin1String("event") || start <= 0)
        return {};
    qint64 end = card.value(QStringLiteral("end")).toLongLong();
    if (end <= start)
        end = start + 60 * 60 * 1000;
    QStringList l{QStringLiteral("BEGIN:VCALENDAR"), QStringLiteral("VERSION:2.0"),
                  QStringLiteral("PRODID:-//Lightning//Matrix event//EN"),
                  QStringLiteral("BEGIN:VEVENT"),
                  QStringLiteral("UID:%1@%2").arg(card.value(QStringLiteral("root")).toString().mid(1),
                                                  roomId.section(QLatin1Char(':'), 1)),
                  QStringLiteral("DTSTAMP:") + icsTime(QDateTime::currentMSecsSinceEpoch()),
                  QStringLiteral("DTSTART:") + icsTime(start),
                  QStringLiteral("DTEND:") + icsTime(end),
                  QStringLiteral("SUMMARY:") + icsEscape(card.value(QStringLiteral("title")).toString())};
    const QString where = card.value(QStringLiteral("location")).toString();
    if (!where.isEmpty())
        l.append(QStringLiteral("LOCATION:") + icsEscape(where));
    const QString link = card.value(QStringLiteral("link")).toString();
    if (link.startsWith(QLatin1String("https://")))
        l.append(QStringLiteral("URL:") + icsEscape(link));
    if (card.value(QStringLiteral("cancelled")).toBool())
        l.append(QStringLiteral("STATUS:CANCELLED"));
    l << QStringLiteral("END:VEVENT") << QStringLiteral("END:VCALENDAR");
    return l.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

bool RoomCardsController::saveIcs(const QVariantMap &card, const QString &roomId, const QUrl &target)
{
    const QString text = icsText(card, roomId);
    if (text.isEmpty() || !target.isLocalFile())
        return false;
    QSaveFile file(target.toLocalFile());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(text.toUtf8());
    return file.commit();
}
