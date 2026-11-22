
#include "matrix/MockMatrixClient.h"
#include "models/QuickSwitcherModel.h"
#include "models/TextFold.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 2000;

QStringList names(const QuickSwitcherModel &m)
{
    QStringList out;
    for (int row = 0; row < m.rowCount(); ++row)
        out << m.data(m.index(row, 0), QuickSwitcherModel::NameRole).toString();
    return out;
}

class ManyRoomsClient final : public MockMatrixClient
{
public:
    QList<RoomInfo> fixture;
    QList<RoomInfo> rooms() const override { return fixture; }
};

RoomInfo fixtureRoom(const QString &id, const QString &name,
                     const QString &category)
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    r.isDirect = category == QLatin1String("dm");
    r.isSpace = category == QLatin1String("space");
    if (category == QLatin1String("invite"))
        r.membership = RoomInfo::Invited;
    return r;
}

QList<RoomInfo> fiftyFiveWeakRooms()
{
    QList<RoomInfo> out;
    for (int i = 0; i < 55; ++i)
        out << fixtureRoom(QStringLiteral("!r%1:x").arg(i, 2, 10, QLatin1Char('0')),
                           QStringLiteral("Chat with alice %1").arg(i),
                           QStringLiteral("room"));
    return out;
}

QString idAt(const QuickSwitcherModel &m, int row)
{
    return m.resultAt(row).value(QStringLiteral("roomId")).toString();
}

int rowOf(const QuickSwitcherModel &m, const QString &roomId)
{
    for (int row = 0; row < m.rowCount(); ++row)
        if (idAt(m, row) == roomId)
            return row;
    return -1;
}

QString categoryFor(const QuickSwitcherModel &m, const QString &name)
{
    for (int row = 0; row < m.rowCount(); ++row) {
        const QModelIndex idx = m.index(row, 0);
        if (m.data(idx, QuickSwitcherModel::NameRole).toString() == name)
            return m.data(idx, QuickSwitcherModel::CategoryRole).toString();
    }
    return {};
}
}

