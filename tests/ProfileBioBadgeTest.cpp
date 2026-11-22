
#include "matrix/MockMatrixClient.h"
#include "profile/ProfileBadges.h"
#include "profile/ProfileBioManager.h"

#include <QSignalSpy>
#include <QtTest>

namespace {

struct RecordedFetch {
    QString userId;
    quint64 opId = 0;
};

class FakeBioClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsProfileBios() const override { return supports; }
    void fetchProfileBio(const QString &userId, quint64 opId) override
    {
        fetches.append({ userId, opId });
    }
    void setProfileBio(const QString &text, quint64 opId) override
    {
        writes.append(text);
        lastWriteOp = opId;
    }
    QString currentUserId() const override { return self; }

    bool supports = true;
    QString self = QStringLiteral("@me:example.org");
    QList<RecordedFetch> fetches;
    QStringList writes;
    quint64 lastWriteOp = 0;
};

const QString kAlice = QStringLiteral("@alice:example.org");

}

class ProfileBioBadgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void delayedBioReadsReserveCapacityForTheOpenedProfile()
    {
        FakeBioClient client;
        ProfileBioManager manager;
        manager.setClient(&client);
        const auto user = [](int i) { return QStringLiteral("@p%1:example.org").arg(i); };
        const auto answer = QStringLiteral("bio");
        const auto complete = [&](quint64 op, const QString &id) {
            Q_EMIT client.profileBioReceived(op, id, answer, true);
        };
        for (int i = 0; i < 250; ++i) {
            manager.request(user(i));
            complete(client.fetches.last().opId, user(i));
        }
        QList<QPair<quint64, QString>> delayed;
        for (int i = 250; i < 550; ++i) {
            const auto before = client.fetches.size();
            manager.request(user(i));
            if (client.fetches.size() != before)
                delayed.append({client.fetches.last().opId, user(i)});
        }
        QCOMPARE(delayed.size(), 16);
        manager.setOwnBio("new bio");
        Q_EMIT client.profileBioSet(client.lastWriteOp, true, answer, {});
        for (auto it = delayed.crbegin(); it != delayed.crend(); ++it)
            complete(it->first, it->second);
        const QString latest = user(549);
        QCOMPARE(client.fetches.last().userId, latest);
        complete(client.fetches.last().opId, latest);
        QVERIFY(!manager.bioFor(latest).isEmpty());
        QVERIFY(!manager.bioFor(client.self).isEmpty());
        int retained = 1;
        for (int i = 0; i < 550; ++i)
            retained += !manager.bioFor(user(i)).isEmpty();
        QVERIFY(retained <= 256);
        manager.request(user(600));
        const auto stale = client.fetches.last().opId;
        for (int i = 601; i < 640; ++i)
            manager.request(user(i));
        Q_EMIT client.loggedOut();
        const auto dispatched = client.fetches.size();
        manager.request(user(600));
        QCOMPARE(client.fetches.size(), dispatched + 1);
        complete(stale, user(600));
        QVERIFY(manager.bioFor(user(600)).isEmpty());
        complete(client.fetches.last().opId, user(600));
        QVERIFY(!manager.bioFor(user(600)).isEmpty());
    }

    void anUnknownUserRendersNothingAndIsAskedExactlyOnce()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        QVERIFY(bios.available());

        QCOMPARE(bios.bioFor(kAlice), QString());

        bios.request(kAlice);
        QCOMPARE(client.fetches.size(), 1);
        bios.request(kAlice);
        bios.request(kAlice);
        QCOMPARE(client.fetches.size(), 1);

        QSignalSpy revisions(&bios, &ProfileBioManager::revisionChanged);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QStringLiteral("hello\nworld"), true);
        QCOMPARE(bios.bioFor(kAlice), QStringLiteral("hello\nworld"));
        QCOMPARE(revisions.count(), 1);
    }

    // A user with no bio answers M_NOT_FOUND (supported == true, empty
    // string), which must not look like a server without extended profiles:
    // that hides the editing surface.
    void aFullCacheGivesUpItsOldestBioAndKeepsThisAccounts()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        bios.request(client.self);
        Q_EMIT client.profileBioReceived(client.fetches.last().opId, client.self,
                                         QStringLiteral("mine"), true);
        const auto user = [](int i) {
            return QStringLiteral("@u%1:example.org").arg(i);
        };
        for (int i = 0; i < 300; ++i) {
            bios.request(user(i));
            QCOMPARE(client.fetches.last().userId, user(i));
            Q_EMIT client.profileBioReceived(client.fetches.last().opId, user(i),
                                             QStringLiteral("bio"), true);
        }
        QCOMPARE(bios.bioFor(user(299)), QStringLiteral("bio"));
        QCOMPARE(bios.bioFor(user(0)), QString());
        QCOMPARE(bios.ownBio(), QStringLiteral("mine"));
        const int before = client.fetches.size();
        bios.request(user(0));
        QCOMPARE(client.fetches.size(), before + 1);
    }

    void anAbsentBioIsNotAnUnsupportedServer()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.request(kAlice);
        QSignalSpy supported(&bios, &ProfileBioManager::supportedChanged);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QString(), true);
        QCOMPARE(bios.bioFor(kAlice), QString());
        QVERIFY(bios.supported());
        QCOMPARE(supported.count(), 0);
        bios.request(QStringLiteral("@bob:example.org"));
        QCOMPARE(client.fetches.size(), 2);
    }

    void aServerWithoutExtendedProfilesLatchesAndStopsBeingAsked()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.request(kAlice);
        QSignalSpy supported(&bios, &ProfileBioManager::supportedChanged);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QString(), false);
        QVERIFY(!bios.supported());
        QCOMPARE(supported.count(), 1);

        bios.request(QStringLiteral("@bob:example.org"));
        bios.refresh(kAlice);
        QCOMPARE(client.fetches.size(), 1);
    }

    void anAnswerForAnUnknownOpIsDropped()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        bios.request(kAlice);

        Q_EMIT client.profileBioReceived(9999, kAlice,
                                         QStringLiteral("ghost"), true);
        QCOMPARE(bios.bioFor(kAlice), QString());
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QStringLiteral("real"), true);
        QCOMPARE(bios.bioFor(kAlice), QStringLiteral("real"));
    }

    void aWriteIsNeverAppliedOptimisticallyAndTakesTheStoredValue()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.setOwnBio(QStringLiteral("  typed  "));
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(bios.busy());
        QCOMPARE(bios.ownBio(), QString());
        bios.setOwnBio(QStringLiteral("again"));
        QCOMPARE(client.writes.size(), 1);

        Q_EMIT client.profileBioSet(client.lastWriteOp, true,
                                    QStringLiteral("typed"), QString());
        QVERIFY(!bios.busy());
        QCOMPARE(bios.ownBio(), QStringLiteral("typed"));
        QCOMPARE(bios.lastError(), QString());
    }

    void aRefusedWriteReportsAndChangesNothing()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);

        bios.setOwnBio(QStringLiteral("first"));
        Q_EMIT client.profileBioSet(client.lastWriteOp, true,
                                    QStringLiteral("first"), QString());
        QCOMPARE(bios.ownBio(), QStringLiteral("first"));

        bios.setOwnBio(QStringLiteral("second"));
        Q_EMIT client.profileBioSet(client.lastWriteOp, false, QString(),
                                    QStringLiteral("forbidden"));
        QCOMPARE(bios.ownBio(), QStringLiteral("first"));
        QCOMPARE(bios.lastError(), QStringLiteral("forbidden"));
        QVERIFY(bios.supported());

        bios.setOwnBio(QStringLiteral("third"));
        Q_EMIT client.profileBioSet(client.lastWriteOp, false, QString(),
                                    QStringLiteral("unsupported"));
        QVERIFY(!bios.supported());
    }

    void signingOutDropsEveryAnswer()
    {
        FakeBioClient client;
        ProfileBioManager bios;
        bios.setClient(&client);
        bios.request(kAlice);
        Q_EMIT client.profileBioReceived(client.fetches.first().opId, kAlice,
                                         QStringLiteral("mine"), true);
        QCOMPARE(bios.bioFor(kAlice), QStringLiteral("mine"));

        Q_EMIT client.loggedOut();
        QCOMPARE(bios.bioFor(kAlice), QString());
        bios.request(kAlice);
        QCOMPARE(client.fetches.size(), 2);
    }

    void aBackendWithoutBiosOffersNothing()
    {
        FakeBioClient client;
        client.supports = false;
        ProfileBioManager bios;
        bios.setClient(&client);
        QVERIFY(!bios.available());
        bios.request(kAlice);
        bios.setOwnBio(QStringLiteral("hello"));
        bios.clearOwnBio();
        QCOMPARE(client.fetches.size(), 0);
        QCOMPARE(client.writes.size(), 0);
    }

    void theBadgeIsATableAndAlmostEveryUserHasNone()
    {
        ProfileBadges badges;
        QVERIFY(!ProfileBadges::badges().isEmpty());
        for (const ProfileBadges::Badge &badge : ProfileBadges::badges()) {
            QVERIFY2(!badge.userId.isEmpty(), "a badge with no holder");
            QVERIFY2(badge.userId.startsWith(QLatin1Char('@')),
                     qPrintable(badge.userId));
            QVERIFY2(!badge.label.isEmpty(), qPrintable(badge.userId));
            QVERIFY2(!badge.description.isEmpty(), qPrintable(badge.userId));
            QVERIFY(badges.hasBadge(badge.userId));
            QCOMPARE(badges.labelFor(badge.userId), badge.label);
            QCOMPARE(badges.descriptionFor(badge.userId), badge.description);
            const QVariantMap map = badges.badgeFor(badge.userId);
            QCOMPARE(map.value(QStringLiteral("label")).toString(), badge.label);
            QCOMPARE(map.value(QStringLiteral("description")).toString(),
                     badge.description);
        }

        QVERIFY(!badges.hasBadge(kAlice));
        QCOMPARE(badges.labelFor(kAlice), QString());
        QCOMPARE(badges.descriptionFor(kAlice), QString());
        QVERIFY(badges.badgeFor(kAlice).isEmpty());
        QCOMPARE(badges.labelFor(QString()), QString());
    }

    void aBadgeIsNeverGivenToASimilarLookingId()
    {
        ProfileBadges badges;
        const QString holder = ProfileBadges::badges().first().userId;
        QVERIFY(badges.hasBadge(holder));
        QVERIFY(!badges.hasBadge(holder.toUpper()));
        QVERIFY(!badges.hasBadge(holder + QStringLiteral("x")));
        QVERIFY(!badges.hasBadge(holder.mid(1)));
        const int colon = holder.indexOf(QLatin1Char(':'));
        QVERIFY(colon > 0);
        QVERIFY(!badges.hasBadge(holder.left(colon)
                                 + QStringLiteral(":elsewhere.example")));
    }

    void theRecordedBadgeIsTheOneThatWasAwarded()
    {
        ProfileBadges badges;
        QCOMPARE(badges.labelFor(
                     QStringLiteral("@romanticanimegerl:cutefunny.art")),
                 QStringLiteral("idea master"));
    }

    void noBadgeDescriptionClaimsAPermissionOrAVerification()
    {
        for (const ProfileBadges::Badge &badge : ProfileBadges::badges()) {
            const QString lowered = badge.description.toLower();
            for (const char *claim : { "verified", "moderator", "admin" })
                QVERIFY2(!lowered.contains(QLatin1String(claim)),
                         qPrintable(badge.description));
            const QString label = badge.label.toLower();
            for (const char *banned : { "verified", "admin", "moderator",
                                        "official", "staff", "trusted" }) {
                QVERIFY2(!label.contains(QLatin1String(banned)),
                         qPrintable(badge.label));
            }
        }
    }
};

QTEST_GUILESS_MAIN(ProfileBioBadgeTest)
#include "ProfileBioBadgeTest.moc"
