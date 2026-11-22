
#include "matrix/MatrixClient.h"
#include "models/UserLookup.h"
#include "models/UserSearchModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastSearchOp = 0;
    quint64 lastProfileOp = 0;
    QString lastSearchQuery;
    QString lastProfileUser;
    int profileCalls = 0;

    bool supportsRoomManagement() const override { return true; }
    quint64 searchUsers(const QString &query, int) override
    {
        lastSearchQuery = query;
        lastSearchOp = nextOp++;
        return lastSearchOp;
    }
    quint64 fetchUserProfile(const QString &userId) override
    {
        ++profileCalls;
        lastProfileUser = userId;
        lastProfileOp = nextOp++;
        return lastProfileOp;
    }

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
};

struct Rig {
    FakeClient client;
    UserSearchModel model;

    Rig()
    {
        model.setClient(&client);
        model.setDebounceMs(0);
    }

    void search(const QString &query)
    {
        model.setQuery(query);
        QTest::qWait(1);
    }
    void finishDirectory(const QVariantList &rows, bool ok = true)
    {
        Q_EMIT client.userSearchFinished(client.lastSearchOp, ok, rows, false,
                                         ok ? QString()
                                            : QStringLiteral("network"));
    }
    void finishDirectoryAs(const QVariantList &rows, bool limited,
                           const QString &failure = {})
    {
        Q_EMIT client.userSearchFinished(client.lastSearchOp, failure.isEmpty(),
                                         rows, limited, failure);
    }
    void confirmProfile(const QString &userId, const QString &displayName)
    {
        Q_EMIT client.userProfileFinished(client.lastProfileOp, true, userId,
                                          displayName, QString(), QString());
    }
    void refuteProfile(const QString &userId)
    {
        Q_EMIT client.userProfileFinished(client.lastProfileOp, false, userId,
                                          QString(), QString(),
                                          QStringLiteral("not_found"));
    }
};

QVariantMap directoryRow(const QString &userId, const QString &displayName)
{
    return { { QStringLiteral("userId"), userId },
             { QStringLiteral("displayName"), displayName },
             { QStringLiteral("avatarUrl"), QString() } };
}

QString sourceAt(const UserSearchModel &model, int row)
{
    return model.data(model.index(row, 0), UserSearchModel::SourceRole)
        .toString();
}

}

class UserLookupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void localpartFallbackDerivation()
    {
        using namespace matrix::user_lookup;
        QCOMPARE(localpartOrUserId(QStringLiteral("@matas:matrix.smetonis.net")),
                 QStringLiteral("matas"));
        QCOMPARE(localpartOrUserId(QStringLiteral("@a:x:8448")),
                 QStringLiteral("a"));
        QCOMPARE(localpartOrUserId(QStringLiteral("@:server")),
                 QStringLiteral("@:server"));
        QCOMPARE(localpartOrUserId(QStringLiteral("plain-name")),
                 QStringLiteral("plain-name"));
        QCOMPARE(localpartOrUserId(QString()), QString());
    }

    void candidateDerivation()
    {
        using namespace matrix::user_lookup;
        const QString server = QStringLiteral("example.org");

        QCOMPARE(exactCandidate(QStringLiteral("admin"), server),
                 QStringLiteral("@admin:example.org"));
        QCOMPARE(exactCandidate(QStringLiteral("@admin"), server),
                 QStringLiteral("@admin:example.org"));
        QCOMPARE(exactCandidate(QStringLiteral("admin:other.example"), server),
                 QStringLiteral("@admin:other.example"));
        QCOMPARE(exactCandidate(QStringLiteral("@admin:other.example"), server),
                 QStringLiteral("@admin:other.example"));
        QCOMPARE(exactCandidate(QStringLiteral("  admin  "), server),
                 QStringLiteral("@admin:example.org"));
        QCOMPARE(exactCandidate(QStringLiteral("@a:server.example:8448"), server),
                 QStringLiteral("@a:server.example:8448"));

        QVERIFY(exactCandidate(QStringLiteral("has space"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("bad!char"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("@"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("a:bad_server!"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("admin"), QString()).isEmpty());

        QCOMPARE(serverNameFromUserId(QStringLiteral("@me:example.org")),
                 QStringLiteral("example.org"));
        QVERIFY(serverNameFromUserId(QStringLiteral("nonsense")).isEmpty());

        QVERIFY(queryNamesServer(QStringLiteral("@a:b.org")));
        QVERIFY(queryNamesServer(QStringLiteral("a:b.org")));
        QVERIFY(!queryNamesServer(QStringLiteral("@admin")));
        QVERIFY(!queryNamesServer(QStringLiteral("admin")));
    }

    void bracketedIpv6ServerNamesAreExactCandidates()
    {
        using namespace matrix::user_lookup;
        const QString dns = QStringLiteral("example.org");
        QCOMPARE(exactCandidate(QStringLiteral("@alice:[2001:db8::1]"), dns),
                 QStringLiteral("@alice:[2001:db8::1]"));
        QCOMPARE(exactCandidate(QStringLiteral("@Alice:[2001:DB8::1]:8448"), dns),
                 QStringLiteral("@Alice:[2001:DB8::1]:8448"));
        QCOMPARE(exactCandidate(QStringLiteral("bob:[::ffff:192.0.2.1]"), dns),
                 QStringLiteral("@bob:[::ffff:192.0.2.1]"));
        QCOMPARE(exactCandidate(QStringLiteral("carol"), QStringLiteral("[::1]:8448")),
                 QStringLiteral("@carol:[::1]:8448"));
        QCOMPARE(serverNameFromUserId(QStringLiteral("@me:[::1]:8448")),
                 QStringLiteral("[::1]:8448"));

        const QStringList bad = {
            QStringLiteral("[2001:db8::1"), QStringLiteral("2001:db8::1"),
            QStringLiteral("[2001:db8::1]x"), QStringLiteral("[2001:db8::1]:"),
            QStringLiteral("[2001:db8::1]:65536"), QStringLiteral("[2001:db8::1]:84a"),
            QStringLiteral("[]"), QStringLiteral("[1::2::3]"), QStringLiteral("[12345::1]"),
            QStringLiteral("[1:2:3:4:5:6:7:8:9]"), QStringLiteral("[1:2:3:4:5:6:7]"),
            QStringLiteral("[::1.2.3.256]"), QStringLiteral("[::g]"),
            QStringLiteral("[ ::1]"), QStringLiteral("[fe80::1%eth0]"),
            QStringLiteral("example.org:"), QStringLiteral("example.org:99999"),
        };
        for (const QString &server : bad) {
            QVERIFY2(!isValidServerName(server), qPrintable(server));
            QVERIFY2(exactCandidate(QStringLiteral("@a:") + server, dns).isEmpty(),
                     qPrintable(server));
        }
        QVERIFY(!isValidServerName(QStringLiteral("[::1] ")));
        QVERIFY(isValidServerName(QStringLiteral("[1:2:3:4:5:6:7:8]")));
        QVERIFY(isValidServerName(QStringLiteral("[::]")));
        QVERIFY(isValidServerName(QStringLiteral("example.org:8448")));
        QVERIFY(isValidServerName(QStringLiteral("192.0.2.1:8448")));
    }

    void anIpv6IdWithNoDirectoryHitKeepsItsExplicitRow()
    {
        Rig rig;
        rig.search(QStringLiteral("@alice:[2001:db8::1]:8448"));
        QCOMPARE(rig.client.lastProfileUser,
                 QStringLiteral("@alice:[2001:db8::1]:8448"));
        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@alice:[2001:db8::1]:8448"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_mxid"));
    }

    void bareLocalpartUsesAccountServerAndNeedsConfirmation()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        QCOMPARE(rig.client.lastProfileUser, QStringLiteral("@admin:example.org"));

        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 0);
        QCOMPARE(rig.model.state(), QStringLiteral("loading"));

        rig.confirmProfile(QStringLiteral("@admin:example.org"),
                           QStringLiteral("Admin"));
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@admin:example.org"));
        QCOMPARE(rig.model.displayNameAt(0), QStringLiteral("Admin"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_local"));
        QCOMPARE(rig.model.state(), QStringLiteral("results"));
    }

    void refutedLocalpartIsNeverInvented()
    {
        Rig rig;
        rig.search(QStringLiteral("ghost"));
        rig.finishDirectory({});
        rig.refuteProfile(QStringLiteral("@ghost:example.org"));
        QCOMPARE(rig.model.rowCount(), 0);
        QCOMPARE(rig.model.state(), QStringLiteral("no_results"));
    }

    void typedFullIdIsOfferedImmediatelyAndEnriched()
    {
        Rig rig;
        rig.search(QStringLiteral("@carol:federated.example"));
        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@carol:federated.example"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_mxid"));
        QVERIFY(rig.model.displayNameAt(0).isEmpty());

        rig.confirmProfile(QStringLiteral("@carol:federated.example"),
                           QStringLiteral("Carol"));
        QCOMPARE(rig.model.displayNameAt(0), QStringLiteral("Carol"));
    }

    void localpartWithServerButNoAtIsExactMxid()
    {
        Rig rig;
        rig.search(QStringLiteral("admin:other.example"));
        QCOMPARE(rig.client.lastProfileUser, QStringLiteral("@admin:other.example"));
        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_mxid"));
    }

    void duplicateDirectoryAndExactResultIsMerged()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        rig.confirmProfile(QStringLiteral("@admin:example.org"), QString());
        rig.finishDirectory({ directoryRow(QStringLiteral("@admin:example.org"),
                                           QStringLiteral("Admin From Directory")),
                              directoryRow(QStringLiteral("@adminson:example.org"),
                                           QStringLiteral("Adminson")) });
        QCOMPARE(rig.model.rowCount(), 2);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@admin:example.org"));
        QCOMPARE(sourceAt(rig.model, 0), QStringLiteral("exact_local"));
        QCOMPARE(rig.model.displayNameAt(0), QStringLiteral("Admin From Directory"));
        QCOMPARE(sourceAt(rig.model, 1), QStringLiteral("directory"));
    }

    void invalidLocalpartDispatchesNoProfileLookup()
    {
        Rig rig;
        rig.search(QStringLiteral("has space"));
        QCOMPARE(rig.client.profileCalls, 0);
        rig.finishDirectory({});
        QCOMPARE(rig.model.rowCount(), 0);
        QCOMPARE(rig.model.state(), QStringLiteral("no_results"));
    }

    void staleProfileResultIsRejected()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        const quint64 firstProfileOp = rig.client.lastProfileOp;
        rig.search(QStringLiteral("bob"));

        Q_EMIT rig.client.userProfileFinished(firstProfileOp, true,
                                              QStringLiteral("@admin:example.org"),
                                              QStringLiteral("Admin"), QString(),
                                              QString());
        rig.finishDirectory({});
        for (int i = 0; i < rig.model.rowCount(); ++i)
            QVERIFY(rig.model.userIdAt(i) != QStringLiteral("@admin:example.org"));

        rig.confirmProfile(QStringLiteral("@bob:example.org"), QStringLiteral("Bob"));
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.userIdAt(0), QStringLiteral("@bob:example.org"));
    }

    void ownUserIsNeverACandidate()
    {
        Rig rig;
        rig.search(QStringLiteral("me"));
        QCOMPARE(rig.client.profileCalls, 0);
    }

    void failedDirectoryWithConfirmedExactStillShowsResult()
    {
        Rig rig;
        rig.search(QStringLiteral("admin"));
        rig.finishDirectory({}, false);
        rig.confirmProfile(QStringLiteral("@admin:example.org"),
                           QStringLiteral("Admin"));
        QCOMPARE(rig.model.rowCount(), 1);
        QCOMPARE(rig.model.state(), QStringLiteral("results"));
    }

    void mergeIsPureAndDeduplicates()
    {
        UserSearchModel::Result exact;
        exact.userId = QStringLiteral("@a:x.org");
        exact.source = QStringLiteral("exact_local");
        UserSearchModel::Result dupe;
        dupe.userId = QStringLiteral("@a:x.org");
        dupe.displayName = QStringLiteral("A");
        UserSearchModel::Result self;
        self.userId = QStringLiteral("@own:x.org");

        const auto merged = UserSearchModel::mergeResults(
            { exact }, { dupe, self }, QStringLiteral("@own:x.org"));
        QCOMPARE(merged.size(), 1);
        QCOMPARE(merged.first().source, QStringLiteral("exact_local"));
        QCOMPARE(merged.first().displayName, QStringLiteral("A"));
    }

    void aHistoricalFullIdIsAnExactCandidateButNeverFabricated()
    {
        using namespace matrix::user_lookup;
        const QString server = QStringLiteral("example.org");
        QCOMPARE(exactCandidate(QStringLiteral("@Ali'ce:old.example"), server),
                 QStringLiteral("@Ali'ce:old.example"));
        QCOMPARE(exactCandidate(QStringLiteral("@a!b:[::1]"), server),
                 QStringLiteral("@a!b:[::1]"));
        QVERIFY(exactCandidate(QStringLiteral("ali'ce"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("@é:old.example"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("@a\tb:old.example"), server).isEmpty());
        QVERIFY(exactCandidate(QStringLiteral("@") + QString(250, QLatin1Char('a'))
                                   + QStringLiteral(":old.example"), server).isEmpty());
    }

    void aLimitedDirectoryKeepsItsRowsAndSaysWhy()
    {
        Rig rig;
        rig.search(QStringLiteral("al"));
        QVariantList rows;
        for (int i = 0; i < 20; ++i)
            rows << directoryRow(QStringLiteral("@al%1:example.org").arg(i),
                                 QStringLiteral("Al %1").arg(i));
        rig.finishDirectoryAs(rows, true);
        QCOMPARE(rig.model.rowCount(), 20);
        QCOMPARE(rig.model.state(), QStringLiteral("results"));
        QVERIFY(rig.model.property("limited").toBool());
        QVERIFY(rig.model.property("notice").toString().contains(
            QStringLiteral("@name:example.org")));

        rig.search(QStringLiteral("alb"));
        rig.finishDirectoryAs({ directoryRow(QStringLiteral("@alb:example.org"),
                                             QStringLiteral("Alb")) }, false);
        QVERIFY(!rig.model.property("limited").toBool());
        QVERIFY(rig.model.property("notice").toString().isEmpty());
    }

    void searchFailuresAndEmptyAnswersSayWhatToTryNext()
    {
        Rig rig;
        rig.search(QStringLiteral("z z"));
        rig.finishDirectoryAs({}, false, QStringLiteral("rate_limited"));
        QCOMPARE(rig.model.state(), QStringLiteral("error"));
        QCOMPARE(rig.model.property("errorCategory").toString(),
                 QStringLiteral("rate_limited"));
        const QString wait = rig.model.property("notice").toString();
        QVERIFY(!wait.isEmpty());

        rig.search(QStringLiteral("z zz"));
        rig.finishDirectoryAs({}, false, QStringLiteral("unrecognized"));
        const QString unsupported = rig.model.property("notice").toString();
        QVERIFY(!unsupported.isEmpty());
        QVERIFY(unsupported != wait);

        rig.search(QStringLiteral("z zzz"));
        rig.finishDirectoryAs({}, false, QStringLiteral("network"));
        QCOMPARE(rig.model.property("errorCategory").toString(), QStringLiteral("network"));
        QVERIFY(rig.model.property("notice").toString().isEmpty());

        rig.search(QStringLiteral("nobody here"));
        rig.finishDirectoryAs({}, false);
        QCOMPARE(rig.model.state(), QStringLiteral("no_results"));
        const QString hint = rig.model.property("notice").toString();
        QVERIFY(hint.contains(QStringLiteral("@name:example.org")));

        rig.search(QStringLiteral("@a b:x.org"));
        rig.finishDirectoryAs({}, false);
        QCOMPARE(rig.model.state(), QStringLiteral("no_results"));
        const QString invalid = rig.model.property("notice").toString();
        QVERIFY(!invalid.isEmpty());
        QVERIFY(invalid != hint);

        rig.search(QStringLiteral("admin"));
        rig.finishDirectoryAs({}, false, QStringLiteral("network"));
        rig.confirmProfile(QStringLiteral("@admin:example.org"), QStringLiteral("Admin"));
        QCOMPARE(rig.model.state(), QStringLiteral("results"));
        QVERIFY(!rig.model.property("notice").toString().isEmpty());
    }

    void anUnconfirmedTypedIdIsMarkedUntilItsProfileAnswers()
    {
        Rig rig;
        const int role = rig.model.roleNames().key(QByteArrayLiteral("confirmed"), -1);
        QVERIFY(role >= 0);
        rig.search(QStringLiteral("@carol:federated.example"));
        rig.finishDirectory({ directoryRow(QStringLiteral("@carl:federated.example"),
                                           QStringLiteral("Carl")) });
        QCOMPARE(rig.model.rowCount(), 2);
        QVERIFY(!rig.model.data(rig.model.index(0, 0), role).toBool());
        QVERIFY(rig.model.data(rig.model.index(1, 0), role).toBool());
        rig.confirmProfile(QStringLiteral("@carol:federated.example"), QStringLiteral("Carol"));
        QVERIFY(rig.model.data(rig.model.index(0, 0), role).toBool());
    }

    void roomTargetsAreParsedLocally()
    {
        using namespace matrix::user_lookup;
        const struct {
            const char *text;
            const char *id;
            const char *event;
            bool link;
        } good[] = {
            { "!abc:example.org", "!abc:example.org", "", false },
            { " #room:example.org:8448 ", "#room:example.org:8448", "", false },
            { "!abc:[2001:db8::1]", "!abc:[2001:db8::1]", "", false },
            { "!AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA_", "!AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA_", "", false },
            { "https://matrix.to/#/%23room%3Aexample.org", "#room:example.org", "", true },
            { "matrix.to/#/!abc:example.org/%24ev?via=example.org", "!abc:example.org", "$ev", true },
            { "HTTPS://MATRIX.TO/#/#room:example.org", "#room:example.org", "", true },
            { "matrix:roomid/abc:example.org/e/ev?via=x", "!abc:example.org", "$ev", true },
            { "matrix:r/room:example.org", "#room:example.org", "", true },
        };
        for (const auto &c : good) {
            const RoomTarget t = parseRoomTarget(QString::fromUtf8(c.text));
            QVERIFY2(t.isValid(), c.text);
            QCOMPARE(t.id, QString::fromUtf8(c.id));
            QCOMPARE(t.eventId, QString::fromUtf8(c.event));
            QCOMPARE(t.isLink, c.link);
        }
        const char *bad[] = {
            "", "!", "#", "#room", "!short", "room:example.org", "@user:example.org",
            "#room:bad_server!", "!a b:example.org", "#room:[::1",
            "https://matrix.to/#/@user:example.org", "https://matrix.to/#/!a:x/notanevent",
            "matrix:u/user:example.org", "matrix:roomid/abc:example.org/x/ev",
            "https://example.org/#/!abc:example.org",
        };
        for (const char *text : bad)
            QVERIFY2(!parseRoomTarget(QString::fromUtf8(text)).isValid(), text);
    }
};

QTEST_GUILESS_MAIN(UserLookupTest)
#include "UserLookupTest.moc"
