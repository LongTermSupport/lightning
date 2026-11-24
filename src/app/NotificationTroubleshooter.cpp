#include "app/NotificationTroubleshooter.h"

#include "matrix/MatrixClient.h"

#include <QSystemTrayIcon>
#include <QStringList>
#include <QVariantMap>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#endif

namespace {

QVariantMap check(const QString &id, const QString &title,
                  NotificationTroubleshooter::Status status,
                  const QString &detail, const QString &fix = QString())
{
    QVariantMap out;
    out.insert(QStringLiteral("id"), id);
    out.insert(QStringLiteral("title"), title);
    out.insert(QStringLiteral("status"), static_cast<int>(status));
    out.insert(QStringLiteral("detail"), detail);
    out.insert(QStringLiteral("fix"), fix);
    return out;
}

}

NotificationTroubleshooter::NotificationTroubleshooter(QObject *parent)
    : QObject(parent)
{
}

QString NotificationTroubleshooter::currentPlatform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

NotificationTroubleshooter::DeliveryPath
NotificationTroubleshooter::probeDeliveryPath()
{
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected() && bus.interface()) {
        const QString name = QStringLiteral("org.freedesktop.Notifications");
        // A daemon that is not running yet but is D-Bus activatable (dunst,
        // mako, xfce4-notifyd ...) is started by the first Notify call.
        if (bus.interface()->isServiceRegistered(name)
            || bus.interface()->activatableServiceNames().value().contains(name)) {
            return DesktopService;
        }
    }
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    return NoPath;
#else
    if (QSystemTrayIcon::isSystemTrayAvailable()
        && QSystemTrayIcon::supportsMessages()) {
        return TrayBalloon;
    }
    return NoPath;
#endif
}

