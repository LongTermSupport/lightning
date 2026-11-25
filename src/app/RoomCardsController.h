#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

class MatrixClient;
class RemindersController;

// Shared checklists and community events (rust/src/roomcards.rs). The card
// state is folded in Rust from the root, its sender's edits and the
// reference-related updates; this class routes the requests, builds the
// plain-text fallback other clients show, and keeps event reminders in step.
class RoomCardsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    static constexpr qint64 kReminderLeadMs = 15 * 60 * 1000;

    explicit RoomCardsController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    void setReminders(RemindersController *reminders);

    int revision() const { return m_revision; }

    Q_INVOKABLE QVariantMap card(const QString &roomId, const QString &eventId) const;
    Q_INVOKABLE bool busy(const QString &roomId, const QString &eventId) const;
    Q_INVOKABLE void load(const QString &roomId, const QString &eventId, bool server);
    Q_INVOKABLE bool create(const QString &roomId, const QString &kind, const QVariantMap &data);
    Q_INVOKABLE bool edit(const QString &roomId, const QString &eventId, const QVariantMap &data);
    Q_INVOKABLE bool update(const QString &roomId, const QString &eventId, const QString &op,
                            const QString &item, const QString &text);
    Q_INVOKABLE bool rsvp(const QString &roomId, const QString &eventId, const QString &status);

    // Wall-clock date and time in an IANA zone, as UTC ms; 0 when the time
    // does not exist there (a DST gap) or the input is invalid.
    Q_INVOKABLE qint64 toUtcMs(const QString &isoDate, const QString &hhmm,
                               const QString &timeZone) const;
    // {date: "yyyy-MM-dd", time: "HH:mm"} of `ms` in the zone.
    Q_INVOKABLE QVariantMap wallClock(qint64 ms, const QString &timeZone) const;
    Q_INVOKABLE QString systemTimeZone() const;
    Q_INVOKABLE QStringList timeZones() const;
    // "Fri 3 Apr 2026, 18:00 (Europe/Vilnius, UTC+03:00)" in the event's zone,
    // plus the reader's own time when the zones differ.
    Q_INVOKABLE QString formatWhen(qint64 startMs, qint64 endMs, const QString &timeZone) const;
    static QString fallbackBody(const QString &kind, const QVariantMap &data);
    static QString icsText(const QVariantMap &card, const QString &roomId);
    Q_INVOKABLE bool saveIcs(const QVariantMap &card, const QString &roomId, const QUrl &target);

    void setNowForTest(qint64 ms) { m_nowForTest = ms; }

Q_SIGNALS:
    void changed();
    void createFinished(bool ok, const QString &message);
    void actionFailed(const QString &roomId, const QString &eventId, const QString &message);

private:
    struct Pending { QString roomId; QString eventId; QString action; };
    quint64 send(const QString &roomId, const QString &eventId, const QString &action,
                 const QVariantMap &payload);
    void onFinished(quint64 opId, const QString &roomId, const QString &action, bool ok,
                    const QVariantMap &result);
    void syncReminder(const QString &roomId, const QString &eventId, const QVariantMap &card);
    qint64 now() const;
    static QString key(const QString &roomId, const QString &eventId)
    { return roomId + QLatin1Char('\x1f') + eventId; }

    QPointer<MatrixClient> m_client;
    QPointer<RemindersController> m_reminders;
    QHash<QString, QVariantMap> m_cards;
    QHash<quint64, Pending> m_pending;
    QHash<quint64, QString> m_pendingRsvp;
    int m_revision = 0;
    qint64 m_nowForTest = 0;
};
