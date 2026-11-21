#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class MatrixClient;

class UserProfileResolver : public QObject
{
    Q_OBJECT

public:
    struct Profile {
        QString displayName;
        QString avatarUrl;
        bool known = false;
    };

    explicit UserProfileResolver(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void clear();

    Profile profile(const QString &userId) const;

    Q_INVOKABLE QVariantMap lookup(const QString &userId);
    Q_INVOKABLE void request(const QString &userId);

    int inFlightCount() const { return m_inFlight.size(); }
    int cachedCount() const { return m_profiles.size(); }
    int failedCount() const { return m_failedAt.size(); }
    void setFailureRetryForTest(int ms) { m_failureRetryMs = ms; expireFailures(); }

Q_SIGNALS:
    void resolved(const QString &userId, const QString &displayName,
                  const QString &avatarUrl);

private:
    void request(const QString &userId, bool interactive);
    void fetch(const QString &userId, bool interactive = false);
    void makeRoom(const QString &userId);
    void expireFailures();
    void onFinished(quint64 opId, bool ok, const QString &userId,
                    const QString &displayName, const QString &avatarUrl,
                    const QString &category);

    // Completed profiles, retry records and dispatched reads share this budget.
    static constexpr int kMaxProfiles = 4000;
    static constexpr int kMaxInFlight = 16;
    static constexpr int kMaxWaiting = 32;

    MatrixClient *m_client = nullptr;
    QHash<QString, Profile> m_profiles;
    QHash<quint64, QString> m_inFlight;
    QSet<QString> m_asking;
    QHash<QString, qint64> m_failedAt;
    QStringList m_profileOrder;
    QStringList m_failureOrder;
    QStringList m_waiting;
    QTimer m_retryExpiry;
    QElapsedTimer m_clock;
    int m_failureRetryMs = 5 * 60 * 1000;
};