QVariantList NotificationTroubleshooter::evaluate(const Inputs &in)
{
    QVariantList out;

    switch (in.delivery) {
    case DesktopService:
        out.append(check(QStringLiteral("delivery"),
                         tr("A notification service is running"), Pass,
                         tr("A notification service is available on your desktop's session "
                            "bus. Only the test notification can show that it "
                            "displays.")));
        break;
    case TrayBalloon:
        if (in.trayIconShown) {
            out.append(check(QStringLiteral("delivery"),
                             tr("Notifications use the tray icon"), Pass,
                             tr("Lightning shows notifications as balloons from "
                                "its tray icon on this system, and the icon is "
                                "showing.")));
        } else {
            out.append(check(
                QStringLiteral("delivery"),
                tr("The tray icon is not showing"), Fail,
                tr("Notifications can only appear as balloons from Lightning's "
                   "tray icon here, and the icon is hidden. Turn on keeping "
                   "Lightning in the tray, or start a notification service.")));
        }
        break;
    case NoPath:
        out.append(check(
            QStringLiteral("delivery"), tr("No way to show a notification"),
            Fail,
            in.platform == QLatin1String("linux")
                ? tr("Nothing is listening for notifications on the session bus "
                     "and there is no tray to fall back on. Start a notification "
                     "service (your desktop's own, or one such as dunst or "
                     "mako).")
                : tr("The system tray is not available, so there is nowhere to "
                     "show a notification from.")));
        break;
    }

    QString osDetail;
    if (in.platform == QLatin1String("macos")) {
        osDetail = tr("Lightning cannot read whether macOS allows its "
                      "notifications or whether a Focus mode is on. If the test "
                      "below does not appear, check System Settings, "
                      "Notifications and Focus.");
    } else if (in.platform == QLatin1String("windows")) {
        osDetail = tr("Lightning cannot read whether Windows allows its "
                      "notifications or whether Focus assist is on. If the test "
                      "below does not appear, check Settings, System, "
                      "Notifications.");
    } else {
        osDetail = tr("Lightning cannot read whether your desktop is in Do Not "
                      "Disturb or has muted Lightning. If the test below does "
                      "not appear, check your desktop's notification settings.");
    }
    if (in.desktopInhibited) {
        out.append(check(QStringLiteral("system"),
                         tr("Your desktop is in Do Not Disturb"), Warn,
                         tr("Your notification service says it is not "
                            "showing notifications right now. Lightning "
                            "still sends them, and your desktop keeps them "
                            "in its history.")));
    } else {
        out.append(check(QStringLiteral("system"),
                         tr("System permission and Do Not Disturb"), Unknown,
                         osDetail));
    }

    out.append(in.appEnabled
        ? check(QStringLiteral("app"), tr("Desktop notifications are on"), Pass,
                tr("Lightning is allowed to show notifications."))
        : check(QStringLiteral("app"), tr("Desktop notifications are off"), Fail,
                tr("Lightning is set not to show any desktop notification."),
                QStringLiteral("enableApp")));

    if (!in.rulesKnown) {
        out.append(check(QStringLiteral("account"),
                         tr("Account notification settings"), Unknown,
                         tr("They have not been loaded from your homeserver "
                            "yet.")));
    } else if (in.masterMute) {
        out.append(check(QStringLiteral("account"),
                         tr("All notifications are muted on your account"), Fail,
                         tr("The master mute is on, so your homeserver sends no "
                            "notifications for any room, to any of your "
                            "devices."),
                         QStringLiteral("unmute")));
    } else if (in.accountDefaultLevel == 1 && !in.roomId.isEmpty()
               && (in.roomMode == -1 || in.roomMode == 3)) {
        out.append(check(QStringLiteral("account"),
                         tr("Your account default is mentions and keywords "
                            "only"), Warn,
                         tr("The master mute is off, but ordinary messages in "
                            "rooms without a setting of their own stay quiet. "
                            "Change it under Default alerts above.")));
    } else {
        out.append(check(QStringLiteral("account"),
                         tr("Your account is not muted"), Pass,
                         tr("The master mute is off.")));
    }

    if (!in.roomId.isEmpty()) {
        if (in.roomMode == 2) {
            out.append(check(QStringLiteral("room"),
                             tr("This room is muted"), Fail,
                             tr("It never notifies. Change it under Room "
                                "information, Notifications.")));
        } else if (in.roomMode == 1) {
            out.append(check(QStringLiteral("room"),
                             tr("This room notifies only for mentions"), Warn,
                             tr("Ordinary messages here stay quiet. Change it "
                                "under Room information, Notifications.")));
        } else if (in.roomMode == 0) {
            out.append(check(QStringLiteral("room"),
                             tr("This room notifies for every message"), Pass,
                             tr("Nothing is overriding it.")));
        } else if (in.accountDefaultLevel == 1) {
            out.append(check(QStringLiteral("room"),
                             tr("This room follows your defaults, which are "
                                "mentions only"), Warn,
                             tr("It has no setting of its own, so ordinary "
                                "messages here stay quiet. Change the default "
                                "above, or set this room under Room "
                                "information, Notifications.")));
        } else if (in.accountDefaultLevel == 0) {
            out.append(check(QStringLiteral("room"),
                             tr("This room notifies for every message"), Pass,
                             tr("It has no setting of its own and follows your "
                                "account default, which is all messages.")));
        } else {
            out.append(check(QStringLiteral("room"),
                             tr("This room follows your defaults"), Unknown,
                             tr("It has no setting of its own, and your "
                                "account default could not be read, so "
                                "whether ordinary messages notify is not "
                                "known.")));
        }
    }
    if (in.roomSnoozed) {
        out.append(check(QStringLiteral("snooze"),
                         tr("This conversation is snoozed"), Warn,
                         tr("Its notifications are held until the snooze "
                            "ends, then summed up in one notification. "
                            "Unsnooze it from the room's menu.")));
    }
    if (in.quietNow) {
        QStringList still;
        if (in.quietAllowsMentions)
            still << tr("Mentions still notify.");
        if (in.quietAllowsDirect)
            still << tr("Direct messages still notify.");
        if (in.quietAllowsRoom)
            still << tr("This room still notifies.");
        still << tr("Calls still ring.");
        out.append(check(QStringLiteral("quiet"), tr("Quiet hours are on"),
                         Warn,
                         tr("Notifications are held until quiet hours end, "
                            "then summed up in one notification. They still "
                            "reach Activity and the unread counts.")
                             + QLatin1Char(' ')
                             + still.join(QLatin1Char(' '))));
    }
    return out;
}

QString NotificationTroubleshooter::localOutcome(const Inputs &in, bool mention)
{
    if (!in.appEnabled)
        return tr("Lightning would not show it: desktop notifications are "
                  "off on this device.");
    if (in.roomMode == 2)
        return tr("Lightning would not show it: this room is muted.");
    if (in.roomSnoozed)
        return tr("Lightning would hold it until this conversation's snooze "
                  "ends.");
    if (in.quietNow && !(mention && in.quietAllowsMentions)
        && !(in.roomIsDirect && in.quietAllowsDirect) && !in.quietAllowsRoom)
        return tr("Lightning would hold it until quiet hours end.");
    if (in.desktopInhibited)
        return tr("Lightning would send it, but your desktop is in Do Not "
                  "Disturb and will not show it now.");
    return {};
}

