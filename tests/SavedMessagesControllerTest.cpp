#include "app/SavedMessagesController.h"
#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

class SavedFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    QString user = QStringLiteral("@one:example.org");
    QVariantMap lastWrite;
    QString lastType;
    QString requestedRoom;
    QString requestedEvent;
    int reads = 0;
    bool supportsAccountPrefs() const override { return true; }
    bool isLoggedIn() const override { return !user.isEmpty(); }
    QString currentUserId() const override { return user; }
    QString homeserverUrl() const override { return {}; }
    void readAccountPref(const QString &type) override { lastType = type; ++reads; }
    void writeAccountPref(const QString &type, const QVariantMap &content) override
    { lastType = type; lastWrite = content; }
    bool useOps = false;
    quint64 lastOp = 0;
    QStringList requested;
    QHash<QString, quint64> opFor;
    quint64 requestEventSource(const QString &roomId, const QString &eventId) override
    {
        requestedRoom = roomId;
        requestedEvent = eventId;
        requested << eventId;
        const quint64 op = useOps ? ++lastOp : 0;
        opFor.insert(eventId, op);
        return op;
    }
    void answer(const QString &eventId, quint64 op, const QString &body,
                const QVariantMap &edited = {}, bool editUnreadable = false)
    {
        const QString json = QStringLiteral(
            "{\"type\":\"m.room.message\",\"sender\":\"@bob:example.org\","
            "\"content\":{\"msgtype\":\"m.text\",\"body\":\"%1\"}}").arg(body);
        Q_EMIT eventSourceReceived(QStringLiteral("!room:example.org"), eventId, true,
                                   json, QVariantMap{
                                       { QStringLiteral("opId"), op },
                                       { QStringLiteral("effectiveContent"), edited },
                                       { QStringLiteral("editUnreadable"), editUnreadable },
                                   });
    }
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { user.clear(); Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    RoomInfo roomInfo(const QString &roomId) const override
    { RoomInfo r; r.id = roomId; r.name = QStringLiteral("Room"); return r; }
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
};

namespace {
QVariantMap savedRef(const QString &room, const QString &event, qint64 at)
{
    return {{QStringLiteral("room_id"), room}, {QStringLiteral("event_id"), event},
            {QStringLiteral("thread_root_id"), QString()},
            {QStringLiteral("saved_at_ms"), at}};
}

QVariantMap savedList(const QVariantList &entries)
{
    return {{QStringLiteral("version"), 1}, {QStringLiteral("entries"), entries}};
}

QVariantMap entryFor(const SavedMessagesController &saved, const QString &event)
{
    for (const QVariant &v : saved.entries())
        if (v.toMap().value(QStringLiteral("eventId")) == event)
            return v.toMap();
    return {};
}

bool setNow(SavedMessagesController &saved, qint64 ms)
{
    return QMetaObject::invokeMethod(&saved, "setNowForTest", Q_ARG(qint64, ms));
}
}

class SavedMessagesControllerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void mergesFreshServerListAndKeepsLocalRemoval()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        const auto ref = [](const QString &event) {
            return QVariantMap{{QStringLiteral("room_id"), QStringLiteral("!room:example.org")},
                {QStringLiteral("event_id"), event},
                {QStringLiteral("thread_root_id"), QString()},
                {QStringLiteral("saved_at_ms"), 42}};
        };
        const auto content = [](const QVariantList &entries) {
            return QVariantMap{{QStringLiteral("version"), 1},
                               {QStringLiteral("entries"), entries}};
        };
        Q_EMIT client.accountPrefRead(client.lastType, true,
                                      content({ref(QStringLiteral("$old"))}));
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$local"), {});
        QCOMPARE(client.reads, 2);
        QVERIFY(client.lastWrite.isEmpty());
        Q_EMIT client.accountPrefRead(client.lastType, true,
            content({ref(QStringLiteral("$old")), ref(QStringLiteral("$remote"))}));
        QCOMPARE(client.lastWrite.value(QStringLiteral("entries")).toList().size(), 3);
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        Q_EMIT client.accountPrefRead(client.lastType, true,
            content({ref(QStringLiteral("$old")), ref(QStringLiteral("$remote")),
                     ref(QStringLiteral("$local"))}));
        saved.remove(QStringLiteral("!room:example.org"), QStringLiteral("$old"));
        Q_EMIT client.accountPrefRead(client.lastType, true,
            content({ref(QStringLiteral("$old")), ref(QStringLiteral("$remote")),
                     ref(QStringLiteral("$local")), ref(QStringLiteral("$new"))}));
        const auto written = client.lastWrite.value(QStringLiteral("entries")).toList();
        QCOMPARE(written.size(), 3);
        for (const QVariant &item : written)
            QVERIFY(item.toMap().value(QStringLiteral("event_id")) != QStringLiteral("$old"));
        QCOMPARE(saved.count(), 3);
    }

    void aSaveDoesNotResurrectAnItemAnotherDeviceRemoved()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        const auto ref = [](const QString &event) {
            return QVariantMap{{QStringLiteral("room_id"), QStringLiteral("!room:example.org")},
                {QStringLiteral("event_id"), event},
                {QStringLiteral("thread_root_id"), QString()},
                {QStringLiteral("saved_at_ms"), 42}};
        };
        const auto content = [](const QVariantList &entries) {
            return QVariantMap{{QStringLiteral("version"), 1},
                               {QStringLiteral("entries"), entries}};
        };
        Q_EMIT client.accountPrefRead(client.lastType, true,
            content({ref(QStringLiteral("$keep")), ref(QStringLiteral("$gone"))}));
        QCOMPARE(saved.count(), 2);
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$y"), {});
        Q_EMIT client.accountPrefRead(client.lastType, true,
            content({ref(QStringLiteral("$keep"))}));
        QStringList written;
        for (const QVariant &item : client.lastWrite.value(QStringLiteral("entries")).toList())
            written << item.toMap().value(QStringLiteral("event_id")).toString();
        written.sort();
        QCOMPARE(written, (QStringList{QStringLiteral("$keep"), QStringLiteral("$y")}));
    }

    void anEmptyObjectIsAnEmptyList()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, true, QVariantMap{});
        QCOMPARE(saved.count(), 0);
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$y"), {});
        Q_EMIT client.accountPrefRead(client.lastType, true, QVariantMap{});
        QCOMPARE(client.lastWrite.value(QStringLiteral("entries")).toList().size(), 1);
    }

    void invalidFreshServerListIsNeverOverwritten()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$local"), {});
        Q_EMIT client.accountPrefRead(client.lastType, true,
            QVariantMap{{QStringLiteral("version"), 1},
                        {QStringLiteral("entries"), QVariantList{QVariantMap{
                            {QStringLiteral("body"), QStringLiteral("secret")}}}}});
        QVERIFY(client.lastWrite.isEmpty());
        QVERIFY(!saved.busy());
        QCOMPARE(saved.count(), 0);
    }

    void concurrentSaveAtCapacityDoesNotEvictServerEntries()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$local"), {});
        QVariantList remote;
        for (int i = 0; i < 100; ++i) {
            remote.append(QVariantMap{
                {QStringLiteral("room_id"), QStringLiteral("!room:example.org")},
                {QStringLiteral("event_id"), QStringLiteral("$remote%1").arg(i)},
                {QStringLiteral("thread_root_id"), QString()},
                {QStringLiteral("saved_at_ms"), 42},
            });
        }
        Q_EMIT client.accountPrefRead(client.lastType, true,
            QVariantMap{{QStringLiteral("version"), 1},
                        {QStringLiteral("entries"), remote}});
        QVERIFY(client.lastWrite.isEmpty());
        QVERIFY(!saved.busy());
        QVERIFY(!saved.error().isEmpty());
    }

    void onlyReferencesAreWrittenAndAccountsAreIsolated()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        QCOMPARE(client.lastType, SavedMessagesController::eventType());
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QVERIFY(saved.loaded());
        QSignalSpy savedNotice(&saved, &SavedMessagesController::saveSucceeded);
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$reply"),
                   QStringLiteral("$root"));
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QCOMPARE(client.lastWrite.value(QStringLiteral("version")).toInt(), 1);
        const QVariantList refs = client.lastWrite.value(QStringLiteral("entries")).toList();
        QCOMPARE(refs.size(), 1);
        const QVariantMap ref = refs.first().toMap();
        QCOMPARE(ref.size(), 4);
        QCOMPARE(ref.value(QStringLiteral("room_id")).toString(),
                 QStringLiteral("!room:example.org"));
        QCOMPARE(ref.value(QStringLiteral("event_id")).toString(),
                 QStringLiteral("$reply"));
        QCOMPARE(ref.value(QStringLiteral("thread_root_id")).toString(),
                 QStringLiteral("$root"));
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(client.lastWrite))
                    .toJson().contains("secret body"));
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QCOMPARE(savedNotice.size(), 1);
        QSignalSpy opened(&saved, &SavedMessagesController::openRequested);
        saved.open(QStringLiteral("!room:example.org"), QStringLiteral("$reply"));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().at(2).toString(), QStringLiteral("$root"));
        client.logout();
        QCOMPARE(saved.count(), 0);
        QVERIFY(!saved.loaded());
        client.user = QStringLiteral("@two:example.org");
        Q_EMIT client.loginSucceeded(client.user);
        QCOMPARE(saved.count(), 0);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QVERIFY(!saved.isSaved(QStringLiteral("!room:example.org"),
                               QStringLiteral("$reply")));
    }

    void removalUndoAndRedactionNeverRestoreCachedText()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$event"), {});
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        Q_EMIT client.accountPrefRead(client.lastType, true, client.lastWrite);
        saved.resolveVisible();
        QCOMPARE(client.requestedEvent, QStringLiteral("$event"));
        Q_EMIT client.eventSourceReceived(QStringLiteral("!room:example.org"),
                                          QStringLiteral("$event"), true,
            QStringLiteral("{\"type\":\"m.room.message\",\"sender\":\"@bob:example.org\",\"content\":{\"msgtype\":\"m.text\",\"body\":\"secret body\"}}"), {});
        QCOMPARE(saved.entries().first().toMap().value(QStringLiteral("preview"))
                     .toString(), QStringLiteral("secret body"));
        Q_EMIT client.eventRedacted(QStringLiteral("!room:example.org"),
                                    QStringLiteral("$event"));
        const QVariantMap redacted = saved.entries().first().toMap();
        QVERIFY(!redacted.value(QStringLiteral("available")).toBool());
        QVERIFY(redacted.value(QStringLiteral("preview")).toString().isEmpty());
        saved.remove(QStringLiteral("!room:example.org"), QStringLiteral("$event"));
        Q_EMIT client.accountPrefRead(client.lastType, true, client.lastWrite);
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QCOMPARE(saved.count(), 0);
        Q_EMIT client.accountPrefRead(client.lastType, true, client.lastWrite);
        saved.undoRemove();
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QCOMPARE(saved.count(), 1);
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(client.lastWrite))
                    .toJson().contains("secret body"));
        QVERIFY(saved.entries().first().toMap().value(QStringLiteral("preview"))
                    .toString().isEmpty());
    }

    void failedWriteRestoresThePriorList()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$event"), {});
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QCOMPARE(saved.count(), 1);
        Q_EMIT client.accountPrefWritten(client.lastType, false,
                                          QStringLiteral("timeout"));
        QCOMPARE(saved.count(), 0);
        QVERIFY(!saved.busy());
        QVERIFY(!saved.error().isEmpty());
    }

    void aWriteRefusedAfterAnInferredAbsenceRereadsOnceAndWritesTheMergedList()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$mine"), {});
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        QCOMPARE(saved.count(), 1);
        const int readsBefore = client.reads;
        Q_EMIT client.accountPrefWritten(client.lastType, false,
                                          QStringLiteral("stale_absence"));
        QVERIFY(saved.busy());
        QCOMPARE(client.reads, readsBefore + 1);
        client.lastWrite.clear();
        const QVariantMap theirs{
            { QStringLiteral("room_id"), QStringLiteral("!room:example.org") },
            { QStringLiteral("event_id"), QStringLiteral("$theirs") },
            { QStringLiteral("thread_root_id"), QString() },
            { QStringLiteral("saved_at_ms"), 5 },
        };
        Q_EMIT client.accountPrefRead(client.lastType, true,
            { { QStringLiteral("version"), 1 },
              { QStringLiteral("entries"), QVariantList{ theirs } } });
        QCOMPARE(client.lastWrite.value(QStringLiteral("entries")).toList().size(), 2);
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        QVERIFY(!saved.busy());
        QCOMPARE(saved.count(), 2);

        // a second refusal in the same action is surfaced, not retried again
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$more"), {});
        Q_EMIT client.accountPrefRead(client.lastType, true, client.lastWrite);
        Q_EMIT client.accountPrefWritten(client.lastType, false, QStringLiteral("stale_absence"));
        Q_EMIT client.accountPrefRead(client.lastType, true, client.lastWrite);
        Q_EMIT client.accountPrefWritten(client.lastType, false, QStringLiteral("stale_absence"));
        QVERIFY(!saved.busy());
        QVERIFY(!saved.error().isEmpty());
    }

    void redactionWinsOverAnInFlightResolution()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        saved.save(QStringLiteral("!room:example.org"), QStringLiteral("$event"), {});
        Q_EMIT client.accountPrefRead(client.lastType, false, {});
        Q_EMIT client.accountPrefWritten(client.lastType, true, {});
        saved.resolveVisible();
        Q_EMIT client.eventRedacted(QStringLiteral("!room:example.org"),
                                    QStringLiteral("$event"));
        Q_EMIT client.eventSourceReceived(QStringLiteral("!room:example.org"),
                                          QStringLiteral("$event"), true,
            QStringLiteral("{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\",\"body\":\"stale secret\"}}"), {});
        const QVariantMap ref = saved.entries().first().toMap();
        QVERIFY(ref.value(QStringLiteral("resolved")).toBool());
        QVERIFY(!ref.value(QStringLiteral("available")).toBool());
        QVERIFY(ref.value(QStringLiteral("preview")).toString().isEmpty());
    }

    void aReplacementRequestGetsItsOwnTimeoutBudget()
    {
        SavedFakeClient client;
        client.useOps = true;
        SavedMessagesController saved;
        saved.setClient(&client);
        QVERIFY(setNow(saved, 0));
        const QString room = QStringLiteral("!room:example.org");
        QVariantList refs;
        for (int i = 1; i <= 5; ++i)
            refs << savedRef(room, QStringLiteral("$%1").arg(i), 100 - i);
        Q_EMIT client.accountPrefRead(client.lastType, true, savedList(refs));
        saved.resolveVisible();
        QCOMPARE(client.requested, (QStringList{QStringLiteral("$1"), QStringLiteral("$2"),
                                                QStringLiteral("$3"), QStringLiteral("$4")}));

        QVERIFY(setNow(saved, 14000));
        client.answer(QStringLiteral("$1"), client.opFor.value(QStringLiteral("$1")),
                      QStringLiteral("one"));
        QCOMPARE(client.requested.last(), QStringLiteral("$5"));

        QVERIFY(setNow(saved, 15000));
        QVERIFY(entryFor(saved, QStringLiteral("$2")).value(QStringLiteral("failed")).toBool());
        QVERIFY(!entryFor(saved, QStringLiteral("$5")).value(QStringLiteral("resolved")).toBool());

        QVERIFY(setNow(saved, 16000));
        client.answer(QStringLiteral("$5"), client.opFor.value(QStringLiteral("$5")),
                      QStringLiteral("five"));
        QCOMPARE(entryFor(saved, QStringLiteral("$5")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("five"));

        const quint64 expiredOp = client.opFor.value(QStringLiteral("$2"));
        client.answer(QStringLiteral("$2"), expiredOp, QStringLiteral("too late"));
        QVERIFY(!entryFor(saved, QStringLiteral("$2")).value(QStringLiteral("available")).toBool());
        QVERIFY(entryFor(saved, QStringLiteral("$2")).value(QStringLiteral("failed")).toBool());

        const int before = client.requested.size();
        QVERIFY(QMetaObject::invokeMethod(&saved, "retryPreview", Q_ARG(QString, room),
                                          Q_ARG(QString, QStringLiteral("$2"))));
        QCOMPARE(client.requested.size(), before + 1);
        QVERIFY(!entryFor(saved, QStringLiteral("$2")).value(QStringLiteral("failed")).toBool());
        client.answer(QStringLiteral("$2"), expiredOp, QStringLiteral("too late"));
        QVERIFY(!entryFor(saved, QStringLiteral("$2")).value(QStringLiteral("available")).toBool());
        client.answer(QStringLiteral("$2"), client.opFor.value(QStringLiteral("$2")),
                      QStringLiteral("two"));
        QCOMPARE(entryFor(saved, QStringLiteral("$2")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("two"));
    }

    void anAccountSwitchMidResolveDropsLateAnswersAndPreviews()
    {
        SavedFakeClient client;
        client.useOps = true;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, true,
            savedList({savedRef(QStringLiteral("!room:example.org"), QStringLiteral("$1"), 9)}));
        saved.resolveVisible();
        const quint64 op = client.opFor.value(QStringLiteral("$1"));
        client.user = QStringLiteral("@two:example.org");
        Q_EMIT client.loginSucceeded(client.user);
        Q_EMIT client.accountPrefRead(client.lastType, true,
            savedList({savedRef(QStringLiteral("!room:example.org"), QStringLiteral("$1"), 9)}));
        client.answer(QStringLiteral("$1"), op, QStringLiteral("account one secret"));
        QVERIFY(entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview"))
                    .toString().isEmpty());
    }

    void filtersFollowTheAccountAndTheRoomsThatAreStillSaved()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        const QVariantMap both = savedList({
            savedRef(QStringLiteral("!a:example.org"), QStringLiteral("$1"), 9),
            savedRef(QStringLiteral("!b:example.org"), QStringLiteral("$2"), 8)});
        Q_EMIT client.accountPrefRead(client.lastType, true, both);
        saved.setRoomFilter(QStringLiteral("!a:example.org"));
        saved.setTypeFilter(QStringLiteral("media"));
        saved.setQuery(QStringLiteral("x"));
        saved.refresh();
        Q_EMIT client.accountPrefRead(client.lastType, true, both);
        QCOMPARE(saved.roomFilter(), QStringLiteral("!a:example.org"));
        QCOMPARE(saved.query(), QStringLiteral("x"));

        saved.setTypeFilter(QString());
        saved.setQuery(QString());
        saved.refresh();
        Q_EMIT client.accountPrefRead(client.lastType, true, savedList({
            savedRef(QStringLiteral("!b:example.org"), QStringLiteral("$2"), 8)}));
        QCOMPARE(saved.roomFilter(), QString());
        QCOMPARE(saved.entries().size(), 1);

        saved.setRoomFilter(QStringLiteral("!b:example.org"));
        saved.setTypeFilter(QStringLiteral("text"));
        saved.setQuery(QStringLiteral("y"));
        client.user = QStringLiteral("@two:example.org");
        Q_EMIT client.loginSucceeded(client.user);
        QCOMPARE(saved.roomFilter(), QString());
        QCOMPARE(saved.typeFilter(), QString());
        QCOMPARE(saved.query(), QString());
        Q_EMIT client.accountPrefRead(client.lastType, true, savedList({
            savedRef(QStringLiteral("!c:example.org"), QStringLiteral("$3"), 7)}));
        QCOMPARE(saved.entries().size(), 1);

        saved.setQuery(QStringLiteral("nothing matches"));
        QCOMPARE(saved.entries().size(), 0);
        QVERIFY(QMetaObject::invokeMethod(&saved, "clearFilters"));
        QCOMPARE(saved.entries().size(), 1);
    }

    void anEditReplacesTheResolvedPreviewAndItsFilterMatch()
    {
        SavedFakeClient client;
        client.useOps = true;
        SavedMessagesController saved;
        saved.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        Q_EMIT client.accountPrefRead(client.lastType, true,
                                      savedList({savedRef(room, QStringLiteral("$1"), 9)}));
        saved.resolveVisible();
        const quint64 first = client.opFor.value(QStringLiteral("$1"));
        client.answer(QStringLiteral("$1"), first, QStringLiteral("teh typo"));
        saved.setQuery(QStringLiteral("typo"));
        QCOMPARE(saved.entries().size(), 1);

        Q_EMIT client.eventEdited(room, QStringLiteral("$1"));
        QCOMPARE(client.requested.size(), 2);
        QCOMPARE(entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("teh typo"));
        const quint64 second = client.opFor.value(QStringLiteral("$1"));
        client.answer(QStringLiteral("$1"), second, QStringLiteral("teh typo"),
                      QVariantMap{{QStringLiteral("msgtype"), QStringLiteral("m.text")},
                                  {QStringLiteral("body"), QStringLiteral("the fix")}});
        QCOMPARE(saved.entries().size(), 0);
        saved.setQuery(QStringLiteral("fix"));
        QCOMPARE(entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("the fix"));

        client.answer(QStringLiteral("$1"), first, QStringLiteral("teh typo"));
        QCOMPARE(entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("the fix"));

        TimelineEvent edited;
        edited.eventId = QStringLiteral("$1");
        edited.edited = true;
        edited.body = QStringLiteral("encrypted edit");
        Q_EMIT client.eventChangedAt(MatrixClient::threadTimelineId(room, QStringLiteral("$root")),
                                     0, edited);
        QCOMPARE(client.requested.size(), 3);
        Q_EMIT client.eventChangedAt(room, 0, edited);
        QCOMPARE(client.requested.size(), 3);
        saved.setQuery(QString());
        client.answer(QStringLiteral("$1"), client.opFor.value(QStringLiteral("$1")),
                      QStringLiteral("teh typo"), {}, true);
        const QString unreadable =
            entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview")).toString();
        QVERIFY(!unreadable.isEmpty());
        QVERIFY(!unreadable.contains(QStringLiteral("typo")));

        Q_EMIT client.eventEdited(room, QStringLiteral("$1"));
        Q_EMIT client.eventRedacted(room, QStringLiteral("$1"));
        client.answer(QStringLiteral("$1"), client.opFor.value(QStringLiteral("$1")),
                      QStringLiteral("resurrected"));
        QVERIFY(saved.entries().first().toMap().value(QStringLiteral("preview"))
                    .toString().isEmpty());
        QVERIFY(!saved.entries().first().toMap().value(QStringLiteral("available")).toBool());
    }

    void reopeningRereadsPreviewsAndKeepsTheirTextMeanwhile()
    {
        SavedFakeClient client;
        client.useOps = true;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, true,
            savedList({savedRef(QStringLiteral("!room:example.org"), QStringLiteral("$1"), 9)}));
        saved.resolveVisible();
        client.answer(QStringLiteral("$1"), client.opFor.value(QStringLiteral("$1")),
                      QStringLiteral("before"));
        QVERIFY(QMetaObject::invokeMethod(&saved, "refreshPreviews"));
        QCOMPARE(client.requested.size(), 2);
        QCOMPARE(entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("before"));
        client.answer(QStringLiteral("$1"), client.opFor.value(QStringLiteral("$1")),
                      QStringLiteral("after"));
        QCOMPARE(entryFor(saved, QStringLiteral("$1")).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("after"));
    }

    void malformedAccountDataCannotBecomeABookmark()
    {
        SavedFakeClient client;
        SavedMessagesController saved;
        saved.setClient(&client);
        Q_EMIT client.accountPrefRead(client.lastType, true, QVariantMap{
            { QStringLiteral("version"), 1 },
            { QStringLiteral("entries"), QVariantList{QVariantMap{
                { QStringLiteral("room_id"), QStringLiteral("!r:example.org\x1f" "thread") },
                { QStringLiteral("event_id"), QStringLiteral("$e") },
                { QStringLiteral("thread_root_id"), QStringLiteral("$root") },
                { QStringLiteral("saved_at_ms"), 42 },
            }} },
        });
        QVERIFY(!saved.loaded());
        QCOMPARE(saved.count(), 0);
        QVERIFY(!saved.error().isEmpty());
    }
};

QTEST_GUILESS_MAIN(SavedMessagesControllerTest)
#include "SavedMessagesControllerTest.moc"