class QuickSwitcherTest : public QObject
{
    Q_OBJECT

    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

private Q_SLOTS:
    void matchingIgnoresAccentsAndCase()
    {
        const QString cafe = QStringLiteral("Caf\u00e9 Z\u00fcrich");
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("cafe"), cafe, {}) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("ZURICH"), cafe, {}) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("caf\u00e9"),
                                               QStringLiteral("Cafe"), {}) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(
                    QStringLiteral("jose"), QStringLiteral("Direct chat"),
                    { QStringLiteral("Jos\u00e9") }) > 0);
        QCOMPARE(QuickSwitcherModel::matchScore(QStringLiteral("cafe"), cafe, {}),
                 1000);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("xyz"), cafe, {}) < 0);
        QCOMPARE(textfold::fold(QStringLiteral("\u00c5ngstr\u00f6m")),
                 QStringLiteral("angstrom"));
    }

    void matchScorePolicy()
    {
        QCOMPARE(QuickSwitcherModel::matchScore(QString(), QStringLiteral("x"),
                                                {}), 0);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("zzz"),
                                               QStringLiteral("General"), {})
                < 0);
        const int prefix = QuickSwitcherModel::matchScore(
            QStringLiteral("gen"), QStringLiteral("General"), {});
        const int word = QuickSwitcherModel::matchScore(
            QStringLiteral("team"), QStringLiteral("Dev Team"), {});
        const int sub = QuickSwitcherModel::matchScore(
            QStringLiteral("ener"), QStringLiteral("General"), {});
        QVERIFY(prefix > word);
        QVERIFY(word > sub);
        QVERIFY(sub > 0);
        QVERIFY(QuickSwitcherModel::matchScore(
                    QStringLiteral("bob"), QStringLiteral("Direct chat"),
                    { QStringLiteral("@bob:mock.local") }) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(
                    QStringLiteral("bob chat"), QStringLiteral("Direct chat"),
                    { QStringLiteral("@bob:mock.local") }) > 0);
    }

    void emptyQueryListsAllRoomsWithCategories()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.refresh();

        const QStringList all = names(model);
        QVERIFY(all.contains(QStringLiteral("General")));
        QVERIFY(all.contains(QStringLiteral("Developers")));
        QVERIFY(all.contains(QStringLiteral("Bob")));
        QVERIFY(all.contains(QStringLiteral("Team")));

        QCOMPARE(categoryFor(model, QStringLiteral("Bob")),
                 QStringLiteral("dm"));
        QCOMPARE(categoryFor(model, QStringLiteral("Team")),
                 QStringLiteral("space"));
        QCOMPARE(categoryFor(model, QStringLiteral("General")),
                 QStringLiteral("room"));
    }

    void nameSearchFiltersAndRanks()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);

        model.setQuery(QStringLiteral("dev"));
        QCOMPARE(names(model), QStringList{ QStringLiteral("Developers") });

        model.setQuery(QStringLiteral("team"));
        QCOMPARE(names(model), QStringList{ QStringLiteral("Team") });
    }

    void dmMatchesByParticipantMxid()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);

        // The DM is named "Bob" and its participant is @bob:mock.local —
        // typing the mxid still finds it.
        model.setQuery(QStringLiteral("@bob"));
        const QStringList found = names(model);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first(), QStringLiteral("Bob"));
        QCOMPARE(categoryFor(model, QStringLiteral("Bob")),
                 QStringLiteral("dm"));
    }

    void noMatchYieldsEmpty()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("nonexistent-room-xyz"));
        QCOMPARE(model.rowCount(), 0);
    }

    void resultAtCarriesRoutingPayload()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("team"));
        QCOMPARE(model.rowCount(), 1);
        const QVariantMap r = model.resultAt(0);
        QCOMPARE(r.value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!space-team:mock.local"));
        QVERIFY(r.value(QStringLiteral("isSpace")).toBool());
        QVERIFY(!r.value(QStringLiteral("isInvite")).toBool());
    }

    void logoutClearsResults()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.refresh();
        QVERIFY(model.rowCount() > 0);

        client.logout();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.query().isEmpty());
    }

    void anExactDmIsNotCrowdedOutByFiftyWeakRoomMatches()
    {
        ManyRoomsClient client;
        client.fixture = fiftyFiveWeakRooms();
        client.fixture << fixtureRoom(QStringLiteral("!dm:x"),
                                      QStringLiteral("Alice"),
                                      QStringLiteral("dm"));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("alice"));
        const int row = rowOf(model, QStringLiteral("!dm:x"));
        QVERIFY(row >= 0);
        QCOMPARE(model.resultAt(row).value(QStringLiteral("category")).toString(),
                 QStringLiteral("dm"));
        int rooms = 0;
        for (int i = 0; i < model.rowCount(); ++i)
            rooms += model.resultAt(i).value(QStringLiteral("category"))
                         == QStringLiteral("room");
        QCOMPARE(rooms, 50);
    }

    void anEmptyQueryStillListsPeopleSpacesAndInvitesPastFiftyRooms()
    {
        ManyRoomsClient client;
        client.fixture = fiftyFiveWeakRooms();
        client.fixture << fixtureRoom(QStringLiteral("!dm:x"), QStringLiteral("Bob"),
                                      QStringLiteral("dm"))
                       << fixtureRoom(QStringLiteral("!sp:x"), QStringLiteral("Team"),
                                      QStringLiteral("space"))
                       << fixtureRoom(QStringLiteral("!inv:x"), QStringLiteral("Party"),
                                      QStringLiteral("invite"));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.refresh();
        QVERIFY(rowOf(model, QStringLiteral("!dm:x")) >= 0);
        QVERIFY(rowOf(model, QStringLiteral("!sp:x")) >= 0);
        QVERIFY(rowOf(model, QStringLiteral("!inv:x")) >= 0);
        QCOMPARE(model.rowCount(), 53);
        QStringList order;
        for (int i = 0; i < model.rowCount(); ++i) {
            const QString cat =
                model.resultAt(i).value(QStringLiteral("category")).toString();
            if (order.isEmpty() || order.last() != cat)
                order << cat;
        }
        QCOMPARE(order, (QStringList{ QStringLiteral("room"), QStringLiteral("dm"),
                                      QStringLiteral("space"),
                                      QStringLiteral("invite") }));

        model.setQuery(QStringLiteral("team"));
        QCOMPARE(idAt(model, 0), QStringLiteral("!sp:x"));
        model.setQuery(QStringLiteral("party"));
        QCOMPARE(idAt(model, 0), QStringLiteral("!inv:x"));
        QVERIFY(model.resultAt(0).value(QStringLiteral("isInvite")).toBool());
    }

    void exactMatchesOutrankWeakOnesAndEqualNamesOrderByRoomId()
    {
        ManyRoomsClient client;
        client.fixture = fiftyFiveWeakRooms();
        client.fixture << fixtureRoom(QStringLiteral("!b:x"), QStringLiteral("Alice"),
                                      QStringLiteral("room"))
                       << fixtureRoom(QStringLiteral("!a:x"), QStringLiteral("Alice"),
                                      QStringLiteral("room"));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("alice"));
        QCOMPARE(idAt(model, 0), QStringLiteral("!a:x"));
        QCOMPARE(idAt(model, 1), QStringLiteral("!b:x"));
    }

    void anExactNameWithAccentsIsNotCrowdedOutByPrefixMatches()
    {
        ManyRoomsClient client;
        for (int i = 0; i < 55; ++i)
            client.fixture << fixtureRoom(
                QStringLiteral("!p%1:x").arg(i, 2, 10, QLatin1Char('0')),
                QStringLiteral("Alice chat %1").arg(i), QStringLiteral("room"));
        client.fixture << fixtureRoom(QStringLiteral("!exact:x"),
                                      QStringLiteral("Ålice"),
                                      QStringLiteral("room"));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("alice"));
        QCOMPARE(idAt(model, 0), QStringLiteral("!exact:x"));
    }

    void eachCategoryReportsTheMatchesItsCapLeftOut()
    {
        ManyRoomsClient client;
        client.fixture = fiftyFiveWeakRooms();
        client.fixture << fixtureRoom(QStringLiteral("!dm:x"), QStringLiteral("Alice"),
                                      QStringLiteral("dm"));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("alice"));
        const QVariantMap hidden = model.property("hiddenByCategory").toMap();
        QCOMPARE(hidden.value(QStringLiteral("room")).toInt(), 5);
        QVERIFY(!hidden.contains(QStringLiteral("dm")));

        model.setQuery(QStringLiteral("alice 7"));
        QVERIFY(model.property("hiddenByCategory").toMap().isEmpty());
    }

    void aPastedRoomIdAliasOrLinkFindsTheRoomLocally()
    {
        ManyRoomsClient client;
        client.fixture = fiftyFiveWeakRooms();
        RoomInfo lounge = fixtureRoom(QStringLiteral("!named:x"),
                                      QStringLiteral("Lounge"), QStringLiteral("room"));
        lounge.canonicalAlias = QStringLiteral("#lounge:x");
        client.fixture << lounge;
        QuickSwitcherModel model;
        model.setClient(&client);

        const QStringList local = {
            QStringLiteral("!named:x"), QStringLiteral("  #lounge:x "),
            QStringLiteral("https://matrix.to/#/%23lounge%3Ax"),
            QStringLiteral("https://matrix.to/#/!named:x?via=x"),
            QStringLiteral("matrix:r/lounge:x"),
        };
        for (const QString &query : local) {
            model.setQuery(query);
            QCOMPARE(model.rowCount(), 1);
            QCOMPARE(idAt(model, 0), QStringLiteral("!named:x"));
            QVERIFY2(model.resultAt(0).value(QStringLiteral("link")).toString().isEmpty(),
                     qPrintable(query));
        }

        const QStringList toEvent = {
            QStringLiteral("https://matrix.to/#/!named:x/$ev1?via=x"),
            QStringLiteral("matrix:roomid/named:x/e/ev1"),
        };
        for (const QString &query : toEvent) {
            model.setQuery(query);
            QCOMPARE(model.rowCount(), 1);
            QCOMPARE(idAt(model, 0), QStringLiteral("!named:x"));
            QCOMPARE(model.resultAt(0).value(QStringLiteral("link")).toString(), query);
        }

        model.setQuery(QStringLiteral("!named"));
        QCOMPARE(model.rowCount(), 0);
        model.setQuery(QStringLiteral("lounge"));
        QCOMPARE(idAt(model, 0), QStringLiteral("!named:x"));
    }

    void anUnknownAddressOffersAPreviewRowAndNeverARoomJump()
    {
        ManyRoomsClient client;
        client.fixture << fixtureRoom(QStringLiteral("!named:x"),
                                      QStringLiteral("Lounge"), QStringLiteral("room"));
        QuickSwitcherModel model;
        model.setClient(&client);
        for (const QString &query : { QStringLiteral("#elsewhere:example.org"),
                                      QStringLiteral("!other:[::1]:8448") }) {
            model.setQuery(query);
            QCOMPARE(model.rowCount(), 1);
            const QVariantMap row = model.resultAt(0);
            QCOMPARE(row.value(QStringLiteral("category")).toString(),
                     QStringLiteral("address"));
            QCOMPARE(row.value(QStringLiteral("link")).toString(), query);
            QVERIFY(!row.value(QStringLiteral("isSpace")).toBool());
        }
    }

    void anOpenPaletteFollowsRoomChangesAndAClosedOneDoesNot()
    {
        ManyRoomsClient client;
        client.fixture << fixtureRoom(QStringLiteral("!a:x"), QStringLiteral("Alpha"),
                                      QStringLiteral("room"))
                       << fixtureRoom(QStringLiteral("!b:x"), QStringLiteral("Beta"),
                                      QStringLiteral("room"));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.refresh();
        QCOMPARE(model.rowCount(), 2);
        QSignalSpy changed(&model, &QuickSwitcherModel::resultsChanged);

        Q_EMIT client.roomsChanged();
        QCOMPARE(changed.count(), 0);

        client.fixture.removeFirst();
        Q_EMIT client.roomsChanged();
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(idAt(model, 0), QStringLiteral("!b:x"));

        model.reset();
        client.fixture << fixtureRoom(QStringLiteral("!c:x"), QStringLiteral("Gamma"),
                                      QStringLiteral("room"));
        Q_EMIT client.roomsChanged();
        QCOMPARE(model.rowCount(), 0);
    }
};

QTEST_MAIN(QuickSwitcherTest)
#include "QuickSwitcherTest.moc"