QString NotificationTroubleshooter::ruleName(const QString &ruleId,
                                             const QString &kind)
{
    if (kind == QLatin1String("room"))
        return tr("This room's own setting");
    if (kind == QLatin1String("content"))
        return ruleId.startsWith(QLatin1Char('.'))
            ? tr("Your user name")
            : tr("Keyword “%1”").arg(ruleId);
    if (ruleId == QLatin1String(".m.rule.master"))
        return tr("Mute all notifications");
    if (ruleId == QLatin1String(".m.rule.is_user_mention")
        || ruleId == QLatin1String(".m.rule.contains_display_name"))
        return tr("Mentions of you");
    if (ruleId == QLatin1String(".m.rule.is_room_mention")
        || ruleId == QLatin1String(".m.rule.roomnotif"))
        return tr("Mentions of the whole room");
    if (ruleId == QLatin1String(".m.rule.room_one_to_one")
        || ruleId == QLatin1String(".m.rule.encrypted_room_one_to_one"))
        return tr("Direct messages");
    if (ruleId == QLatin1String(".m.rule.message")
        || ruleId == QLatin1String(".m.rule.encrypted"))
        return tr("Messages in group rooms");
    return ruleId;
}

void NotificationTroubleshooter::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_simulationOp = 0;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::pushRuleSimulated, this,
            [this](quint64 op, bool ok, const QVariantMap &result) {
        if (op == 0 || op != m_simulationOp)
            return;
        m_simulationOp = 0;
        if (!ok) {
            setSimulation({ { QStringLiteral("state"), QStringLiteral("failed") } });
            return;
        }
        const bool notify = result.value(QStringLiteral("notify")).toBool();
        const bool highlight = result.value(QStringLiteral("highlight")).toBool();
        const QString ruleId = result.value(QStringLiteral("rule_id")).toString();
        Inputs in;
        if (m_provider)
            in = m_provider(m_simulationRoom);
        setSimulation({
            { QStringLiteral("state"), QStringLiteral("done") },
            { QStringLiteral("matched"), result.value(QStringLiteral("matched")) },
            { QStringLiteral("ruleId"), ruleId },
            { QStringLiteral("ruleName"),
              ruleName(ruleId, result.value(QStringLiteral("kind")).toString()) },
            { QStringLiteral("notify"), notify },
            { QStringLiteral("highlight"), highlight },
            { QStringLiteral("sound"), result.value(QStringLiteral("sound")).toBool() },
            { QStringLiteral("local"), notify ? localOutcome(in, highlight) : QString() },
        });
    });
}

void NotificationTroubleshooter::setSimulation(const QVariantMap &simulation)
{
    m_simulation = simulation;
    Q_EMIT simulationChanged();
}

void NotificationTroubleshooter::simulate(const QString &roomId,
                                          const QString &body, bool mentionMe)
{
    m_simulationOp = 0;
    if (roomId.isEmpty()) {
        setSimulation({ { QStringLiteral("state"), QStringLiteral("noRoom") } });
        return;
    }
    const quint64 op = m_client
        ? m_client->simulatePushRule(roomId, body.left(500), mentionMe) : 0;
    if (op == 0) {
        setSimulation({ { QStringLiteral("state"), QStringLiteral("unsupported") } });
        return;
    }
    m_simulationOp = op;
    m_simulationRoom = roomId;
    setSimulation({ { QStringLiteral("state"), QStringLiteral("pending") } });
}

void NotificationTroubleshooter::run(const QString &roomId)
{
    Inputs in;
    if (m_provider)
        in = m_provider(roomId);
    m_checks = evaluate(in);
    m_hasRun = true;
    Q_EMIT checksChanged();
}

void NotificationTroubleshooter::reset()
{
    m_simulationOp = 0;
    if (!m_simulation.isEmpty())
        setSimulation({});
    if (m_checks.isEmpty() && !m_hasRun)
        return;
    m_checks.clear();
    m_hasRun = false;
    Q_EMIT checksChanged();
}

void NotificationTroubleshooter::sendTest()
{
    if (m_testSender)
        m_testSender();
    Q_EMIT testSent();
}
