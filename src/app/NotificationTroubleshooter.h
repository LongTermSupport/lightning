#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

// "Why did I not get a notification?" A fixed list of checks, each with a
// plain verdict, run on demand, plus a real test notification.
//
// What it can and cannot know is part of the contract. It reads: whether a
// desktop notification service is on the session bus or can be started by it
// (Linux) or the tray can show a balloon (Windows, macOS), Lightning's own switch, the account's
// master mute, and the open room's mode. It CANNOT read the operating
// system's notification permission, nor Focus / Do Not Disturb except where a
// freedesktop service reports it as inhibited, so that check is otherwise
// "unknown" and says so; only the test notification settles it.
//
// simulate() asks matrix-sdk which push rule matches a synthetic message in a
// room (read-only), then names what this device would do on top of it.
class MatrixClient;

class NotificationTroubleshooter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList checks READ checks NOTIFY checksChanged)
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY checksChanged)
    Q_PROPERTY(QVariantMap simulation READ simulation NOTIFY simulationChanged)

public:
    enum Status { Pass, Warn, Fail, Unknown };
    Q_ENUM(Status)

    enum DeliveryPath { NoPath, DesktopService, TrayBalloon };
    Q_ENUM(DeliveryPath)

    struct Inputs {
        DeliveryPath delivery = NoPath;
        bool appEnabled = true;
        bool rulesKnown = false;
        bool masterMute = false;
        QString roomId;
        int roomMode = -1;
        int accountDefaultLevel = -1;
        bool trayIconShown = false;
        QString platform;
        bool roomIsDirect = false;
        bool roomSnoozed = false;
        bool quietNow = false;
        bool quietAllowsMentions = false;
        bool quietAllowsDirect = false;
        bool quietAllowsRoom = false;
        bool desktopInhibited = false;
    };

    explicit NotificationTroubleshooter(QObject *parent = nullptr);

    void setInputsProvider(std::function<Inputs(const QString &roomId)> provider)
    { m_provider = std::move(provider); }
    void setTestSender(std::function<void()> sender)
    { m_testSender = std::move(sender); }

    QVariantList checks() const { return m_checks; }
    bool hasRun() const { return m_hasRun; }

    static QVariantList evaluate(const Inputs &inputs);
    // What this device adds to a notifying rule; empty when it would show.
    static QString localOutcome(const Inputs &inputs, bool mention);
    static QString ruleName(const QString &ruleId, const QString &kind);

    void setClient(MatrixClient *client);
    QVariantMap simulation() const { return m_simulation; }

    static DeliveryPath probeDeliveryPath();
    static QString currentPlatform();

    Q_INVOKABLE void run(const QString &roomId);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void sendTest();
    Q_INVOKABLE void simulate(const QString &roomId, const QString &body,
                              bool mentionMe);

Q_SIGNALS:
    void checksChanged();
    void testSent();
    void simulationChanged();

private:
    std::function<Inputs(const QString &)> m_provider;
    std::function<void()> m_testSender;
    QVariantList m_checks;
    bool m_hasRun = false;
    MatrixClient *m_client = nullptr;
    QVariantMap m_simulation;
    quint64 m_simulationOp = 0;
    QString m_simulationRoom;
    void setSimulation(const QVariantMap &simulation);
};
