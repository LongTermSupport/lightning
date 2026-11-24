#include <QtTest>
#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

namespace {
class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return user; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return fakeRooms; }
    QList<TimelineEvent> timeline(const QString &) const override
    { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override { ++messageSends; }
    void sendReply(const QString &, const QString &,
                   const QString &) override {}
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    quint64 spaceCommunityAction(const QString &room, const QString &action,
                                 const QVariantMap &payload)
    {
        lastRoom = room;
        lastAction = action;
        lastPayload = payload;
        ++calls;
        return ++counter;
    }
    QList<RoomInfo> fakeRooms;
    QString user = QStringLiteral("@me:example.org");
    QString lastRoom;
    QString lastAction;
    QVariantMap lastPayload;
    quint64 counter = 0;
    int calls = 0;
    int messageSends = 0;
};

const QString space = QStringLiteral("!space:example.org");

bool load(SpaceManager &manager)
{
    return QMetaObject::invokeMethod(&manager, "loadCommunity", Q_ARG(QString, space));
}

bool action(SpaceManager &manager, const char *verb, const QVariantMap &data = {})
{
    return QMetaObject::invokeMethod(&manager, "communityAction",
        Q_ARG(QString, QString::fromLatin1(verb)), Q_ARG(QVariantMap, data));
}

bool finish(FakeClient &client, bool ok, const QVariantMap &data)
{
    return QMetaObject::invokeMethod(&client, "spaceCommunityFinished",
        Q_ARG(quint64, client.counter), Q_ARG(QString, client.lastRoom),
        Q_ARG(QString, client.lastAction), Q_ARG(bool, ok), Q_ARG(QVariantMap, data));
}

QVariantMap state(const SpaceManager &manager)
{
    return manager.property("communityState").toMap();
}

QVariantMap guideState()
{
    return {{QStringLiteral("guide"), QVariantMap{
        {QStringLiteral("welcome"), QStringLiteral("Welcome")},
        {QStringLiteral("rules"), QStringLiteral("Be kind")},
        {QStringLiteral("rooms"), QVariantList{QStringLiteral("!two:example.org"),
            QStringLiteral("!hidden:example.org"), QStringLiteral("!one:example.org")}}}},
        {QStringLiteral("can_edit_guide"), true}};
}
}

class SpaceCommunityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void guideRecommendationsOnlyUseVisibleLobbyRowsInGuideOrder()
    {
        FakeClient client;
        SpaceManager manager;
        manager.setClient(&client);
        QVERIFY(load(manager));
        QVERIFY(finish(client, true, guideState()));
        QVariantList sections{QVariantMap{{QStringLiteral("rows"), QVariantList{
            QVariantMap{{QStringLiteral("roomId"), QStringLiteral("!one:example.org")},
                        {QStringLiteral("joined"), true}},
            QVariantMap{{QStringLiteral("roomId"), QStringLiteral("!two:example.org")},
                        {QStringLiteral("joined"), false}, {QStringLiteral("joinRule"), QStringLiteral("restricted")}}
        }}}};
        QVariantList result;
        QVERIFY(QMetaObject::invokeMethod(&manager, "guideRooms", Q_RETURN_ARG(QVariantList, result),
                                         Q_ARG(QVariantList, sections)));
        QCOMPARE(result.size(), 2);
        QCOMPARE(result[0].toMap().value("roomId").toString(), QStringLiteral("!two:example.org"));
        QCOMPARE(result[1].toMap().value("roomId").toString(), QStringLiteral("!one:example.org"));
        QCOMPARE(client.calls, 1);
        QCOMPARE(client.messageSends, 0);
    }

    void refusedGuideWriteRetainsTheLastServerState()
    {
        FakeClient client;
        SpaceManager manager;
        manager.setClient(&client);
        QVERIFY(load(manager));
        QVERIFY(finish(client, true, guideState()));
        QVERIFY(action(manager, "guide", {{QStringLiteral("welcome"), QStringLiteral("Changed")}}));
        QCOMPARE(state(manager), guideState());
        QVERIFY(finish(client, false, {{QStringLiteral("category"), QStringLiteral("forbidden")}}));
        QCOMPARE(state(manager), guideState());
        QVERIFY(!manager.property("communityBusy").toBool());
        QVERIFY(!manager.property("communityMessage").toString().isEmpty());
    }

    void rolePreviewUsesTheActualThresholds()
    {
        SpaceManager manager;
        const QVariantMap powers{{QStringLiteral("kick"), 40}, {QStringLiteral("ban"), 60},
            {QStringLiteral("m.room.power_levels"), 100}, {QStringLiteral("events_default"), 0}};
        QVariantList result;
        QVERIFY(QMetaObject::invokeMethod(&manager, "roleCapabilityPreview",
            Q_RETURN_ARG(QVariantList, result), Q_ARG(QVariantMap, powers),
            Q_ARG(qlonglong, 0), Q_ARG(qlonglong, 50)));
        QCOMPARE(result.size(), 4);
        QHash<QString, QVariantMap> byKey;
        for (const QVariant &r : result) byKey.insert(r.toMap().value("key").toString(), r.toMap());
        QVERIFY(byKey["kick"].value("changed").toBool());
        QVERIFY(byKey["kick"].value("after").toBool());
        QVERIFY(!byKey["ban"].value("after").toBool());
        QVERIFY(!byKey["m.room.power_levels"].value("after").toBool());
        QVERIFY(byKey["events_default"].value("before").toBool());
    }

    void audiencePreview_data()
    {
        QTest::addColumn<QString>("rule");
        QTest::addColumn<QString>("history");
        QTest::addColumn<QString>("joining");
        QTest::addColumn<QString>("reading");
        QTest::newRow("public-world-readable") << "public" << "world_readable" << "Anyone who is not banned" << "including people outside";
        QTest::newRow("nested-restricted-joined") << "restricted" << "joined" << "!parent:example.org" << "when they joined";
        QTest::newRow("nested-knock-restricted-shared") << "knock_restricted" << "shared" << "Other people can ask" << "before they joined";
        QTest::newRow("invite-invited") << "invite" << "invited" << "Only invited" << "when they were invited";
        QTest::newRow("knock-shared") << "knock" << "shared" << "must admit" << "before they joined";
        QTest::newRow("unknown") << "" << "" << "not known" << "not known";
    }

    void audiencePreview()
    {
        QFETCH(QString, rule);
        QFETCH(QString, history);
        QFETCH(QString, joining);
        QFETCH(QString, reading);
        SpaceManager manager;
        QString result;
        const QStringList allowed{QStringLiteral("!parent:example.org")};
        QVERIFY(QMetaObject::invokeMethod(&manager, "audiencePreview", Q_RETURN_ARG(QString, result),
            Q_ARG(QString, rule), Q_ARG(QString, history), Q_ARG(QStringList, allowed)));
        QVERIFY2(result.contains(joining), qPrintable(result));
        QVERIFY2(result.contains(reading), qPrintable(result));
        QVERIFY(result.contains(QStringLiteral("Child rooms keep their own rules")));
    }

    void publishingRequiresAReviewAndEditsInvalidateApproval()
    {
        FakeClient client;
        SpaceManager manager;
        manager.setClient(&client);
        QVERIFY(load(manager));
        QVERIFY(finish(client, true, guideState()));
        QVERIFY(action(manager, "publish"));
        QCOMPARE(client.calls, 1);
        const QVariantMap request{{QStringLiteral("id"), client.user}, {QStringLiteral("body"), QStringLiteral("Final text")}};
        QVERIFY(action(manager, "preview", request));
        QVERIFY(finish(client, true, {{QStringLiteral("approval"), QStringLiteral("review-token")},
                                     {QStringLiteral("body"), QStringLiteral("Final text")}}));
        QVERIFY(!manager.property("communityReview").toMap().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(&manager, "invalidateCommunityReview"));
        QVERIFY(action(manager, "publish"));
        QCOMPARE(client.calls, 2);
        QVERIFY(action(manager, "preview", request));
        QVERIFY(finish(client, true, {{QStringLiteral("approval"), QStringLiteral("review-token")}}));
        QVERIFY(action(manager, "publish", {{QStringLiteral("body"), QStringLiteral("Unreviewed")}}));
        QCOMPARE(client.lastPayload.value("body").toString(), QStringLiteral("Final text"));
        QCOMPARE(client.lastPayload.value("approval").toString(), QStringLiteral("review-token"));
        const QVariantMap sent = client.lastPayload;
        QVERIFY(finish(client, false, {{QStringLiteral("category"), QStringLiteral("network")}}));
        QVERIFY(action(manager, "publish"));
        QCOMPARE(client.lastPayload, sent);
    }

    void staleApprovalAndAccountSwitchDropReviewState()
    {
        FakeClient client;
        SpaceManager manager;
        manager.setClient(&client);
        QVERIFY(load(manager));
        QVERIFY(finish(client, true, guideState()));
        const QVariantMap request{{QStringLiteral("id"), client.user}, {QStringLiteral("body"), QStringLiteral("Text")}};
        QVERIFY(action(manager, "preview", request));
        QVERIFY(finish(client, true, {{QStringLiteral("approval"), QStringLiteral("token")}}));
        QVERIFY(action(manager, "publish"));
        QVERIFY(finish(client, false, {{QStringLiteral("category"), QStringLiteral("stale")}}));
        QVERIFY(manager.property("communityReview").toMap().isEmpty());
        QVERIFY(action(manager, "preview", request));
        const int calls = client.calls;
        client.user = QStringLiteral("@other:example.org");
        QVERIFY(finish(client, true, {{QStringLiteral("approval"), QStringLiteral("old-account")}}));
        QVERIFY(manager.property("communityReview").toMap().isEmpty());
        manager.abandonPendingChildOps();
        QVERIFY(state(manager).isEmpty());
        QVERIFY(!manager.property("communityBusy").toBool());
        QVERIFY(action(manager, "publish"));
        QCOMPARE(client.calls, calls);
    }

    void lifecycleReviewKeepsReplacementLinksAndNeverMutatesRooms()
    {
        FakeClient client;
        RoomInfo parent;
        parent.id = space;
        parent.isSpace = true;
        parent.childRoomIds = {QStringLiteral("!child:example.org")};
        RoomInfo child;
        child.id = parent.childRoomIds.first();
        child.joinedMemberCount = 7;
        child.successorRoomId = QStringLiteral("!new:example.org");
        child.predecessorRoomId = QStringLiteral("!old:example.org");
        child.lastActivity = QDateTime::fromMSecsSinceEpoch(1000);
        client.fakeRooms = {parent, child};
        SpaceManager manager;
        manager.setClient(&client);
        QVariantMap result;
        QVERIFY(QMetaObject::invokeMethod(&manager, "lifecycleReview", Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, child.id)));
        QCOMPARE(result.value("successor").toString(), child.successorRoomId);
        QCOMPARE(result.value("predecessor").toString(), child.predecessorRoomId);
        QCOMPARE(result.value("joinedMembers").toInt(), 7);
        QCOMPARE(result.value("scope").toStringList(), QStringList{child.id});
        QCOMPARE(result.value("parents").toStringList(), QStringList{space});
        QVERIFY(result.value("activityKnown").toBool());
        QVERIFY(QMetaObject::invokeMethod(&manager, "lifecycleReview", Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, space)));
        QCOMPARE(result.value("scope").toStringList(), (QStringList{space, child.id}));
        QVERIFY(!result.value("activityKnown").toBool());
        QCOMPARE(client.calls, 0);
        QCOMPARE(client.messageSends, 0);
        QCOMPARE(client.fakeRooms[1].successorRoomId, child.successorRoomId);
    }
};

QTEST_GUILESS_MAIN(SpaceCommunityTest)
#include "SpaceCommunityTest.moc"
