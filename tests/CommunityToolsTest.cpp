#include <QtTest>
#include <QDateTime>
#include <QTemporaryDir>
#include <QTimeZone>

#include "app/ModerationQueueController.h"
#include "app/RemindersController.h"
#include "app/RoomCardsController.h"
#include "matrix/MatrixClient.h"
#include "threads/ForumView.h"

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
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
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

    quint64 spaceCommunityAction(const QString &room, const QString &action,
                                 const QVariantMap &payload) override
    {
        lastRoom = room;
        lastAction = action;
        lastPayload = payload;
        actions.append(action);
        return ++counter;
    }
    void finish(bool ok, const QVariantMap &result)
    {
        Q_EMIT spaceCommunityFinished(counter, lastRoom, lastAction, ok, result);
    }
    QString lastRoom;
    QString lastAction;
    QVariantMap lastPayload;
    QStringList actions;
    quint64 counter = 0;
};

const QString room = QStringLiteral("!room:example.org");
const QString root = QStringLiteral("$event");

QVariantMap threadRow(const QString &id, const QString &preview, int replies, qint64 rootMs,
                   bool unread = false)
{
    return {{QStringLiteral("rootEventId"), id}, {QStringLiteral("rootPreview"), preview},
            {QStringLiteral("replyCount"), replies}, {QStringLiteral("unread"), unread},
            {QStringLiteral("rootTimestamp"), QDateTime::fromMSecsSinceEpoch(rootMs, QTimeZone::UTC)}};
}

QStringList ids(const QVariantList &rows)
{
    QStringList out;
    for (const QVariant &r : rows)
        out.append(r.toMap().value(QStringLiteral("rootEventId")).toString());
    return out;
}

QVariantMap spamCase(const QString &reason = QStringLiteral("spam"))
{
    return {{QStringLiteral("user"), QStringLiteral("@spam:evil.org")},
            {QStringLiteral("status"), QStringLiteral("open")},
            {QStringLiteral("evidence"), QVariantList{QVariantMap{
                {QStringLiteral("list"), QStringLiteral("!list:example.org")},
                {QStringLiteral("reason"), reason}}}}};
}
} // namespace

class CommunityToolsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void forumRowsCarryTitleTagsAndKeepUntaggedThreads()
    {
        const QVariantList threads{
            threadRow(QStringLiteral("$a"), QStringLiteral("Release plan #ops\nDetails #Ops #room:x.org"), 0, 30),
            threadRow(QStringLiteral("$b"), QStringLiteral("Untagged question"), 5, 10, true),
            threadRow(QStringLiteral("$c"), QStringLiteral("See https://x.org/#anchor #help"), 2, 20)};
        const QVariantList all = forum::arrange(threads, QStringLiteral("activity"), QStringLiteral("all"), {});
        QCOMPARE(ids(all), (QStringList{QStringLiteral("$a"), QStringLiteral("$b"), QStringLiteral("$c")}));
        const QVariantMap first = all.first().toMap();
        QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Release plan #ops"));
        QCOMPARE(first.value(QStringLiteral("preview")).toString(), QStringLiteral("Details #Ops #room:x.org"));
        QCOMPARE(first.value(QStringLiteral("tags")).toStringList(), QStringList{QStringLiteral("ops")});
        QVERIFY(first.value(QStringLiteral("unanswered")).toBool());
        QCOMPARE(all[2].toMap().value(QStringLiteral("tags")).toStringList(), QStringList{QStringLiteral("help")});

        QCOMPARE(ids(forum::arrange(threads, QStringLiteral("newest"), QStringLiteral("all"), {})),
                 (QStringList{QStringLiteral("$a"), QStringLiteral("$c"), QStringLiteral("$b")}));
        QCOMPARE(ids(forum::arrange(threads, QStringLiteral("replies"), QStringLiteral("all"), {})),
                 (QStringList{QStringLiteral("$b"), QStringLiteral("$c"), QStringLiteral("$a")}));
        QCOMPARE(ids(forum::arrange(threads, {}, QStringLiteral("unread"), {})), QStringList{QStringLiteral("$b")});
        QCOMPARE(ids(forum::arrange(threads, {}, QStringLiteral("unanswered"), {})), QStringList{QStringLiteral("$a")});
        QCOMPARE(ids(forum::arrange(threads, {}, QStringLiteral("all"), QStringLiteral("help"))),
                 QStringList{QStringLiteral("$c")});
        QCOMPARE(forum::allTags(threads), (QStringList{QStringLiteral("ops"), QStringLiteral("help")}));
    }

    void eventTimesFollowTheirZoneAcrossDaylightSaving()
    {
        if (!QTimeZone("Europe/Vilnius").isValid())
            QSKIP("no time-zone database on this machine");
        RoomCardsController cards;
        const QString vilnius = QStringLiteral("Europe/Vilnius");
        QCOMPARE(cards.toUtcMs(QStringLiteral("2026-07-01"), QStringLiteral("18:00"), vilnius),
                 QDateTime(QDate(2026, 7, 1), QTime(15, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(cards.toUtcMs(QStringLiteral("2026-12-01"), QStringLiteral("18:00"), vilnius),
                 QDateTime(QDate(2026, 12, 1), QTime(16, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        // 03:30 does not exist in Vilnius on the night clocks go forward.
        QCOMPARE(cards.toUtcMs(QStringLiteral("2026-03-29"), QStringLiteral("03:30"), vilnius), 0);
        QCOMPARE(cards.toUtcMs(QStringLiteral("2026-07-01"), QStringLiteral("18:00"), QStringLiteral("Mars/Base")), 0);

        const qint64 start = QDateTime(QDate(2026, 7, 1), QTime(15, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const QString body = RoomCardsController::fallbackBody(QStringLiteral("event"),
            {{QStringLiteral("title"), QStringLiteral("Meetup")}, {QStringLiteral("start"), start},
             {QStringLiteral("tz"), vilnius}, {QStringLiteral("location"), QStringLiteral("Hall")}});
        QVERIFY2(body.contains(QStringLiteral("When: 2026-07-01 18:00 (Europe/Vilnius, UTC+03:00)")), qPrintable(body));
        QVERIFY2(body.contains(QStringLiteral("UTC: 2026-07-01 15:00")), qPrintable(body));
        QVERIFY(body.startsWith(QStringLiteral("Event: Meetup")));

        const QString ics = RoomCardsController::icsText(
            {{QStringLiteral("kind"), QStringLiteral("event")}, {QStringLiteral("root"), root},
             {QStringLiteral("title"), QStringLiteral("Meet; up")}, {QStringLiteral("start"), start},
             {QStringLiteral("cancelled"), true}}, room);
        QVERIFY2(ics.contains(QStringLiteral("DTSTART:20260701T150000Z\r\n")), qPrintable(ics));
        QVERIFY(ics.contains(QStringLiteral("DTEND:20260701T160000Z\r\n")));
        QVERIFY(ics.contains(QStringLiteral("SUMMARY:Meet\\; up\r\n")));
        QVERIFY(ics.contains(QStringLiteral("STATUS:CANCELLED\r\n")));
    }

    void checklistFallbackListsEveryItemForOtherClients()
    {
        const QString body = RoomCardsController::fallbackBody(QStringLiteral("checklist"),
            {{QStringLiteral("title"), QStringLiteral("Groceries")},
             {QStringLiteral("items"), QVariantList{
                 QVariantMap{{QStringLiteral("id"), QStringLiteral("a")}, {QStringLiteral("text"), QStringLiteral("Milk")}},
                 QVariantMap{{QStringLiteral("id"), QStringLiteral("b")}, {QStringLiteral("text"), QStringLiteral("Eggs")}}}}});
        QCOMPARE(body, QStringLiteral("Checklist: Groceries\n☐ Milk\n☐ Eggs"));
    }

    void createSendsIntegerTimesAndTheFallback()
    {
        FakeClient client;
        RoomCardsController cards;
        cards.setClient(&client);
        QVERIFY(cards.create(room, QStringLiteral("event"),
            {{QStringLiteral("title"), QStringLiteral("Meetup")}, {QStringLiteral("start"), 1782918000000.0},
             {QStringLiteral("end"), 0.0}, {QStringLiteral("tz"), QStringLiteral("UTC")}}));
        QCOMPARE(client.lastAction, QStringLiteral("card_create"));
        const QVariantMap data = client.lastPayload.value(QStringLiteral("data")).toMap();
        QCOMPARE(data.value(QStringLiteral("start")).typeId(), int(QMetaType::LongLong));
        QCOMPARE(client.lastPayload.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("org.lightning_matrix.event"));
        QVERIFY(client.lastPayload.value(QStringLiteral("body")).toString().startsWith(QStringLiteral("Event: Meetup")));
        QVERIFY(!client.lastPayload.value(QStringLiteral("nonce")).toString().isEmpty());
        // One create at a time: a second click before the answer sends nothing.
        QVERIFY(!cards.create(room, QStringLiteral("checklist"), {}));
        QCOMPARE(client.actions.size(), 1);
    }

    void rsvpSetsAReminderAndCancellationStopsIt()
    {
        QTemporaryDir dir;
        FakeClient client;
        RemindersController reminders;
        reminders.setClient(&client);
        reminders.openFor(dir.filePath(QStringLiteral("reminders.json")));
        RoomCardsController cards;
        cards.setClient(&client);
        cards.setReminders(&reminders);

        const qint64 start = QDateTime::currentMSecsSinceEpoch() + 3 * 3600 * 1000;
        QVariantMap ev{{QStringLiteral("kind"), QStringLiteral("event")}, {QStringLiteral("deleted"), false},
                       {QStringLiteral("start"), start}, {QStringLiteral("cancelled"), false},
                       {QStringLiteral("own"), QString()}, {QStringLiteral("can_rsvp"), true}};
        cards.load(room, root, true);
        client.finish(true, ev);
        QCOMPARE(cards.card(room, root).value(QStringLiteral("start")).toLongLong(), start);

        QVERIFY(cards.rsvp(room, root, QStringLiteral("yes")));
        QCOMPARE(client.lastPayload.value(QStringLiteral("status")).toString(), QStringLiteral("yes"));
        client.finish(true, {});
        QCOMPARE(reminders.reminderDue(room, root), start - RoomCardsController::kReminderLeadMs);

        // The organiser moves it an hour later: the reminder follows.
        ev.insert(QStringLiteral("start"), start + 3600 * 1000);
        ev.insert(QStringLiteral("own"), QStringLiteral("yes"));
        QCOMPARE(client.lastAction, QStringLiteral("card_load"));
        client.finish(true, ev);
        QCOMPARE(reminders.reminderDue(room, root), start + 3600 * 1000 - RoomCardsController::kReminderLeadMs);

        ev.insert(QStringLiteral("cancelled"), true);
        cards.load(room, root, false);
        client.finish(true, ev);
        QCOMPARE(reminders.reminderDue(room, root), 0);
    }

    void aModerationBanNeedsAConfirmationForThatOnePerson()
    {
        FakeClient client;
        ModerationQueueController queue;
        queue.setClient(&client);
        queue.open(room);
        QCOMPARE(client.lastAction, QStringLiteral("mod_cases"));
        QVariantMap other = spamCase();
        other.insert(QStringLiteral("user"), QStringLiteral("@other:evil.org"));
        client.finish(true, {{QStringLiteral("cases"), QVariantList{spamCase(), other}},
                             {QStringLiteral("can_ban"), true}, {QStringLiteral("lists"), 1}});
        QCOMPARE(queue.cases().size(), 2);
        const int sent = client.actions.size();

        QVERIFY(!queue.confirmBan(QStringLiteral("@spam:evil.org"), {}));
        QVERIFY(queue.requestBan(QStringLiteral("@spam:evil.org")));
        QCOMPARE(client.actions.size(), sent);
        QVERIFY(!queue.confirmBan(QStringLiteral("@other:evil.org"), {}));
        QCOMPARE(client.actions.size(), sent);

        QVERIFY(queue.confirmBan(QStringLiteral("@spam:evil.org"), QStringLiteral("spam")));
        QCOMPARE(client.lastAction, QStringLiteral("mod_act"));
        QCOMPARE(client.lastPayload.value(QStringLiteral("decision")).toString(), QStringLiteral("ban"));
        QCOMPARE(client.lastPayload.value(QStringLiteral("user")).toString(), QStringLiteral("@spam:evil.org"));
        // Nothing else can be acted on while one ban is in flight.
        QVERIFY(!queue.requestBan(QStringLiteral("@other:evil.org")));
        QVERIFY(!queue.setDecision(QStringLiteral("@other:evil.org"), QStringLiteral("dismissed")));

        client.finish(false, {{QStringLiteral("category"), QStringLiteral("stale")}});
        QCOMPARE(client.lastAction, QStringLiteral("mod_cases"));
        QVERIFY(queue.cases().first().toMap().value(QStringLiteral("result")).toString()
                    .contains(QStringLiteral("no followed list")));
    }

    void aCaseThatChangedUnderTheConfirmationMustBeReviewedAgain()
    {
        FakeClient client;
        ModerationQueueController queue;
        queue.setClient(&client);
        queue.open(room);
        client.finish(true, {{QStringLiteral("cases"), QVariantList{spamCase()}}, {QStringLiteral("can_ban"), true}});
        QVERIFY(queue.requestBan(QStringLiteral("@spam:evil.org")));
        queue.refresh();
        client.finish(true, {{QStringLiteral("cases"), QVariantList{spamCase(QStringLiteral("rule edited"))}},
                             {QStringLiteral("can_ban"), true}});
        QVERIFY(queue.pending().isEmpty());
        QVERIFY(!queue.confirmBan(QStringLiteral("@spam:evil.org"), {}));

        // Lost permission: the queue offers no ban at all.
        queue.refresh();
        client.finish(true, {{QStringLiteral("cases"), QVariantList{spamCase()}}, {QStringLiteral("can_ban"), false}});
        QVERIFY(!queue.requestBan(QStringLiteral("@spam:evil.org")));
    }
};

QTEST_GUILESS_MAIN(CommunityToolsTest)
#include "CommunityToolsTest.moc"
