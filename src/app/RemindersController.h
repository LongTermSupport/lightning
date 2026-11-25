#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include <functional>

class MatrixClient;

// Message reminders and conversation snoozes for the signed-in account. The
// file under the account's recorded root holds ids and times only: a fired
// reminder reads its message from the SDK at that moment.
class RemindersController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QVariantList snoozed READ snoozed NOTIFY changed)
public:
    enum Preview { SenderAndMessage = 0, SenderOnly = 1, Private = 2 };

    explicit RemindersController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    void setPreviewPolicy(std::function<int(const QString &roomId)> policy)
    { m_previewFor = std::move(policy); }

    void openFor(const QString &filePath);
    void close();
    QString filePath() const { return m_path; }

    int revision() const { return m_revision; }
    QVariantList snoozed() const;

    Q_INVOKABLE void remind(const QString &roomId, const QString &eventId,
                            const QString &threadRootId, qint64 dueMs);
    Q_INVOKABLE void cancelReminder(const QString &roomId,
                                    const QString &eventId);
    Q_INVOKABLE qint64 reminderDue(const QString &roomId,
                                   const QString &eventId) const;
    Q_INVOKABLE void snooze(const QString &roomId, qint64 untilMs);
    Q_INVOKABLE void unsnooze(const QString &roomId);
    Q_INVOKABLE qint64 snoozedUntil(const QString &roomId) const;
    // "20m", "1h", "3h", "tomorrow" (09:00 local) as an epoch time.
    Q_INVOKABLE qint64 presetTime(const QString &preset) const;
    bool isSnoozed(const QString &roomId) const
    { return snoozedUntil(roomId) > 0; }

    void setNowForTest(qint64 ms) { m_nowForTest = ms; }
    void runDueForTest() { processDue(); }

Q_SIGNALS:
    void changed();
    void notify(const QString &title, const QString &body,
                const QString &roomId, const QString &eventId,
                const QString &threadRootId);
    void snoozeEnded(const QString &roomId);

public:
    struct Reminder {
        QString roomId;
        QString eventId;
        QString rootId;
        qint64 dueMs = 0;
    };
    const QVector<Reminder> &reminders() const { return m_reminders; }

private:
    struct Snooze {
        QString roomId;
        qint64 untilMs = 0;
    };
    qint64 nowMs() const;
    void load();
    void write();
    void arm();
    void processDue();
    void fire(const Reminder &reminder);
    void post(const Reminder &reminder, bool found, bool redacted,
              const QString &sender, const QString &text);
    void receiveSource(const QString &roomId, const QString &eventId,
                       bool ok, const QString &json, const QVariantMap &meta);
    void bump();

    MatrixClient *m_client = nullptr;
    std::function<int(const QString &)> m_previewFor;
    QString m_path;
    QVector<Reminder> m_reminders;
    QVector<Snooze> m_snoozes;
    QHash<quint64, Reminder> m_resolving;
    QTimer m_timer;
    qint64 m_nowForTest = -1;
    int m_revision = 0;
};
