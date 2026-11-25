#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// A room's moderation case queue (rust/src/modqueue.rs): members covered by a
// followed policy list, one case each. Nothing is enforced automatically and
// there is no bulk path: a ban needs requestBan() then confirmBan() for the
// same member, one case at a time, and Rust re-checks the rule, the member
// and the viewer's power before sending it.
class ModerationQueueController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId NOTIFY changed)
    Q_PROPERTY(QVariantList cases READ cases NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canBan READ canBan NOTIFY changed)
    Q_PROPERTY(bool truncated READ truncated NOTIFY changed)
    Q_PROPERTY(int listCount READ listCount NOTIFY changed)
    Q_PROPERTY(QVariantMap pending READ pending NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
public:
    explicit ModerationQueueController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    QVariantList cases() const { return m_cases; }
    bool loading() const { return m_loadOp != 0; }
    bool busy() const { return m_actOp != 0; }
    bool canBan() const { return m_canBan; }
    bool truncated() const { return m_truncated; }
    int listCount() const { return m_listCount; }
    QVariantMap pending() const { return m_pending; }
    QString message() const { return m_message; }

    Q_INVOKABLE void open(const QString &roomId);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void close();
    // Shows what a ban would do; sends nothing.
    Q_INVOKABLE bool requestBan(const QString &userId);
    Q_INVOKABLE void cancelRequest();
    Q_INVOKABLE bool confirmBan(const QString &userId, const QString &reason);
    Q_INVOKABLE bool setDecision(const QString &userId, const QString &decision);

Q_SIGNALS:
    void changed();

private:
    void onFinished(quint64 opId, const QString &roomId, const QString &action, bool ok,
                    const QVariantMap &result);
    QVariantMap caseFor(const QString &userId) const;
    void setResult(const QString &userId, const QString &result);
    static QString describe(const QString &category);
    void reset();

    QPointer<MatrixClient> m_client;
    QString m_roomId;
    QString m_actUser;
    QVariantList m_cases;
    QVariantMap m_pending;
    QVariantMap m_results;
    QString m_message;
    quint64 m_loadOp = 0;
    quint64 m_actOp = 0;
    bool m_canBan = false;
    bool m_truncated = false;
    int m_listCount = 0;
};
