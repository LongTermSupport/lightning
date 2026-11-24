#include "app/RemindersController.h"
#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class ReminderFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    QStringList requested;
    quint64 lastOp = 0;
    quint64 requestEventSource(const QString &, const QString &eventId) override
    {
        requested << eventId;
        return ++lastOp;
    }
    void answer(const QString &eventId, quint64 op, const QString &json)
    {
        Q_EMIT eventSourceReceived(QStringLiteral("!room:example.org"), eventId,
                                   true, json,
                                   QVariantMap{ { QStringLiteral("opId"), op } });
    }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override {}
    bool restoreSession() override { return false; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    RoomInfo roomInfo(const QString &roomId) const override
    { RoomInfo r; r.id = roomId; r.name = QStringLiteral("Planning"); return r; }
    QString displayNameFor(const QString &, const QString &) const override
    { return QStringLiteral("Bob"); }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
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

namespace {
const QString kRoom = QStringLiteral("!room:example.org");
constexpr qint64 kNow = 1'800'000'000'000;

QString message(const QString &body)
{
    return QStringLiteral(
        "{\"type\":\"m.room.message\",\"sender\":\"@bob:example.org\","
        "\"content\":{\"msgtype\":\"m.text\",\"body\":\"%1\"}}").arg(body);
}
}

class RemindersControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void aReminderSurvivesRestartAndStoresIdsAndTimesOnly()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("acct/reminders.json"));
        {
            RemindersController reminders;
            reminders.setNowForTest(kNow);
            reminders.openFor(path);
            reminders.remind(kRoom, QStringLiteral("$e"), QStringLiteral("$root"),
                             kNow + 60'000);
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonArray list = root.value(QStringLiteral("reminders")).toArray();
        QCOMPARE(list.size(), 1);
        QStringList keys = list.at(0).toObject().keys();
        keys.sort();
        QCOMPARE(keys, (QStringList{ QStringLiteral("due"), QStringLiteral("event"),
                                     QStringLiteral("room"), QStringLiteral("root") }));

        RemindersController again;
        again.setNowForTest(kNow);
        again.openFor(path);
        QCOMPARE(again.reminderDue(kRoom, QStringLiteral("$e")), kNow + 60'000);
    }

    void aReminderFiresOnceAndOpensTheExactMessage()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("reminders.json"));
        ReminderFakeClient client;
        RemindersController reminders;
        reminders.setClient(&client);
        reminders.setPreviewPolicy([](const QString &) {
            return int(RemindersController::SenderAndMessage);
        });
        reminders.setNowForTest(kNow);
        reminders.openFor(path);
        reminders.remind(kRoom, QStringLiteral("$e"), QStringLiteral("$root"),
                         kNow + 1000);
        QSignalSpy notified(&reminders, &RemindersController::notify);
        reminders.runDueForTest();
        QVERIFY(client.requested.isEmpty());

        reminders.setNowForTest(kNow + 1000);
        reminders.runDueForTest();
        QCOMPARE(client.requested, QStringList{ QStringLiteral("$e") });
        client.answer(QStringLiteral("$e"), client.lastOp, message(QStringLiteral("ship it")));
        QCOMPARE(notified.count(), 1);
        const QList<QVariant> card = notified.takeFirst();
        QVERIFY(card.at(1).toString().contains(QStringLiteral("ship it")));
        QCOMPARE(card.at(2).toString(), kRoom);
        QCOMPARE(card.at(3).toString(), QStringLiteral("$e"));
        QCOMPARE(card.at(4).toString(), QStringLiteral("$root"));

        reminders.runDueForTest();
        QCOMPARE(client.requested.size(), 1);
        QVERIFY(!QFile::exists(path));
        RemindersController again;
        again.setNowForTest(kNow + 2000);
        again.openFor(path);
        QCOMPARE(again.reminderDue(kRoom, QStringLiteral("$e")), 0);
    }

    void aRemovedMessageFiresWithoutContent()
    {
        QTemporaryDir dir;
        ReminderFakeClient client;
        RemindersController reminders;
        reminders.setClient(&client);
        reminders.setPreviewPolicy([](const QString &) {
            return int(RemindersController::SenderAndMessage);
        });
        reminders.setNowForTest(kNow);
        reminders.openFor(dir.filePath(QStringLiteral("r.json")));
        reminders.remind(kRoom, QStringLiteral("$gone"), {}, kNow + 10);
        QSignalSpy notified(&reminders, &RemindersController::notify);
        reminders.setNowForTest(kNow + 10);
        reminders.runDueForTest();
        client.answer(QStringLiteral("$gone"), client.lastOp, QStringLiteral(
            "{\"type\":\"m.room.message\",\"sender\":\"@bob:example.org\","
            "\"content\":{},\"unsigned\":{\"redacted_because\":{}}}"));
        QCOMPARE(notified.count(), 1);
        const QList<QVariant> card = notified.takeFirst();
        QVERIFY(card.at(1).toString().contains(QStringLiteral("removed")));
        QVERIFY(!card.at(1).toString().contains(QStringLiteral("Bob")));
        QCOMPARE(card.at(3).toString(), QString());
    }

    void aPrivatePreviewNamesNeitherSenderNorText()
    {
        QTemporaryDir dir;
        ReminderFakeClient client;
        RemindersController reminders;
        reminders.setClient(&client);
        reminders.setPreviewPolicy([](const QString &) {
            return int(RemindersController::Private);
        });
        reminders.setNowForTest(kNow);
        reminders.openFor(dir.filePath(QStringLiteral("r.json")));
        reminders.remind(kRoom, QStringLiteral("$e"), {}, kNow + 10);
        QSignalSpy notified(&reminders, &RemindersController::notify);
        reminders.setNowForTest(kNow + 10);
        reminders.runDueForTest();
        client.answer(QStringLiteral("$e"), client.lastOp, message(QStringLiteral("secret")));
        QCOMPARE(notified.count(), 1);
        const QList<QVariant> card = notified.takeFirst();
        for (const QVariant &field : card.mid(0, 2)) {
            QVERIFY(!field.toString().contains(QStringLiteral("secret")));
            QVERIFY(!field.toString().contains(QStringLiteral("Bob")));
            QVERIFY(!field.toString().contains(QStringLiteral("Planning")));
        }
    }

    void remindersDueWhileClosedFireOnceAsOneSummary()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("r.json"));
        {
            RemindersController reminders;
            reminders.setNowForTest(kNow);
            reminders.openFor(path);
            for (int i = 0; i < 3; ++i)
                reminders.remind(kRoom, QStringLiteral("$e%1").arg(i), {},
                                 kNow + 100 + i);
        }
        ReminderFakeClient client;
        RemindersController reminders;
        reminders.setClient(&client);
        QSignalSpy notified(&reminders, &RemindersController::notify);
        reminders.setNowForTest(kNow + 10'000);
        reminders.openFor(path);
        QTRY_COMPARE(notified.count(), 1);
        QVERIFY(notified.at(0).at(1).toString().contains(QLatin1Char('3')));
        QVERIFY(client.requested.isEmpty());
        QTest::qWait(20);
        QCOMPARE(notified.count(), 1);

        RemindersController again;
        QSignalSpy againNotified(&again, &RemindersController::notify);
        again.setNowForTest(kNow + 20'000);
        again.openFor(path);
        QTest::qWait(20);
        QCOMPARE(againNotified.count(), 0);
    }

    void aSnoozeOutlivesARestartAndEndsOnTime()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("r.json"));
        {
            RemindersController reminders;
            reminders.setNowForTest(kNow);
            reminders.openFor(path);
            reminders.snooze(kRoom, kNow + 5000);
            QVERIFY(reminders.isSnoozed(kRoom));
            QCOMPARE(reminders.snoozed().size(), 1);
        }
        RemindersController reminders;
        QSignalSpy ended(&reminders, &RemindersController::snoozeEnded);
        reminders.setNowForTest(kNow + 1000);
        reminders.openFor(path);
        QVERIFY(reminders.isSnoozed(kRoom));
        reminders.setNowForTest(kNow + 5000);
        QVERIFY(!reminders.isSnoozed(kRoom));
        reminders.runDueForTest();
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(0).toString(), kRoom);
        QVERIFY(!QFile::exists(path));
    }

    void unsnoozingEndsTheSnoozeAtOnce()
    {
        QTemporaryDir dir;
        RemindersController reminders;
        reminders.setNowForTest(kNow);
        reminders.openFor(dir.filePath(QStringLiteral("r.json")));
        reminders.snooze(kRoom, kNow + 5000);
        QSignalSpy ended(&reminders, &RemindersController::snoozeEnded);
        reminders.unsnooze(kRoom);
        QCOMPARE(ended.count(), 1);
        QVERIFY(!reminders.isSnoozed(kRoom));
    }

    void aClosedAccountKeepsNothingAndWritesNothing()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("r.json"));
        RemindersController reminders;
        reminders.setNowForTest(kNow);
        reminders.openFor(path);
        reminders.snooze(kRoom, kNow + 5000);
        QVERIFY(QFile::exists(path));
        reminders.close();
        QVERIFY(QFile::remove(path));
        QVERIFY(!reminders.isSnoozed(kRoom));
        reminders.remind(kRoom, QStringLiteral("$e"), {}, kNow + 5000);
        reminders.snooze(kRoom, kNow + 5000);
        QVERIFY(!QFile::exists(path));
        QCOMPARE(reminders.reminderDue(kRoom, QStringLiteral("$e")), 0);
    }
};

QTEST_GUILESS_MAIN(RemindersControllerTest)
#include "RemindersControllerTest.moc"
