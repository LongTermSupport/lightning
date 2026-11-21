// "Send later": ScheduledSendController policy.
//
//   * a message is scheduled server-side (MSC4140) only when the server
//     supports delayed events, the room is known unencrypted, and the message
//     has no thread/reply relation; everything else stays in the local queue;
//   * a local entry for an unencrypted room persists across restarts; one for
//     an encrypted (or unknown) room is memory-only, so scheduled plaintext
//     never touches disk;
//   * a due local entry is marked "sending" and persisted before the send
//     leaves, goes through the room-level send (any room), and leaves the
//     queue only on the room's acceptance; a refusal is reported;
//   * nothing is dispatched until the sync runs (starting/retrying is not
//     connected); the reconnect fires it, earliest deadline first;
//   * a network/rate-limit failure stays pending for the next reconnect and is
//     resent under the same transaction id; any other refusal is failed;
//   * a row found in "sending" on the next start is reported as unsent and
//     never re-fired on a guess;
//   * server-side mutations are serialized on the entry's one in-flight op: a
//     reschedule/edit cancels first and resubmits only on "cancelled"; a
//     failed cancel is reported and never followed by a replacement (that
//     would deliver twice); changes asked mid-schedule apply once the delay
//     id arrives;
//   * a server-held entry past its deadline is retired; one persisted without
//     a delay id is reported, never re-sent;
//   * sign-out drops the memory queue.
//
// Policy and wiring against a fake client: a real homeserver's delayed events
// and the composer's Send-later button are not exercised here.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "models/ScheduledSendController.h"
#include "storage/SecretStore.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

class FakeSecretStore final : public SecretStore
{
public:
    explicit FakeSecretStore(QObject *parent = nullptr)
        : SecretStore(parent)
    {
    }
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

struct RoomSend {
    quint64 op = 0;
    QString roomId;
    QString body;
    QString threadRootId;
    QString replyToEventId;
    QVariantMap spec;
    QString persistedStatusAtSend;
    QStringList mentionIds;
    QString txnId;
};

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    using MatrixClient::sendReply;
    using MatrixClient::sendTextMessage;
    using MatrixClient::sendThreadReplyTo;

    QList<RoomInfo> roomSet;
    ConnectionState state = Syncing;
    // Rust reports starting/retrying as Syncing too; only a running sync is connected.
    bool running = true;
    SettingsManager *settings = nullptr;
    quint64 nextOp = 1;
    bool refuseServerScheduling = false;
    bool roomSendSupported = true;
    int probes = 0;
    QList<RoomSend> roomSends;
    QList<RoomSend> legacySends;        // the timeline lane (mock/HTTP)
    mutable int stateReads = 0;
    struct ScheduleCall {
        QString roomId; QString body; qint64 delayMs; quint64 op; QStringList mentionIds;
    };
    QList<ScheduleCall> scheduleCalls;
    struct UpdateCall { QString delayId; QString action; quint64 op; };
    QList<UpdateCall> updateCalls;
    struct ContentCall {
        QString roomId; QString content; qint64 delayMs; quint64 op; QString txnId;
    };
    QList<ContentCall> contentSchedules;
    QList<ContentCall> contentSends;

    quint64 scheduleContent(const QString &roomId, const QString &content,
                            qint64 delayMs) override
    {
        const quint64 op = nextOp++;
        contentSchedules.append({ roomId, content, delayMs, op });
        return op;
    }
    quint64 sendRoomContent(const QString &roomId, const QString &content,
                            const QString &txnId) override
    {
        const quint64 op = nextOp++;
        contentSends.append({ roomId, content, 0, op, txnId });
        return op;
    }
    bool syncRunning() const override { return connectionState() == Syncing && running; }

    QString persistedStatusFor(const QString &body) const
    {
        if (!settings)
            return {};
        for (const QVariant &v : settings->scheduledSends()) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("body")).toString() == body)
                return m.value(QStringLiteral("status")).toString();
        }
        return QStringLiteral("<absent>");
    }

    void probeDelayedEvents() override { ++probes; }
    quint64 scheduleMessage(const QString &roomId, const QString &body,
                            const QVariantMap &, const QStringList &mentionIds,
                            qint64 delayMs) override
    {
        if (refuseServerScheduling)
            return 0;
        const quint64 op = nextOp++;
        scheduleCalls.append({ roomId, body, delayMs, op, mentionIds });
        return op;
    }
    quint64 updateScheduledMessage(const QString &delayId,
                                   const QString &action) override
    {
        const quint64 op = nextOp++;
        updateCalls.append({ delayId, action, op });
        return op;
    }
    quint64 sendRoomMessage(const QString &roomId, const QString &body,
                            const QVariantMap &spec, const QStringList &mentionIds,
                            const QString &replyToEventId,
                            const QString &threadRootEventId,
                            const QString &txnId) override
    {
        if (!roomSendSupported)
            return 0;
        const quint64 op = nextOp++;
        roomSends.append({ op, roomId, body, threadRootEventId, replyToEventId, spec,
                           persistedStatusFor(body), mentionIds, txnId });
        return op;
    }

    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &, const QVariantMap &spec) override
    {
        legacySends.append({ 0, roomId, body, {}, {}, spec, persistedStatusFor(body) });
    }
    void sendReply(const QString &roomId, const QString &replyToEventId,
                   const QString &body, const QStringList &,
                   const QVariantMap &spec) override
    {
        legacySends.append({ 0, roomId, body, {}, replyToEventId, spec,
                             persistedStatusFor(body) });
    }
    void sendThreadReplyTo(const QString &roomId, const QString &threadRootId,
                           const QString &inReplyTo, const QString &body,
                           const QStringList &, const QVariantMap &spec) override
    {
        legacySends.append({ 0, roomId, body, threadRootId, inReplyTo, spec,
                             persistedStatusFor(body) });
    }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@alice:mock.local"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override
    {
        ++stateReads;
        return state;
    }
    QList<RoomInfo> rooms() const override { return roomSet; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

RoomInfo room(const QString &id, bool encrypted, bool encryptionKnown = true)
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.membership = RoomInfo::Joined;
    info.encrypted = encrypted;
    info.encryptionKnown = encryptionKnown;
    return info;
}

const QString kPlain = QStringLiteral("!plain:mock.local");
const QString kEncrypted = QStringLiteral("!enc:mock.local");
const QString kUnknown = QStringLiteral("!unknown:mock.local");
const QString kAlice = QStringLiteral("@alice:mock.local");
const QString kBob = QStringLiteral("@bob:mock.local");
const QString kAliceLink = QStringLiteral("[Alice](https://matrix.to/#/@alice:mock.local)");
const QString kBobLink = QStringLiteral("[Bob](https://matrix.to/#/@bob:mock.local)");

