#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
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
#endif

private Q_SLOTS:
#ifdef ENABLE_RUST_SDK_BACKEND
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
};

QTEST_MAIN(RoomListDrainTest)
#include "RoomListDrainTest.moc"
