#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#include "models/RoomListModel.h"
#include "spaces/SpaceManager.h"
#endif

class RoomListDrainTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static constexpr int kDrainCap = 64;

    static QString roomId(int i) { return QStringLiteral("!r%1:example.org").arg(i); }

    static QJsonObject room(int i, const QString &name = {})
    {
        return QJsonObject{
            { QStringLiteral("id"), roomId(i) },
            { QStringLiteral("name"), name.isEmpty() ? QStringLiteral("Room %1").arg(i) : name },
        };
    }

    static QByteArray wire(const QJsonObject &event)
    {
        return QJsonDocument(event).toJson(QJsonDocument::Compact);
    }

    static QByteArray reset(int rooms)
    {
        QJsonArray list;
        for (int i = 0; i < rooms; ++i)
            list.append(room(i));
        return wire({ { QStringLiteral("type"), QStringLiteral("room_list_reset") },
                      { QStringLiteral("rooms"), list } });
    }

    static QByteArray set(int index, const QString &name)
    {
        return wire({ { QStringLiteral("type"), QStringLiteral("room_list_set") },
                      { QStringLiteral("index"), index },
                      { QStringLiteral("room"), room(index, name) } });
    }

    static QByteArray staleRemove(int index)
    {
        return wire({ { QStringLiteral("type"), QStringLiteral("room_list_remove") },
                      { QStringLiteral("index"), index },
                      { QStringLiteral("expected_id"), QStringLiteral("!gone:example.org") } });
    }

    static QByteArray spaces()
    {
        return wire({ { QStringLiteral("type"), QStringLiteral("space_list_reset") },
                      { QStringLiteral("spaces"), QJsonArray() } });
    }

    static int drainAll(RustSdkMatrixClient &client, QList<QByteArray> events)
    {
        int drains = 0;
        while (!events.isEmpty()) {
            const int left = client.drainEventsForTest(events);
            events = events.mid(events.size() - left);
            ++drains;
        }
        return drains;
    }

    static void roomCounts()
    {
        QTest::addColumn<int>("rooms");
        QTest::newRow("10") << 10;
        QTest::newRow("100") << 100;
        QTest::newRow("1000") << 1000;
    }

    static constexpr qint64 kNewestMs = 1700000000000;

    static QJsonObject activeRoom(int i, qint64 ms)
    {
        QJsonObject o = room(i);
        o.insert(QStringLiteral("last_activity_ms"), double(ms));
        return o;
    }

    struct Shown {
        SettingsManager settings;
        RustSdkMatrixClient client{ &settings };
        SpaceManager spaces;
        RoomListModel model;

        Shown(int rooms, bool keepStill)
        {
            QJsonArray list;
            for (int i = 0; i < rooms; ++i)
                list.append(activeRoom(i, kNewestMs - i * 60000LL));
            client.drainEventsForTest(
                { wire({ { QStringLiteral("type"), QStringLiteral("room_list_reset") },
                         { QStringLiteral("rooms"), list } }) });
            spaces.setClient(&client);
            model.setSpaceManager(&spaces);
            model.setClient(&client);
            model.setDeferReordering(keepStill);
        }

        int rowOf(int i) const
        {
            for (int row = 0; row < model.rowCount(); ++row) {
                if (model.data(model.index(row), RoomListModel::RoomIdRole).toString()
                    == roomId(i))
                    return row;
            }
            return -1;
        }
    };

    // The SDK's sorter reports a room that moved to the top as these two diffs.
    static QList<QByteArray> newMessage(int rooms, int at, bool burst)
    {
        const QList<QByteArray> move{
            wire({ { QStringLiteral("type"), QStringLiteral("room_list_remove") },
                   { QStringLiteral("index"), at },
                   { QStringLiteral("expected_id"), roomId(at) } }),
            wire({ { QStringLiteral("type"), QStringLiteral("room_list_push_front") },
                   { QStringLiteral("room"), activeRoom(at, kNewestMs + 60000) } }),
        };
        if (!burst)
            return move;
        QList<QByteArray> events{ set(0, QStringLiteral("Before")), spaces() };
        events += move;
        events += { spaces(), set(rooms - 1, QStringLiteral("After")) };
        return events;
    }

    static void movedRooms()
    {
        QTest::addColumn<int>("rooms");
        QTest::addColumn<bool>("burst");
        for (int rooms : { 10, 100, 1000 }) {
            QTest::addRow("%d single", rooms) << rooms << false;
            QTest::addRow("%d burst", rooms) << rooms << true;
        }
    }
#endif

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("room-list-drain-test"));
    }

