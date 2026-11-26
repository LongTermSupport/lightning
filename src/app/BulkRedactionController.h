#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class MatrixClient;
class TimelineModel;

class BulkRedactionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool selecting READ selecting NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString timelineId READ timelineId NOTIFY changed)
    Q_PROPERTY(bool purge READ purge NOTIFY changed)
    Q_PROPERTY(bool purgeCounting READ purgeCounting NOTIFY changed)
    Q_PROPERTY(QString purgeUserId READ purgeUserId NOTIFY changed)
    Q_PROPERTY(int purgeMessageCount READ purgeMessageCount NOTIFY changed)
    Q_PROPERTY(int purgeStateCount READ purgeStateCount NOTIFY changed)
    Q_PROPERTY(bool purgeCapped READ purgeCapped NOTIFY changed)
    Q_PROPERTY(QString purgeName READ purgeName NOTIFY changed)
    Q_PROPERTY(bool purgeCanLookFurther READ purgeCanLookFurther NOTIFY changed)
public:
    explicit BulkRedactionController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    bool selecting() const { return m_selecting; }
    bool running() const { return m_running; }
    int selectedCount() const { return m_ids.size(); }
    int completed() const { return m_completed; }
    int total() const { return m_total; }
    int failed() const { return m_failed; }
    bool cancelled() const { return m_cancelled; }
    QString error() const { return m_error; }
    QString timelineId() const { return m_timelineId; }
    bool purge() const { return m_purge; }
    bool purgeCounting() const { return m_purgeOp != 0; }
    QString purgeUserId() const { return m_purgeUserId; }
    int purgeMessageCount() const { return m_purgeMessages.size(); }
    int purgeStateCount() const { return m_purgeState.size(); }
    bool purgeCapped() const { return m_purgeCapped; }
    QString purgeName() const { return m_purgeName; }
    bool purgeCanLookFurther() const
    {
        return m_purge && !m_running && !m_cancelled && m_purgeOp == 0
            && m_purgeCapped && m_purgeWalkable;
    }

    /// Pages of 100 events: one walk scans at most ~1,000.
    static constexpr int kPurgeWalkRequests = 10;

    Q_INVOKABLE void begin(TimelineModel *model);
    /// Starts a deletion selection holding `ids`; false if any was refused.
    Q_INVOKABLE bool adopt(TimelineModel *model, const QStringList &ids,
                           bool canRedactOthers);
    Q_INVOKABLE void toggle(const QString &eventId, bool canRedactOthers);
    Q_INVOKABLE void toggleFromPointer(const QString &eventId,
                                       bool canRedactOthers);
    Q_INVOKABLE void selectRange(const QString &first, const QString &last,
                                  bool canRedactOthers);
    Q_INVOKABLE bool isSelected(const QString &eventId) const;
    Q_INVOKABLE void start(const QString &reason);
    Q_INVOKABLE void cancel();
    /// Counts `userId`'s events for "Remove recent messages": the loaded
    /// timeline plus one bounded backward walk. Nothing is redacted here.
    Q_INVOKABLE bool preparePurge(TimelineModel *model, const QString &userId,
                                  const QString &displayName,
                                  bool canRedactOthers);
    /// Counts again from where the last walk stopped; walk results only.
    Q_INVOKABLE bool continuePurge(bool canRedactOthers);
    Q_INVOKABLE void startPurge(const QString &reason, bool keepStateEvents);

    void setRequestTimeoutForTest(int ms) { m_requestTimeoutMs = ms; }
    void setAbandonedExpiryForTest(qint64 ms) { m_abandonedExpiryMs = ms; }

signals:
    void changed();

private:
    void add(const QString &eventId, bool canRedactOthers);
    void sendNext();
    void finish();
    void onResult(const QString &roomId, const QString &eventId,
                  bool ok, const QString &category);
    void onRequestTimeout();
    void abandon(const QString &key, bool alreadyCounted);
    bool isAbandoned(const QString &key);
    void pruneAbandoned();
    void onUserRecentEvents(quint64 opId, const QString &roomId,
                            const QString &userId, bool ok,
                            const QStringList &messageIds,
                            const QStringList &stateIds,
                            const QString &nextFrom);
    void requestWalk(const QString &from);
    void onRateLimited(const QString &roomId, const QString &eventId,
                       qint64 retryAfterMs);
    void resetPurge();
    void scheduleNext(int delayMs);

    QPointer<MatrixClient> m_client;
    QPointer<TimelineModel> m_model;
    QString m_roomId;
    QString m_timelineId;
    QStringList m_ids;
    QStringList m_queue;
    QString m_inFlight;
    QString m_reason;
    QString m_anchor;
    QString m_error;
    /// Request key -> when it was abandoned (ms since epoch). Expired by
    /// pruneAbandoned(): a result that never comes must not block that
    /// message for the rest of the session.
    struct Abandoned {
        qint64 atMs = 0;
        bool counted = false;
    };
    QHash<QString, Abandoned> m_abandonedRequests;
    QTimer m_requestTimer;
    int m_requestTimeoutMs = 60000;
    qint64 m_abandonedExpiryMs = 10 * 60 * 1000;
    bool m_selecting = false;
    bool m_running = false;
    bool m_cancelled = false;
    int m_completed = 0;
    int m_total = 0;
    int m_failed = 0;
    quint64 m_generation = 0;

    bool m_purge = false;
    bool m_purgeCapped = false;
    bool m_purgeWalkable = false;
    quint64 m_purgeOp = 0;
    QString m_purgeUserId;
    QString m_purgeName;
    QString m_purgeFrom;
    QString m_purgeNextFrom;
    QStringList m_purgeMessages;
    QStringList m_purgeState;
    QHash<QString, qint64> m_retryAfterMs;
    QHash<QString, int> m_rateLimitRetries;
};