QVariantMap message(const QString &roomId, const QString &body,
                    const QString &threadRootId = QString(),
                    const QString &replyToEventId = QString())
{
    return QVariantMap{
        { QStringLiteral("roomId"), roomId },
        { QStringLiteral("body"), body },
        { QStringLiteral("threadRootId"), threadRootId },
        { QStringLiteral("replyToEventId"), replyToEventId },
    };
}

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// The Rust sync's states: offline, then starting/retrying (reported as Syncing),
// and only "running" announces the homeserver as reachable.
void goOffline(FakeClient &c)
{
    c.state = MatrixClient::Offline;
    c.running = false;
    Q_EMIT c.connectionStateChanged(MatrixClient::Offline);
}
void goRetrying(FakeClient &c)
{
    c.state = MatrixClient::Syncing;
    c.running = false;
    Q_EMIT c.connectionStateChanged(MatrixClient::Syncing);
}
void goRunning(FakeClient &c)
{
    c.state = MatrixClient::Syncing;
    c.running = true;
    Q_EMIT c.homeserverReachable();
}
// Red proof only: lets these cases compile against a controller without the seam.
template <typename T>
void setRetryPolicy(T &scheduler, qint64 delayMs, int maxFailures)
{
    if constexpr (requires { scheduler.setRetryPolicy(delayMs, maxFailures); })
        scheduler.setRetryPolicy(delayMs, maxFailures);
    else
        QFAIL("no retry policy seam");
}

void flap(FakeClient &c, int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        goRetrying(c);
        QTest::qWait(40);
        goOffline(c);
        QTest::qWait(40);
    }
}

QVariantMap mentioning(const QString &roomId, const QString &body, const QStringList &ids)
{
    QVariantMap m = message(roomId, body);
    m.insert(QStringLiteral("mentionIds"), ids);
    return m;
}

QVariantMap withMedia(QVariantMap m, const QList<quint64> &ops,
                      const QString &caption = QString())
{
    QVariantList media;
    for (quint64 op : ops)
        media.append(QVariantMap{
            { QStringLiteral("op"), op },
            { QStringLiteral("fileName"), QStringLiteral("file-%1.png").arg(op) },
            { QStringLiteral("threadRootId"), m.value(QStringLiteral("threadRootId")) },
            { QStringLiteral("replyToEventId"), m.value(QStringLiteral("replyToEventId")) },
            { QStringLiteral("caption"), media.isEmpty() ? caption : QString() },
        });
    m.insert(QStringLiteral("media"), media);
    return m;
}

const QString kPlainContent = QStringLiteral(
    "{\"msgtype\":\"m.image\",\"body\":\"a.png\",\"url\":\"mxc://hs/a\"}");
const QString kSecretContent = QStringLiteral(
    "{\"msgtype\":\"m.image\",\"body\":\"a.png\",\"file\":{\"key\":\"SECRET\"}}");

struct Harness {
    FakeSecretStore secrets;
    SettingsManager settings;
    FakeClient client;
    ScheduledSendController scheduler;

    Harness()
    {
        settings.setSecretStore(&secrets);
        settings.saveSession(QStringLiteral("https://mock.local"),
                             QStringLiteral("@alice:mock.local"),
                             QStringLiteral("DEVICE"),
                             QStringLiteral("token-fixture"));
        client.settings = &settings;
        client.roomSet = { room(kPlain, false), room(kEncrypted, true),
                           room(kUnknown, false,false) };
        scheduler.setSettings(&settings);
        scheduler.setClient(&client);
    }

    QVariantMap entry(const QString &id) const
    {
        for (const QVariant &v : scheduler.pending())
            if (v.toMap().value(QStringLiteral("id")).toString() == id)
                return v.toMap();
        return {};
    }
    void enableServer()
    {
        Q_EMIT client.delayedEventsSupportReceived(true, true);
    }
    QString serverEntry(const QString &body, const QString &delayId)
    {
        const QString id = scheduler.schedule(message(kPlain, body), nowMs() + 90000);
        Q_EMIT client.scheduledSendFinished(client.scheduleCalls.last().op, kPlain, true,
                                            delayId, QString());
        return id;
    }
    void answerRoomSend(bool ok, const QString &category = QString())
    {
        Q_EMIT client.roomSendFinished(client.roomSends.last().op, kPlain, ok, category);
    }
};

}

class ScheduledSendTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("scheduled-send-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void scheduleRefusesAnEmptyBodyAPastDeadlineAndNoRoom()
    {
        Harness h;
        QVERIFY(h.scheduler.schedule(message(kPlain, QStringLiteral("  ")),
                                     nowMs() + 60000).isEmpty());
        QVERIFY(h.scheduler.schedule(message(QString(), QStringLiteral("hi")),
                                     nowMs() + 60000).isEmpty());
        QVERIFY(h.scheduler.schedule(message(kPlain, QStringLiteral("hi")),
                                     nowMs() - 1).isEmpty());
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void anUnencryptedRoomsEntryPersistsAndAnEncryptedOneStaysInMemory()
    {
        Harness h;
        const QString plainId = h.scheduler.schedule(
            message(kPlain, QStringLiteral("later")), nowMs() + 60000);
        QVERIFY(!plainId.isEmpty());
        QCOMPARE(h.entry(plainId).value(QStringLiteral("mode")).toString(),
                 QStringLiteral("local"));
        QCOMPARE(h.entry(plainId).value(QStringLiteral("volatile")).toBool(), false);
        QCOMPARE(h.settings.scheduledSends().size(), 1);

        const QString encId = h.scheduler.schedule(
            message(kEncrypted, QStringLiteral("secret plan")), nowMs() + 60000);
        QVERIFY(!encId.isEmpty());
        QCOMPARE(h.entry(encId).value(QStringLiteral("volatile")).toBool(), true);
        // Unknown encryption state fails closed, like the draft store.
        const QString unknownId = h.scheduler.schedule(
            message(kUnknown, QStringLiteral("maybe secret")), nowMs() + 60000);
        QCOMPARE(h.entry(unknownId).value(QStringLiteral("volatile")).toBool(), true);

        QCOMPARE(h.scheduler.pendingCount(), 3);
        const QVariantList rows = h.settings.scheduledSends();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("roomId")).toString(),
                 kPlain);
        for (const QVariant &v : rows) {
            const QString body = v.toMap().value(QStringLiteral("body")).toString();
            QVERIFY(!body.contains(QStringLiteral("secret")));
        }
        QCOMPARE(h.scheduler.pendingForRoom(kEncrypted).size(), 1);
    }

    void serverModeNeedsSupportAnUnencryptedRoomAndNoRelation()
    {
        Harness h;
        QVERIFY(!h.scheduler.wouldUseServer(kPlain, {}, {}));
        h.enableServer();
        QCOMPARE(h.scheduler.serverScheduling(), 1);
        QVERIFY(h.scheduler.wouldUseServer(kPlain, {}, {}));
        QVERIFY(!h.scheduler.wouldUseServer(kEncrypted, {}, {}));
        QVERIFY(!h.scheduler.wouldUseServer(kUnknown, {}, {}));
        QVERIFY(!h.scheduler.wouldUseServer(kPlain, QStringLiteral("$root"), {}));
        QVERIFY(!h.scheduler.wouldUseServer(kPlain, {}, QStringLiteral("$reply")));

        const qint64 at = nowMs() + 90000;
        const QString id = h.scheduler.schedule(message(kPlain, QStringLiteral("held")), at);
        QCOMPARE(h.entry(id).value(QStringLiteral("mode")).toString(),
                 QStringLiteral("server"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        QCOMPARE(h.client.scheduleCalls.first().roomId, kPlain);
        QVERIFY(h.client.scheduleCalls.first().delayMs > 80000);
        QVERIFY(h.client.scheduleCalls.first().delayMs <= 90000);

        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.first().op,
                                              kPlain, true,
                                              QStringLiteral("delay-1"), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("delayId")).toString(),
                 QStringLiteral("delay-1"));
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), false);

        h.scheduler.cancel(id);
        QCOMPARE(h.client.updateCalls.size(), 1);
        QCOMPARE(h.client.updateCalls.first().delayId, QStringLiteral("delay-1"));
        QCOMPARE(h.client.updateCalls.first().action, QStringLiteral("cancel"));
        QVERIFY(!h.entry(id).isEmpty());
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.first().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QVERIFY(h.entry(id).isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
        QVERIFY(h.client.roomSends.isEmpty());
        QVERIFY(h.client.legacySends.isEmpty());
    }

    void aServerRefusalForAnEncryptedRoomFallsBackToTheLocalQueueAndSaysSo()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(message(kPlain, QStringLiteral("x")),
                                                nowMs() + 90000);
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.first().op,
                                              kPlain, false, QString(),
                                              QStringLiteral("encrypted_unsupported"));
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("mode")).toString(), QStringLiteral("local"));
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
        QVERIFY(!e.value(QStringLiteral("error")).toString().isEmpty());

        const QString id2 = h.scheduler.schedule(message(kPlain, QStringLiteral("y")),
                                                 nowMs() + 90000);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op,
                                              kPlain, false, QString(),
                                              QStringLiteral("server"));
        QCOMPARE(h.entry(id2).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(h.scheduler.pendingCount(), 1);
    }

    void aRescheduleOfAServerHeldMessageCancelsFirstAndResubmitsOnlyOnSuccess()
    {
        Harness h;
        h.enableServer();
        const QString id = h.serverEntry(QStringLiteral("held"), QStringLiteral("delay-1"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        const qint64 later = nowMs() + 300000;
        h.scheduler.reschedule(id, later);
        QCOMPARE(h.client.updateCalls.size(), 1);
        QCOMPARE(h.client.updateCalls.last().action, QStringLiteral("cancel"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        h.scheduler.updateText(id, QStringLiteral("ignored"), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("held"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QVERIFY(h.client.scheduleCalls.last().delayMs > 290000);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);
        QCOMPARE(h.entry(id).value(QStringLiteral("delayId")).toString(), QString());
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op, kPlain,
                                              true, QStringLiteral("delay-2"), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("delayId")).toString(),
                 QStringLiteral("delay-2"));
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));

        h.scheduler.updateText(id, QStringLiteral("  edited  "), QString());
        QCOMPARE(h.client.updateCalls.size(), 2);
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("held"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-2"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QCOMPARE(h.client.scheduleCalls.size(), 3);
        QCOMPARE(h.client.scheduleCalls.last().body, QStringLiteral("edited"));
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("edited"));
    }

    void aFailedCancelIsReportedAndNeverFollowedByAReplacement()
    {
        Harness h;
        h.enableServer();
        const QString id = h.serverEntry(QStringLiteral("held"), QStringLiteral("delay-1"));
        h.scheduler.reschedule(id, nowMs() + 300000);
        QCOMPARE(h.client.updateCalls.size(), 1);
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), false,
                                                QStringLiteral("not_found"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(e.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("already have been sent")));
        QCOMPARE(e.value(QStringLiteral("busy")).toBool(), false);
        h.scheduler.cancel(id);
        QCOMPARE(h.client.updateCalls.size(), 1);
        QVERIFY(h.entry(id).isEmpty());
    }

    void changesAskedWhileTheScheduleIsInFlightApplyOnceTheDelayIdArrives()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(message(kPlain, QStringLiteral("held")),
                                                nowMs() + 90000);
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        const qint64 later = nowMs() + 300000;
        h.scheduler.reschedule(id, later);
        QCOMPARE(h.client.updateCalls.size(), 0);
        QVERIFY(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong() < later);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.first().op, kPlain,
                                              true, QStringLiteral("delay-1"), QString());
        QCOMPARE(h.client.updateCalls.size(), 1);
        QCOMPARE(h.client.updateCalls.last().delayId, QStringLiteral("delay-1"));
        QCOMPARE(h.client.scheduleCalls.size(), 1);
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-1"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);

        const QString id2 = h.scheduler.schedule(message(kPlain, QStringLiteral("two")),
                                                 nowMs() + 90000);
        h.scheduler.cancel(id2);
        QVERIFY(!h.entry(id2).isEmpty());
        QCOMPARE(h.client.updateCalls.size(), 1);
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op, kPlain,
                                              true, QStringLiteral("delay-9"), QString());
        QCOMPARE(h.client.updateCalls.size(), 2);
        QCOMPARE(h.client.updateCalls.last().delayId, QStringLiteral("delay-9"));
        QCOMPARE(h.client.updateCalls.last().action, QStringLiteral("cancel"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-9"),
                                                QStringLiteral("cancel"), true,
                                                QString());
        QVERIFY(h.entry(id2).isEmpty());
    }

    void aDueLocalEntryIsMarkedSendingBeforeItLeavesAndLeavesOnAcceptance()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("due soon")), nowMs() + 60);
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().roomId, kPlain);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("due soon"));
        QCOMPARE(h.client.roomSends.first().persistedStatusAtSend,
                 QStringLiteral("sending"));
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("sending"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), true);
        h.answerRoomSend(true);
        QVERIFY(h.entry(id).isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
        QTest::qWait(150);
        QCOMPARE(h.client.roomSends.size(), 1);
        QVERIFY(h.client.legacySends.isEmpty());
    }

    void aRefusedRoomSendIsReportedNotPretendedAndCanBeRetried()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("refused")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("forbidden"));
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(e.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("not sent")));
        QCOMPARE(e.value(QStringLiteral("busy")).toBool(), false);
        QTest::qWait(150);
        QCOMPARE(h.client.roomSends.size(), 1);
        h.scheduler.sendNow(id);
        QCOMPARE(h.client.roomSends.size(), 2);
        h.answerRoomSend(true);
        QVERIFY(h.entry(id).isEmpty());
    }

    void threadAndReplyEntriesCarryTheirRelationThroughTheRoomLane()
    {
        Harness h;
        h.scheduler.schedule(message(kPlain, QStringLiteral("in thread"),
                                     QStringLiteral("$root"), QString()),
                             nowMs() + 60);
        h.scheduler.schedule(message(kPlain, QStringLiteral("as reply"), QString(),
                                     QStringLiteral("$target")),
                             nowMs() + 60);
        // One send per room at a time: the second leaves once the first lands.
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(true);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 2, 3000);
        QString threadRoot, replyTarget;
        for (const RoomSend &m : h.client.roomSends) {
            if (m.body == QLatin1String("in thread"))
                threadRoot = m.threadRootId;
            if (m.body == QLatin1String("as reply"))
                replyTarget = m.replyToEventId;
        }
        QCOMPARE(threadRoot, QStringLiteral("$root"));
        QCOMPARE(replyTarget, QStringLiteral("$target"));
        QVERIFY(h.client.legacySends.isEmpty());
    }

    void aBackendWithoutTheRoomLaneFallsBackToTheTimelineSends()
    {
        Harness h;
        h.client.roomSendSupported = false;
        h.scheduler.schedule(message(kPlain, QStringLiteral("legacy"), QString(),
                                     QStringLiteral("$target")),
                             nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.legacySends.size(), 1, 3000);
        QCOMPARE(h.client.legacySends.first().replyToEventId, QStringLiteral("$target"));
        QCOMPARE(h.client.legacySends.first().persistedStatusAtSend,
                 QStringLiteral("sending"));
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void nothingLeavesWhileDisconnectedAndTheReconnectFiresIt()
    {
        Harness h;
        h.client.state = MatrixClient::Connecting;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("after reconnect")), nowMs() + 60);
        QVERIFY(!id.isEmpty());
        QTest::qWait(250);
        QVERIFY(h.client.roomSends.isEmpty());
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));

        h.client.state = MatrixClient::Syncing;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("after reconnect"));
        QVERIFY(h.client.probes >= 1);
    }

    void rowsCaughtMidWayOnTheLastRunAreReportedAndNeverRefired()
    {
        Harness h;
        h.settings.setScheduledSends({
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("crashed") },
                { QStringLiteral("roomId"), kPlain },
                { QStringLiteral("body"), QStringLiteral("did this go?") },
                { QStringLiteral("sendAtMs"), nowMs() - 5000 },
                { QStringLiteral("mode"), QStringLiteral("local") },
                { QStringLiteral("status"), QStringLiteral("sending") },
            },
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("unconfirmed") },
                { QStringLiteral("roomId"), kPlain },
                { QStringLiteral("body"), QStringLiteral("held?") },
                { QStringLiteral("sendAtMs"), nowMs() + 60000 },
                { QStringLiteral("mode"), QStringLiteral("server") },
                { QStringLiteral("status"), QStringLiteral("pending") },
            },
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("done") },
                { QStringLiteral("roomId"), kPlain },
                { QStringLiteral("body"), QStringLiteral("sent by the server") },
                { QStringLiteral("sendAtMs"), nowMs() - 120000 },
                { QStringLiteral("mode"), QStringLiteral("server") },
                { QStringLiteral("status"), QStringLiteral("pending") },
                { QStringLiteral("delayId"), QStringLiteral("delay-old") },
            },
        });
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTest::qWait(150);
        QVERIFY(h.client.roomSends.isEmpty());
        QVERIFY(h.client.scheduleCalls.isEmpty());
        QCOMPARE(h.entry(QStringLiteral("crashed")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(h.entry(QStringLiteral("unconfirmed")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QVERIFY(h.entry(QStringLiteral("unconfirmed")).value(QStringLiteral("error"))
                    .toString().contains(QStringLiteral("before the server confirmed")));
        QVERIFY(h.entry(QStringLiteral("done")).isEmpty());
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QCOMPARE(h.settings.scheduledSends().size(), 2);
        h.scheduler.cancel(QStringLiteral("crashed"));
        QVERIFY(h.entry(QStringLiteral("crashed")).isEmpty());
    }

    void aMissedDeadlineFiresOnceOnTheNextStart()
    {
        Harness h;
        h.settings.setScheduledSends({ QVariantMap{
            { QStringLiteral("id"), QStringLiteral("missed") },
            { QStringLiteral("roomId"), kPlain },
            { QStringLiteral("body"), QStringLiteral("overdue") },
            { QStringLiteral("sendAtMs"), nowMs() - 5000 },
            { QStringLiteral("mode"), QStringLiteral("local") },
            { QStringLiteral("status"), QStringLiteral("pending") },
        } });
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("overdue"));
        h.answerRoomSend(true);
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void localRescheduleAndEditKeepTheEntryPendingAndPersisted()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("draft")), nowMs() + 60000);
        h.scheduler.updateText(id, QStringLiteral("  final  "), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("final"));
        h.scheduler.updateText(id, QStringLiteral("   "), QString());
        QCOMPARE(h.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("final"));
        const qint64 later = nowMs() + 120000;
        h.scheduler.reschedule(id, later);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);
        h.scheduler.reschedule(id, nowMs() - 1);
        QCOMPARE(h.entry(id).value(QStringLiteral("sendAtMs")).toLongLong(), later);
        QCOMPARE(h.settings.scheduledSends().size(), 1);
        QCOMPARE(h.settings.scheduledSends().first().toMap()
                     .value(QStringLiteral("body")).toString(),
                 QStringLiteral("final"));
    }

    void signOutDropsTheMemoryQueue()
    {
        Harness h;
        h.scheduler.schedule(message(kPlain, QStringLiteral("a")), nowMs() + 60000);
        h.scheduler.schedule(message(kEncrypted, QStringLiteral("b")), nowMs() + 60000);
        QCOMPARE(h.scheduler.pendingCount(), 2);
        Q_EMIT h.client.loggedOut();
        QCOMPARE(h.scheduler.pendingCount(), 0);
        QVERIFY(h.scheduler.pending().isEmpty());
        QCOMPARE(h.scheduler.serverScheduling(), -1);
    }

    void anAttachmentIsScheduledOnTheServerOnlyOnceUploaded()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(
            withMedia(message(kPlain, QString()), { 900 }), nowMs() + 90000);
        QVERIFY(!id.isEmpty());
        QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("media")).toString(), QStringLiteral("file-900.png"));
        QVERIFY(e.value(QStringLiteral("uploading")).toBool());
        QVERIFY(e.value(QStringLiteral("busy")).toBool());
        QVERIFY(!e.contains(QStringLiteral("content")));
        QVERIFY(h.client.contentSchedules.isEmpty());
        QVERIFY(h.client.scheduleCalls.isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());

        Q_EMIT h.client.scheduledMediaPrepared(900, kPlain, true, false, kPlainContent);
        QCOMPARE(h.client.contentSchedules.size(), 1);
        QCOMPARE(h.client.contentSchedules.first().content, kPlainContent);
        QVERIFY(h.client.contentSchedules.first().delayMs > 80000);
        QVERIFY(h.client.scheduleCalls.isEmpty());
        Q_EMIT h.client.scheduledSendFinished(h.client.contentSchedules.first().op, kPlain,
                                              true, QStringLiteral("delay-m"), QString());
        e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("mode")).toString(), QStringLiteral("server"));
        QCOMPARE(e.value(QStringLiteral("delayId")).toString(), QStringLiteral("delay-m"));
        QVERIFY(!e.contains(QStringLiteral("content")));
        const QVariantList rows = h.settings.scheduledSends();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("content")).toString(),
                 kPlainContent);

        // A reschedule resubmits the same content.
        h.scheduler.reschedule(id, nowMs() + 200000);
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-m"),
                                                QStringLiteral("cancel"), true, QString());
        QCOMPARE(h.client.contentSchedules.size(), 2);
        QCOMPARE(h.client.contentSchedules.last().content, kPlainContent);
    }

    void anEncryptedRoomsAttachmentStaysInMemoryAndIsSentLocally()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(
            withMedia(message(kEncrypted, QString()), { 901 }), nowMs() + 80);
        QCOMPARE(h.entry(id).value(QStringLiteral("mode")).toString(), QStringLiteral("local"));
        QVERIFY(h.entry(id).value(QStringLiteral("volatile")).toBool());
        // Due while still uploading: it waits for its content.
        QTest::qWait(200);
        QVERIFY(h.client.contentSends.isEmpty());
        Q_EMIT h.client.scheduledMediaPrepared(901, kEncrypted, true, true, kSecretContent);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.contentSends.size(), 1, 3000);
        QCOMPARE(h.client.contentSends.first().roomId, kEncrypted);
        QCOMPARE(h.client.contentSends.first().content, kSecretContent);
        QVERIFY(h.client.contentSchedules.isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
        Q_EMIT h.client.roomSendFinished(h.client.contentSends.first().op, kEncrypted, true,
                                         QString());
        QVERIFY(h.entry(id).isEmpty());
    }

    void contentHoldingFileKeysIsNeverPersistedEvenInARoomThoughtUnencrypted()
    {
        Harness h;
        h.enableServer();
        const QString id = h.scheduler.schedule(
            withMedia(message(kPlain, QString()), { 902 }), nowMs() + 90000);
        Q_EMIT h.client.scheduledMediaPrepared(902, kPlain, true, true, kSecretContent);
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("mode")).toString(), QStringLiteral("local"));
        QVERIFY(e.value(QStringLiteral("volatile")).toBool());
        QVERIFY(h.client.contentSchedules.isEmpty());
        for (const QVariant &v : h.settings.scheduledSends())
            QVERIFY(!v.toMap().value(QStringLiteral("content")).toString().contains(
                QStringLiteral("SECRET")));
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void anUnencryptedLocalAttachmentPersistsAndAMissedOneFiresOnNextStart()
    {
        qint64 at = 0;
        {
            Harness h;
            at = nowMs() + 400;
            const QString id = h.scheduler.schedule(
                withMedia(message(kPlain, QString(), QString(), QStringLiteral("$reply")),
                          { 903 }), at);
            Q_EMIT h.client.scheduledMediaPrepared(903, kPlain, true, false, kPlainContent);
            QCOMPARE(h.entry(id).value(QStringLiteral("mode")).toString(),
                     QStringLiteral("local"));
            QCOMPARE(h.settings.scheduledSends().size(), 1);
            h.scheduler.setClient(nullptr);
        }
        QTest::qWait(500);
        Harness next;
        Q_EMIT next.client.connectionStateChanged(MatrixClient::Syncing);
        QTRY_COMPARE_WITH_TIMEOUT(next.client.contentSends.size(), 1, 3000);
        QCOMPARE(next.client.contentSends.first().content, kPlainContent);
        Q_EMIT next.client.roomSendFinished(next.client.contentSends.first().op, kPlain,
                                            true, QString());
        QVERIFY(next.settings.scheduledSends().isEmpty());
        QTest::qWait(100);
        QCOMPARE(next.client.contentSends.size(), 1);
    }

    void aFailedUploadDropsTheEntryAndACancelledOneIgnoresItsLateResult()
    {
        Harness h;
        const QString failed = h.scheduler.schedule(
            withMedia(message(kPlain, QString()), { 904 }), nowMs() + 60000);
        QVERIFY(!failed.isEmpty());
        QVERIFY(h.entry(failed).value(QStringLiteral("uploading")).toBool());
        QCOMPARE(h.entry(failed).value(QStringLiteral("media")).toString(),
                 QStringLiteral("file-904.png"));
        Q_EMIT h.client.scheduledMediaPrepared(904, kPlain, false, false, QString());
        QVERIFY(h.entry(failed).isEmpty());

        const QString cancelled = h.scheduler.schedule(
            withMedia(message(kPlain, QString()), { 905 }), nowMs() + 60000);
        QVERIFY(!cancelled.isEmpty());
        QVERIFY(h.entry(cancelled).value(QStringLiteral("uploading")).toBool());
        QCOMPARE(h.entry(cancelled).value(QStringLiteral("media")).toString(),
                 QStringLiteral("file-905.png"));
        h.scheduler.cancel(cancelled);
        QVERIFY(h.entry(cancelled).isEmpty());
        Q_EMIT h.client.scheduledMediaPrepared(905, kPlain, true, false, kPlainContent);
        QVERIFY(h.scheduler.pending().isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void severalAttachmentsAndTheTextKeepTrayOrderAndACaptionIsNotSentTwice()
    {
        Harness h;
        const qint64 at = nowMs() + 60000;
        h.scheduler.schedule(withMedia(message(kPlain, QStringLiteral("text")), { 906, 907 }),
                             at);
        QVariantList rows = h.scheduler.pending();
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).toMap().value(QStringLiteral("media")).toString(),
                 QStringLiteral("file-906.png"));
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("media")).toString(),
                 QStringLiteral("file-907.png"));
        QCOMPARE(rows.at(2).toMap().value(QStringLiteral("body")).toString(),
                 QStringLiteral("text"));
        QVERIFY(rows.at(0).toMap().value(QStringLiteral("sendAtMs")).toLongLong()
                < rows.at(1).toMap().value(QStringLiteral("sendAtMs")).toLongLong());
        QVERIFY(rows.at(1).toMap().value(QStringLiteral("sendAtMs")).toLongLong()
                < rows.at(2).toMap().value(QStringLiteral("sendAtMs")).toLongLong());

        h.settings.setScheduledSends({});
        Harness captioned;
        const QString id = captioned.scheduler.schedule(
            withMedia(message(kPlain, QString()), { 908 }, QStringLiteral("look")), at);
        QCOMPARE(captioned.scheduler.pending().size(), 1);
        QCOMPARE(captioned.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("look"));
        // The caption is inside the content; it cannot be edited as text.
        Q_EMIT captioned.client.scheduledMediaPrepared(908, kPlain, true, false,
                                                       kPlainContent);
        captioned.scheduler.updateText(id, QStringLiteral("changed"), QString());
        QCOMPARE(captioned.entry(id).value(QStringLiteral("body")).toString(),
                 QStringLiteral("look"));
    }

    void signOutDropsUploadingAndPreparedAttachments()
    {
        Harness h;
        h.scheduler.schedule(withMedia(message(kEncrypted, QString()), { 909, 910 }),
                             nowMs() + 60000);
        Q_EMIT h.client.scheduledMediaPrepared(909, kEncrypted, true, true, kSecretContent);
        QCOMPARE(h.scheduler.pending().size(), 2);
        Q_EMIT h.client.loggedOut();
        QVERIFY(h.scheduler.pending().isEmpty());
        Q_EMIT h.client.scheduledMediaPrepared(910, kEncrypted, true, true, kSecretContent);
        QVERIFY(h.scheduler.pending().isEmpty());
        QVERIFY(h.client.contentSends.isEmpty());
    }

    void contentPreparedForAnotherRoomIsNeverSent()
    {
        Harness h;
        h.enableServer();
        const QString server = h.scheduler.schedule(
            withMedia(message(kPlain, QString()), { 911 }), nowMs() + 90000);
        Q_EMIT h.client.scheduledMediaPrepared(911, kEncrypted, true, true, kSecretContent);
        QVERIFY(h.entry(server).isEmpty());

        const QString local = h.scheduler.schedule(
            withMedia(message(kEncrypted, QString()), { 912 }), nowMs() + 50);
        Q_EMIT h.client.scheduledMediaPrepared(912, kPlain, true, false, kPlainContent);
        QVERIFY(h.entry(local).isEmpty());
        QTest::qWait(200);
        QVERIFY(h.client.contentSchedules.isEmpty());
        QVERIFY(h.client.contentSends.isEmpty());
        QVERIFY(h.settings.scheduledSends().isEmpty());
    }

    void entriesDueTogetherInOneRoomLeaveOneAtATimeInOrder()
    {
        Harness h;
        h.scheduler.schedule(withMedia(message(kPlain, QStringLiteral("after")), { 913 }),
                             nowMs() + 60);
        Q_EMIT h.client.scheduledMediaPrepared(913, kPlain, true, false, kPlainContent);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.contentSends.size(), 1, 3000);
        QTest::qWait(200);
        QVERIFY(h.client.roomSends.isEmpty());
        Q_EMIT h.client.roomSendFinished(h.client.contentSends.first().op, kPlain, true,
                                         QString());
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("after"));
    }
    void anOverdueLocalEntryDoesNotSpinTheTimerWhileOffline()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("due while offline")), nowMs() + 80);
        QVERIFY(!id.isEmpty());
        h.client.state = MatrixClient::Connecting;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Connecting);
        QTest::qWait(200);
        const int before = h.client.stateReads;
        QTest::qWait(300);
        const int wakeups = h.client.stateReads - before;
        QVERIFY2(wakeups < 5, qPrintable(QStringLiteral("woke %1 times").arg(wakeups)));
        QVERIFY(h.client.roomSends.isEmpty());
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));

        h.client.state = MatrixClient::Syncing;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(h.client.roomSends.size(), 1);
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(h.client.roomSends.size(), 1);
        h.answerRoomSend(false);
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        QTest::qWait(200);
        QCOMPARE(h.client.roomSends.size(), 1);
    }

    void aServerHeldEntryStillRetiresWhileOffline()
    {
        Harness h;
        h.settings.setScheduledSends({ QVariantMap{
            { QStringLiteral("id"), QStringLiteral("held") },
            { QStringLiteral("roomId"), kPlain },
            { QStringLiteral("body"), QStringLiteral("server has it") },
            { QStringLiteral("sendAtMs"),
              nowMs() - ScheduledSendController::kServerRetireGraceMs + 300 },
            { QStringLiteral("mode"), QStringLiteral("server") },
            { QStringLiteral("status"), QStringLiteral("pending") },
            { QStringLiteral("delayId"), QStringLiteral("delay-held") },
        } });
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QVERIFY(!h.entry(QStringLiteral("held")).isEmpty());
        h.client.state = MatrixClient::Offline;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Offline);
        QTRY_VERIFY_WITH_TIMEOUT(h.entry(QStringLiteral("held")).isEmpty(), 3000);
        QVERIFY(h.settings.scheduledSends().isEmpty());
        QVERIFY(h.client.roomSends.isEmpty());
    }

    void overdueEntriesLeaveInDeadlineOrderNotCreationOrder()
    {
        Harness h;
        h.client.state = MatrixClient::Connecting;
        h.scheduler.schedule(message(kPlain, QStringLiteral("made first, due later")),
                             nowMs() + 300);
        h.scheduler.schedule(message(kPlain, QStringLiteral("made second, due earlier")),
                             nowMs() + 100);
        QTest::qWait(450);
        QVERIFY(h.client.roomSends.isEmpty());
        h.client.state = MatrixClient::Syncing;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(h.client.roomSends.size(), 1);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("made second, due earlier"));
        h.answerRoomSend(true);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 2, 3000);
        QCOMPARE(h.client.roomSends.at(1).body, QStringLiteral("made first, due later"));
    }

    void aRescheduledEntrySendsInTheOrderTheListShows()
    {
        Harness h;
        h.client.state = MatrixClient::Connecting;
        const QString a = h.scheduler.schedule(message(kPlain, QStringLiteral("a")),
                                               nowMs() + 60000);
        const QString b = h.scheduler.schedule(message(kPlain, QStringLiteral("b")),
                                               nowMs() + 120000);
        h.scheduler.reschedule(b, nowMs() + 100);
        h.scheduler.reschedule(a, nowMs() + 300);
        const QVariantList rows = h.scheduler.pending();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toMap().value(QStringLiteral("id")).toString(), b);
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("id")).toString(), a);
        QTest::qWait(450);
        h.client.state = MatrixClient::Syncing;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(h.client.roomSends.size(), 1);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("b"));
        h.answerRoomSend(true);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 2, 3000);
        QCOMPARE(h.client.roomSends.at(1).body, QStringLiteral("a"));
    }

    void attachmentsAMillisecondApartLeaveInTrayOrderAfterAnOutage()
    {
        Harness h;
        h.client.state = MatrixClient::Connecting;
        const QString first = QStringLiteral("{\"msgtype\":\"m.image\",\"body\":\"1.png\"}");
        const QString second = QStringLiteral("{\"msgtype\":\"m.image\",\"body\":\"2.png\"}");
        h.scheduler.schedule(withMedia(message(kPlain, QStringLiteral("text")), { 914, 915 }),
                             nowMs() + 100);
        Q_EMIT h.client.scheduledMediaPrepared(915, kPlain, true, false, second);
        Q_EMIT h.client.scheduledMediaPrepared(914, kPlain, true, false, first);
        QTest::qWait(300);
        QVERIFY(h.client.contentSends.isEmpty());
        h.client.state = MatrixClient::Syncing;
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(h.client.contentSends.size(), 1);
        QCOMPARE(h.client.contentSends.at(0).content, first);
        Q_EMIT h.client.roomSendFinished(h.client.contentSends.last().op, kPlain, true,
                                         QString());
        QTRY_COMPARE_WITH_TIMEOUT(h.client.contentSends.size(), 2, 3000);
        QCOMPARE(h.client.contentSends.at(1).content, second);
        QVERIFY(h.client.roomSends.isEmpty());
        Q_EMIT h.client.roomSendFinished(h.client.contentSends.last().op, kPlain, true,
                                         QString());
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("text"));
    }

    void editingLocalTextRecomputesItsMentions()
    {
        Harness h;
        const QString removed = h.scheduler.schedule(
            mentioning(kPlain, kAliceLink + QStringLiteral(" hi"), { kAlice }),
            nowMs() + 60000);
        const QString swapped = h.scheduler.schedule(
            mentioning(kPlain, kAliceLink + QStringLiteral(" there"), { kAlice }),
            nowMs() + 60000);
        const QString everyone = h.scheduler.schedule(
            mentioning(kPlain, QStringLiteral("@room hey"), { QStringLiteral("@room") }),
            nowMs() + 60000);
        h.scheduler.updateText(removed, QStringLiteral("hi"), QString());
        h.scheduler.updateText(swapped, kBobLink + QStringLiteral(" there"), QString());
        h.scheduler.updateText(everyone, QStringLiteral("@room hey all"), QString());

        QHash<QString, QStringList> persisted;
        for (const QVariant &v : h.settings.scheduledSends())
            persisted.insert(v.toMap().value(QStringLiteral("id")).toString(),
                             v.toMap().value(QStringLiteral("mentionIds")).toStringList());
        QCOMPARE(persisted.value(removed), QStringList());
        QCOMPARE(persisted.value(swapped), QStringList{ kBob });
        QCOMPARE(persisted.value(everyone), QStringList{ QStringLiteral("@room") });

        h.scheduler.sendNow(removed);
        QCOMPARE(h.client.roomSends.last().mentionIds, QStringList());
        h.answerRoomSend(true);
        h.scheduler.sendNow(swapped);
        QCOMPARE(h.client.roomSends.last().mentionIds, QStringList{ kBob });
        h.answerRoomSend(true);
        h.scheduler.updateText(everyone, QStringLiteral("hey all"), QString());
        h.scheduler.sendNow(everyone);
        QCOMPARE(h.client.roomSends.last().mentionIds, QStringList());
    }

    void editingServerHeldTextResubmitsWithTheNewMentions()
    {
        Harness h;
        h.enableServer();
        const QString held = h.scheduler.schedule(
            mentioning(kPlain, kAliceLink + QStringLiteral(" hi"), { kAlice }),
            nowMs() + 90000);
        QCOMPARE(h.client.scheduleCalls.last().mentionIds, QStringList{ kAlice });
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op, kPlain, true,
                                              QStringLiteral("delay-m1"), QString());
        h.scheduler.updateText(held, kBobLink + QStringLiteral(" hi"), QString());
        QCOMPARE(h.client.updateCalls.last().action, QStringLiteral("cancel"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-m1"),
                                                QStringLiteral("cancel"), true, QString());
        QCOMPARE(h.client.scheduleCalls.size(), 2);
        QCOMPARE(h.client.scheduleCalls.last().body, kBobLink + QStringLiteral(" hi"));
        QCOMPARE(h.client.scheduleCalls.last().mentionIds, QStringList{ kBob });

        const QString flying = h.scheduler.schedule(
            mentioning(kPlain, kAliceLink + QStringLiteral(" again"), { kAlice }),
            nowMs() + 90000);
        h.scheduler.updateText(flying, QStringLiteral("again"), QString());
        Q_EMIT h.client.scheduledSendFinished(h.client.scheduleCalls.last().op, kPlain, true,
                                              QStringLiteral("delay-m2"), QString());
        QCOMPARE(h.client.updateCalls.last().delayId, QStringLiteral("delay-m2"));
        Q_EMIT h.client.scheduledUpdateFinished(h.client.updateCalls.last().op,
                                                QStringLiteral("delay-m2"),
                                                QStringLiteral("cancel"), true, QString());
        QCOMPARE(h.client.scheduleCalls.last().body, QStringLiteral("again"));
        QCOMPARE(h.client.scheduleCalls.last().mentionIds, QStringList());
    }
    void aFlappingOutageDispatchesNothingUntilTheSyncRuns()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("due in an outage")), nowMs() + 100);
        QVERIFY(!id.isEmpty());
        goOffline(h.client);
        flap(h.client, 6);
        goRetrying(h.client);
        const int before = h.client.stateReads;
        QTest::qWait(300);
        const int wakeups = h.client.stateReads - before;
        QVERIFY2(wakeups < 5, qPrintable(QStringLiteral("woke %1 times").arg(wakeups)));
        QVERIFY(h.client.roomSends.isEmpty());
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));

        goRunning(h.client);
        QCOMPARE(h.client.roomSends.size(), 1);
        goRunning(h.client);
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTest::qWait(100);
        QCOMPARE(h.client.roomSends.size(), 1);
    }

    void aTransientFailureStaysPendingAndRetriesOnceWithTheSameTransaction_data()
    {
        QTest::addColumn<QString>("category");
        QTest::newRow("network") << QStringLiteral("network");
        QTest::newRow("rate_limited") << QStringLiteral("rate_limited");
    }

    void aTransientFailureStaysPendingAndRetriesOnceWithTheSameTransaction()
    {
        QFETCH(QString, category);
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("flaky")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, category);
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));
        QCOMPARE(h.entry(id).value(QStringLiteral("busy")).toBool(), false);
        QCOMPARE(h.client.persistedStatusFor(QStringLiteral("flaky")),
                 QStringLiteral("pending"));
        QTest::qWait(300);
        QCOMPARE(h.client.roomSends.size(), 1);

        goOffline(h.client);
        flap(h.client, 2);
        goRetrying(h.client);
        QCOMPARE(h.client.roomSends.size(), 1);
        goRunning(h.client);
        QCOMPARE(h.client.roomSends.size(), 2);
        QVERIFY(!h.client.roomSends.at(0).txnId.isEmpty());
        QCOMPARE(h.client.roomSends.at(1).txnId, h.client.roomSends.at(0).txnId);
        goRunning(h.client);
        QCOMPARE(h.client.roomSends.size(), 2);
        h.answerRoomSend(true);
        QVERIFY(h.entry(id).isEmpty());
        QTest::qWait(150);
        QCOMPARE(h.client.roomSends.size(), 2);
    }

    void aDefinitiveRefusalStillFailsAndIsNotRetriedOnReconnect()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("not allowed")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("forbidden"));
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
        goOffline(h.client);
        goRetrying(h.client);
        goRunning(h.client);
        Q_EMIT h.client.connectionStateChanged(MatrixClient::Syncing);
        QTest::qWait(150);
        QCOMPARE(h.client.roomSends.size(), 1);
        QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));
    }

    void overdueEntriesLeaveInDeadlineOrderThroughAFlappingOutage()
    {
        Harness h;
        goOffline(h.client);
        h.scheduler.schedule(message(kPlain, QStringLiteral("made first, due later")),
                             nowMs() + 250);
        h.scheduler.schedule(message(kPlain, QStringLiteral("made second, due earlier")),
                             nowMs() + 100);
        flap(h.client, 5);
        goRetrying(h.client);
        QTest::qWait(50);
        QVERIFY(h.client.roomSends.isEmpty());
        goRunning(h.client);
        QCOMPARE(h.client.roomSends.size(), 1);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("made second, due earlier"));
        h.answerRoomSend(true);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 2, 3000);
        QCOMPARE(h.client.roomSends.at(1).body, QStringLiteral("made first, due later"));
    }
    void anEditAfterANetworkFailureSendsUnderANewTransaction()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("first words")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("network"));
        h.scheduler.updateText(id, QStringLiteral("second words"), QString());
        goOffline(h.client);
        goRunning(h.client);
        QCOMPARE(h.client.roomSends.size(), 2);
        QCOMPARE(h.client.roomSends.at(1).body, QStringLiteral("second words"));
        QVERIFY(!h.client.roomSends.at(1).txnId.isEmpty());
        QVERIFY(h.client.roomSends.at(1).txnId != h.client.roomSends.at(0).txnId);
    }

    void anUnknownRefusalFailsHonestlyAndIsNotRetried()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("too big")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("unknown"));
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(e.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("Check the room")));
        goOffline(h.client);
        goRunning(h.client);
        QTest::qWait(100);
        QCOMPARE(h.client.roomSends.size(), 1);
    }

    void theRetryStateAndTransactionSurviveARestart()
    {
        Harness h;
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("kept")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("network"));
        const QVariantMap row = h.settings.scheduledSends().first().toMap();
        QCOMPARE(row.value(QStringLiteral("txnId")).toString(),
                 h.client.roomSends.first().txnId);
        QCOMPARE(row.value(QStringLiteral("transientFailures")).toInt(), 1);
        QVERIFY(row.value(QStringLiteral("retryAtMs")).toLongLong() > nowMs());

        Harness next;
        next.settings.setScheduledSends({ QVariantMap{
            { QStringLiteral("id"), QStringLiteral("restarted") },
            { QStringLiteral("roomId"), kPlain },
            { QStringLiteral("body"), QStringLiteral("four tries in") },
            { QStringLiteral("sendAtMs"), nowMs() - 5000 },
            { QStringLiteral("mode"), QStringLiteral("local") },
            { QStringLiteral("status"), QStringLiteral("pending") },
            { QStringLiteral("txnId"), QStringLiteral("txn-kept") },
            { QStringLiteral("transientFailures"), 4 },
        } });
        Q_EMIT next.client.connectionStateChanged(MatrixClient::Syncing);
        QTRY_COMPARE_WITH_TIMEOUT(next.client.roomSends.size(), 1, 3000);
        QCOMPARE(next.client.roomSends.first().txnId, QStringLiteral("txn-kept"));
        next.answerRoomSend(false, QStringLiteral("network"));
        QCOMPARE(next.entry(QStringLiteral("restarted")).value(QStringLiteral("status"))
                     .toString(), QStringLiteral("failed"));
        Q_UNUSED(id);
    }

    void aRoomWaitingToRetryHoldsItsLaterEntries()
    {
        Harness h;
        h.scheduler.schedule(message(kPlain, QStringLiteral("earlier")), nowMs() + 60);
        h.scheduler.schedule(message(kPlain, QStringLiteral("later")), nowMs() + 250);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        QCOMPARE(h.client.roomSends.first().body, QStringLiteral("earlier"));
        h.answerRoomSend(false, QStringLiteral("network"));
        QTest::qWait(450);
        QCOMPARE(h.client.roomSends.size(), 1);
        const int before = h.client.stateReads;
        QTest::qWait(300);
        QVERIFY2(h.client.stateReads - before < 5,
                 qPrintable(QStringLiteral("woke %1 times").arg(h.client.stateReads - before)));
        goOffline(h.client);
        goRunning(h.client);
        QCOMPARE(h.client.roomSends.size(), 2);
        QCOMPARE(h.client.roomSends.at(1).body, QStringLiteral("earlier"));
        h.answerRoomSend(true);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 3, 3000);
        QCOMPARE(h.client.roomSends.at(2).body, QStringLiteral("later"));
    }

    void theRetryTimerFiresWithoutAReconnect()
    {
        Harness h;
        setRetryPolicy(h.scheduler, 150, 5);
        h.scheduler.schedule(message(kPlain, QStringLiteral("again")), nowMs() + 60);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 1, 3000);
        h.answerRoomSend(false, QStringLiteral("rate_limited"));
        QTest::qWait(50);
        QCOMPARE(h.client.roomSends.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 2, 3000);
        QCOMPARE(h.client.roomSends.at(1).txnId, h.client.roomSends.at(0).txnId);
    }

    void theLastAllowedTransientFailureTurnsFailed()
    {
        Harness h;
        setRetryPolicy(h.scheduler, 50, 5);
        const QString id = h.scheduler.schedule(
            message(kPlain, QStringLiteral("unlucky")), nowMs() + 60);
        for (int attempt = 1; attempt <= 4; ++attempt) {
            QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), attempt, 3000);
            h.answerRoomSend(false, QStringLiteral("network"));
            QCOMPARE(h.entry(id).value(QStringLiteral("status")).toString(),
                     QStringLiteral("pending"));
        }
        QTRY_COMPARE_WITH_TIMEOUT(h.client.roomSends.size(), 5, 3000);
        h.answerRoomSend(false, QStringLiteral("network"));
        const QVariantMap e = h.entry(id);
        QCOMPARE(e.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(e.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("Check the room")));
        QTest::qWait(250);
        QCOMPARE(h.client.roomSends.size(), 5);
    }
};

QTEST_GUILESS_MAIN(ScheduledSendTest)
#include "ScheduledSendTest.moc"