#ifdef ENABLE_RUST_SDK_BACKEND
    void aNewMessageMovesItsRoomToTheTopAtTheDefaultSetting_data() { movedRooms(); }
    void aNewMessageMovesItsRoomToTheTopAtTheDefaultSetting()
    {
        QFETCH(int, rooms);
        QFETCH(bool, burst);
        Shown shown(rooms, SettingsManager().keepRoomListOrderStill());
        const int at = rooms / 2;

        QCOMPARE(shown.client.drainEventsForTest(newMessage(rooms, at, burst)), 0);

        QCOMPARE(shown.rowOf(at), 0);
    }

    // Why releases before the drain coalesced moved rooms despite the setting.
    void publishingEachDiffAloneDropsTheStillOrder_data() { movedRooms(); }
    void publishingEachDiffAloneDropsTheStillOrder()
    {
        QFETCH(int, rooms);
        QFETCH(bool, burst);
        Shown shown(rooms, true);
        const int at = rooms / 2;

        for (const QByteArray &event : newMessage(rooms, at, burst))
            shown.client.handleRustEventForTest(QJsonDocument::fromJson(event).object());

        QCOMPARE(shown.rowOf(at), 0);
        QVERIFY(!shown.model.orderHeld());
    }

    void aNewMessageMovesItsRoomToTheTopOfALiveList_data() { movedRooms(); }
    void aNewMessageMovesItsRoomToTheTopOfALiveList()
    {
        QFETCH(int, rooms);
        QFETCH(bool, burst);
        Shown shown(rooms, false);
        const int at = rooms / 2;
        QCOMPARE(shown.rowOf(at), at);

        QCOMPARE(shown.client.drainEventsForTest(newMessage(rooms, at, burst)), 0);

        QCOMPARE(shown.model.rowCount(), rooms);
        QCOMPARE(shown.rowOf(at), 0);
        QCOMPARE(shown.client.m_roomListResyncRequests, 0);
    }

    void aNewMessageLeavesAListKeptStillInPlaceUntilItIsReleased_data() { movedRooms(); }
    void aNewMessageLeavesAListKeptStillInPlaceUntilItIsReleased()
    {
        QFETCH(int, rooms);
        QFETCH(bool, burst);
        Shown shown(rooms, true);
        const int at = rooms / 2;

        QCOMPARE(shown.client.drainEventsForTest(newMessage(rooms, at, burst)), 0);

        QCOMPARE(shown.rowOf(at), at);
        QVERIFY(shown.model.orderHeld());
        QCOMPARE(shown.model.data(shown.model.index(at), RoomListModel::LastActivityRole)
                     .toDateTime().toMSecsSinceEpoch(),
                 kNewestMs + 60000);

        shown.model.releaseOrder();
        QCOMPARE(shown.rowOf(at), 0);
        QVERIFY(!shown.model.orderHeld());
    }
    void aBurstOfRoomListDiffsRefreshesConsumersOncePerDrain_data() { roomCounts(); }
    void aBurstOfRoomListDiffsRefreshesConsumersOncePerDrain()
    {
        QFETCH(int, rooms);
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.drainEventsForTest({ reset(rooms) });
        QCOMPARE(client.rooms().size(), rooms);

        QList<QByteArray> burst;
        for (int i = 0; i < rooms; ++i) {
            burst.append(set(i, QStringLiteral("Renamed %1").arg(i)));
            if (i % 8 == 7)
                burst.append(spaces());
        }
        const int events = burst.size();
        QSignalSpy refreshes(&client, &MatrixClient::roomsChanged);
        const int drains = drainAll(client, burst);

        QCOMPARE(drains, (events + kDrainCap - 1) / kDrainCap);
        QCOMPARE(refreshes.count(), drains);
        for (int i = 0; i < rooms; ++i)
            QCOMPARE(client.m_rooms.value(roomId(i)).name, QStringLiteral("Renamed %1").arg(i));
    }

    void aRoomListRunIsPublishedBeforeTheNextUnrelatedEvent()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.drainEventsForTest({ reset(4) });

        QStringList seenAtError;
        connect(&client, &MatrixClient::errorOccurred, this, [&] {
            seenAtError.append(client.m_rooms.value(roomId(1)).name);
        });
        QSignalSpy refreshes(&client, &MatrixClient::roomsChanged);
        client.drainEventsForTest({
            set(0, QStringLiteral("A")), set(1, QStringLiteral("B")),
            wire({ { QStringLiteral("type"), QStringLiteral("error") },
                   { QStringLiteral("message"), QStringLiteral("x") } }),
            set(2, QStringLiteral("C")), set(3, QStringLiteral("D")),
        });

        QCOMPARE(seenAtError, QStringList{ QStringLiteral("B") });
        QCOMPARE(refreshes.count(), 2);
    }

    void aRejectedDiffAsksForOneResyncUntilTheResetArrives_data() { roomCounts(); }
    void aRejectedDiffAsksForOneResyncUntilTheResetArrives()
    {
        QFETCH(int, rooms);
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.drainEventsForTest({ reset(rooms) });
        QCOMPARE(client.m_roomListResyncRequests, 0);

        QList<QByteArray> stale;
        for (int i = 0; i < rooms; ++i)
            stale.append(staleRemove(i % 5));
        drainAll(client, stale);
        QCOMPARE(client.m_roomListResyncRequests, 1);
        QCOMPARE(client.rooms().size(), rooms);

        // A diff queued before the reset is against the diverged index: it
        // must not be applied even where it happens to validate.
        client.drainEventsForTest({ set(0, QStringLiteral("Stale")) });
        QCOMPARE(client.m_rooms.value(roomId(0)).name, QStringLiteral("Room 0"));

        client.drainEventsForTest({ reset(rooms), set(0, QStringLiteral("Fresh")) });
        QCOMPARE(client.m_roomListResyncRequests, 1);
        QCOMPARE(client.m_rooms.value(roomId(0)).name, QStringLiteral("Fresh"));
    }

    void aLostResetIsAskedForAgainAndTheListRecovers()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.drainEventsForTest({ reset(5) });
        client.drainEventsForTest({ staleRemove(0) });
        QCOMPARE(client.m_roomListResyncRequests, 1);

        // The reset never arrives.
        QTRY_COMPARE_WITH_TIMEOUT(client.m_roomListResyncRequests, 2, 5000);

        // One request in flight: neither more stale diffs nor an overflow add one.
        client.drainEventsForTest({
            staleRemove(1),
            wire({ { QStringLiteral("type"), QStringLiteral("queue_overflow") },
                   { QStringLiteral("message"), QStringLiteral("x") } }),
        });
        QCOMPARE(client.m_roomListResyncRequests, 2);
        client.drainEventsForTest({ set(0, QStringLiteral("Stale")) });
        QCOMPARE(client.m_rooms.value(roomId(0)).name, QStringLiteral("Room 0"));

        client.drainEventsForTest({ reset(5), set(0, QStringLiteral("Fresh")) });
        QCOMPARE(client.m_rooms.value(roomId(0)).name, QStringLiteral("Fresh"));
        QTest::qWait(2300);
        QCOMPARE(client.m_roomListResyncRequests, 2);
    }

    void anotherAccountStartsWithNothingHeldOver()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.drainEventsForTest({ reset(10) });
        QList<QByteArray> burst{ staleRemove(0) };
        for (int i = 0; i < kDrainCap; ++i)
            burst.append(set(i % 10, QStringLiteral("N%1").arg(i)));
        QCOMPARE(client.drainEventsForTest(burst), 1);
        QVERIFY(client.m_roomListAwaitingReset);
        QVERIFY(client.m_backlogPollPending);

        client.clearLocalState();
        QVERIFY(!client.m_roomListAwaitingReset);
        QVERIFY(!client.m_backlogPollPending);
        QVERIFY(!client.m_roomsChangedPending);

        client.drainEventsForTest({
            wire({ { QStringLiteral("type"), QStringLiteral("room_list_push_back") },
                   { QStringLiteral("room"), room(42) } }),
        });
        QCOMPARE(client.rooms().size(), 1);
        QTest::qWait(2300);
        QCOMPARE(client.m_roomListResyncRequests, 1);
    }

    void aBacklogIsPolledAgainWithoutWaitingForTheTimer()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        client.drainEventsForTest({ reset(10) });
        QVERIFY(!client.m_backlogPollPending);

        QList<QByteArray> burst;
        for (int i = 0; i < kDrainCap + 1; ++i)
            burst.append(set(i % 10, QStringLiteral("N%1").arg(i)));
        QCOMPARE(client.drainEventsForTest(burst), 1);
        QVERIFY(client.m_backlogPollPending);

        QCoreApplication::processEvents();
        QVERIFY(!client.m_backlogPollPending);

        client.drainEventsForTest({ set(0, QStringLiteral("quiet")) });
        QVERIFY(!client.m_backlogPollPending);
    }
#else
    void skippedWithoutTheRustBackend() { QSKIP("needs ENABLE_RUST_SDK_BACKEND"); }
#endif

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(RoomListDrainTest)
#include "RoomListDrainTest.moc"
