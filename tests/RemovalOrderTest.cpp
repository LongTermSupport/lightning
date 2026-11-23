
#include "support/LabsAppHarness.h"

#include "accounts/HostRetirementPool.h"
#include "accounts/SessionPool.h"
#include "accounts/SessionTokenCustody.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "auth/AccountManager.h"
#include "models/AccountActivityModel.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "notifications/NotificationManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace labstest;

namespace {
QString markerOf(const matrix::app_data::AccountIdentity &identity)
{
    return identity.rustStorePath + QStringLiteral("/marker");
}

int stepIndex(const QStringList &steps, const QString &step)
{
    return int(steps.indexOf(step));
}

QVariantMap cardFor(const QString &uid)
{
    return QVariantMap{ { QStringLiteral("roomId"), QStringLiteral("!r:two.example") },
                        { QStringLiteral("eventId"), QStringLiteral("$e") },
                        { QStringLiteral("threadRootId"), QString() },
                        { QStringLiteral("accountUserId"), uid } };
}

bool anyErrorContains(const QSignalSpy &errors, const QString &needle)
{
    for (const QList<QVariant> &args : errors) {
        if (args.value(0).toString().contains(needle))
            return true;
    }
    return false;
}

QString sourceOf(const QString &relative)
{
    QFile file(QStringLiteral(SOURCE_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QString bodyOf(const QString &source, const QString &signature)
{
    const qsizetype start = source.indexOf(signature);
    if (start < 0)
        return QString();
    const qsizetype end = source.indexOf(QStringLiteral("\n}\n"), start);
    return source.mid(start, end < 0 ? -1 : end - start);
}

bool touch(const QString &path, const QByteArray &bytes = "fixture")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QStringList accountFilesUnder(const QString &root)
{
    return { matrix::app_data::bridgeLabelsFile(root),
             matrix::app_data::starredGifsDir(root) + QStringLiteral("/marker"),
             matrix::app_data::scrollPositionsFile(root) };
}

void signInAliceAt(AppController &app, const QString &storeSlug)
{
    SettingsManager *s = app.settings();
    s->saveSession(kHsOne, kAlice, QStringLiteral("ALICEDEV"),
                   QStringLiteral("alice-token-fixture"));
    s->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                   QStringLiteral("bob-token-fixture"));
    s->setStoreSlugFor(kAlice, storeSlug);
    app.switchToAccount(kAlice);
    QTRY_VERIFY(!app.accountSwitching());
    QTRY_VERIFY(app.loggedIn());
}
}

class RemovalOrderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        isolateSettings(m_configHome.path(), m_dataHome.path(),
                        "removal-order-test");
    }

    void init() { clearSettings(); }

    void aLiveBackgroundAccountIsSignedOutReleasedThenDeleted()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));

        const auto identity = identityOf(app, kBob);
        QVERIFY(identity.isValid());
        plantStoreFile(identity);
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        bool storeOnDiskAtSignOut = false;
        bridge.setLogoutHook([&storeOnDiskAtSignOut, identity](const QString &uid) {
            if (uid == kBob)
                storeOnDiskAtSignOut = QFileInfo::exists(markerOf(identity));
        });
        bridge.setRetireDelayMs(150);
        NotificationManager *notifications = app.notificationsForTest();
        notifications->recordPayloadForTest(kBob, 501, cardFor(kBob));
        QCOMPARE(notifications->pendingPayloadCountForTest(kBob), 1);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        QCOMPARE(notifications->pendingPayloadCountForTest(kBob), 0);
        QVERIFY2(app.settings()->hasSavedAccount(kBob),
                 "the record went before the account was signed out and "
                 "released");
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        {
            const QStringList steps = app.liveAccountsStepsForTest();
            const int forget = stepIndex(steps, QStringLiteral("removal:1-forget ") + kBob);
            const int resolve = stepIndex(steps, QStringLiteral("removal:2-identity ") + kBob);
            const int retire = stepIndex(steps, QStringLiteral("removal:3-retire ") + kBob);
            QVERIFY(forget >= 0);
            QVERIFY(forget < resolve);
            QVERIFY(resolve < retire);
            QVERIFY(!steps.contains(QStringLiteral("removal:5-delete ") + kBob));
        }

        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);

        QCOMPARE(bridge.callsFor(QStringLiteral("logout"), kBob)
                     + bridge.callsFor(QStringLiteral("oauthLogout"), kBob),
                 1);
        QVERIFY2(storeOnDiskAtSignOut,
                 "the store was deleted before the server sign-out");
        const int signOut = std::max(bridge.firstIndexOf(QStringLiteral("logout"), kBob),
                                     bridge.firstIndexOf(QStringLiteral("oauthLogout"), kBob));
        const int destroyed = bridge.firstIndexOf(QStringLiteral("destroyAndReport"), kBob);
        QVERIFY(signOut >= 0);
        QVERIFY2(destroyed > signOut, "retired before the server sign-out");
        QVERIFY(!QFileInfo::exists(markerOf(identity)));
        const QStringList steps = app.liveAccountsStepsForTest();
        const int released = stepIndex(steps, QStringLiteral("removal:4-released ") + kBob);
        const int deleted = stepIndex(steps, QStringLiteral("removal:5-delete ") + kBob);
        const int recordGone = stepIndex(steps, QStringLiteral("removal:6-record ") + kBob);
        QVERIFY(released >= 0);
        QVERIFY(released < deleted);
        QVERIFY(deleted < recordGone);
        QVERIFY(!anyErrorContains(errors, QStringLiteral("did not finish")));
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QVERIFY(app.loggedIn());
        QVERIFY(!app.sessionPoolForTest()->holdsStore(kBob));
    }

    void aStoreThatNeverClosesDeletesNothing()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        bridge.setStoreHeldForever(kBob, true);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        QTRY_VERIFY_WITH_TIMEOUT(
            anyErrorContains(errors, QStringLiteral("Removal did not finish")),
            30000);
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY(steps.contains(QStringLiteral("removal:4-still-held ") + kBob));
        QVERIFY(!steps.contains(QStringLiteral("removal:5-delete ") + kBob));
        QVERIFY(!steps.contains(QStringLiteral("removal:6-record ") + kBob));
        QVERIFY(app.accountActivity());
        QVERIFY2(!app.accountActivity()->isRemoved(kBob),
                 "a removal that did not finish left the badge row dead");
    }

    void aSecondClickWhileTheRemovalRunsDoesNothingMore()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        bridge.setRetireDelayMs(200);
        app.removeAccount(kBob);
        app.removeAccount(kBob);
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);
        QCOMPARE(bridge.callsFor(QStringLiteral("logout"), kBob)
                     + bridge.callsFor(QStringLiteral("oauthLogout"), kBob),
                 1);
        QCOMPARE(app.liveAccountsStepsForTest()
                     .filter(QStringLiteral("removal:1-forget"))
                     .size(),
                 1);
    }

    void aNonLiveAccountKeepsTodaysRemoval()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        NotificationManager *notifications = app.notificationsForTest();
        notifications->recordPayloadForTest(kCarol, 601, cardFor(kCarol));

        app.removeAccount(kCarol);

        QVERIFY(!app.settings()->hasSavedAccount(kCarol));
        QCOMPARE(notifications->pendingPayloadCountForTest(kCarol), 0);
        QCOMPARE(bridge.callsFor(QStringLiteral("logout"), kCarol), 0);
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("removal:not-live ") + kCarol));
        QVERIFY(app.sessionPoolForTest()->holdsStore(kBob));
    }

    void removalDeletesSavedPositionsUnderTheRecordedRoot()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        app.settings()->setStoreSlugFor(kCarol, QStringLiteral("Carol_typed"));
        const auto carol = identityOf(app, kCarol);
        QVERIFY(!carol.accountRoot.isEmpty());
        QVERIFY2(carol.accountRoot != matrix::app_data::accountRoot(kCarol),
                 "the fixture's recorded root must differ from the derived one");
        const QString positions =
            matrix::app_data::scrollPositionsFile(carol.accountRoot);
        QVERIFY(QDir().mkpath(carol.accountRoot));
        QFile file(positions);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"rooms":{"!r:three.example":{"e":"$e","o":0,"r":3}}})");
        file.close();

        app.removeAccount(kCarol);

        QVERIFY(!app.settings()->hasSavedAccount(kCarol));
        QVERIFY2(!QFile::exists(positions),
                 "removal left the saved positions under the recorded root");
    }

    // Removal deletes the recorded and the canonical root whole; sign-out
    // deletes nothing but this file, so it is where the path has to be right.
    void signOutDeletesSavedPositionsUnderTheRecordedRoot()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        app.settings()->setStoreSlugFor(kAlice, QStringLiteral("Alice_typed"));
        const auto alice = identityOf(app, kAlice);
        QVERIFY(!alice.accountRoot.isEmpty());
        QVERIFY2(alice.accountRoot != matrix::app_data::accountRoot(kAlice),
                 "the fixture's recorded root must differ from the derived one");
        const QString positions =
            matrix::app_data::scrollPositionsFile(alice.accountRoot);
        QVERIFY(QDir().mkpath(alice.accountRoot));
        QFile file(positions);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"rooms":{"!r:one.example":{"e":"$e","o":0,"r":3}}})");
        file.close();

        app.auth()->logout();

        QTRY_VERIFY2_WITH_TIMEOUT(
            !QFile::exists(positions),
            "sign-out left the saved positions under the recorded root", 5000);
    }

    void accountFilesOpenUnderTheRecordedRoot()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        signInAliceAt(app, QStringLiteral("Alice_open"));
        const auto alice = identityOf(app, kAlice);
        QVERIFY(alice.accountRoot != matrix::app_data::accountRoot(kAlice));
        QTRY_COMPARE(app.gif()->starredStore()->currentDirectory(),
                     matrix::app_data::starredGifsDir(alice.accountRoot));
    }

    void signOutDeletesAccountFilesUnderTheRecordedRootOnceTheRecordIsGone()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        signInAliceAt(app, QStringLiteral("Alice_gone"));
        const auto alice = identityOf(app, kAlice);
        QVERIFY(alice.accountRoot != matrix::app_data::accountRoot(kAlice));
        const QStringList files = accountFilesUnder(alice.accountRoot);
        for (const QString &file : files)
            QVERIFY(touch(file));

        // A Rust sign-out drops the record before loggedOut is delivered.
        QVERIFY(app.settings()->clearSessionForAccount(kAlice));
        app.auth()->logout();

        for (const QString &file : files) {
            QTRY_VERIFY2_WITH_TIMEOUT(!QFileInfo::exists(file),
                                      qPrintable(file), 5000);
        }
        QVERIFY(!QFileInfo::exists(alice.accountRoot));
    }

    void signInMovesAccountFilesFromTheDerivedRoot()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        const QStringList legacy =
            accountFilesUnder(matrix::app_data::accountRoot(kAlice));
        for (const QString &file : legacy)
            QVERIFY(touch(file, "{}"));
        QVERIFY(touch(legacy.at(1), "moved"));

        signInAliceAt(app, QStringLiteral("Alice_moved"));

        const auto alice = identityOf(app, kAlice);
        QVERIFY(alice.accountRoot != matrix::app_data::accountRoot(kAlice));
        const QStringList recorded = accountFilesUnder(alice.accountRoot);
        for (int i = 0; i < legacy.size(); ++i) {
            QVERIFY2(!QFileInfo::exists(legacy.at(i)), qPrintable(legacy.at(i)));
            QVERIFY2(QFileInfo::exists(recorded.at(i)), qPrintable(recorded.at(i)));
        }
        QFile marker(recorded.at(1));
        QVERIFY(marker.open(QIODevice::ReadOnly));
        QCOMPARE(marker.readAll(), QByteArray("moved"));
    }

    void signInLeavesAnotherAccountsRecordedRootAlone()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        SettingsManager *s = app.settings();
        s->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                       QStringLiteral("bob-token-fixture"));
        s->setStoreSlugFor(kBob, matrix::app_data::safeUserSlug(kAlice));
        const QString shared = matrix::app_data::accountRoot(kAlice);
        QCOMPARE(identityOf(app, kBob).accountRoot, shared);
        const QString bobsFile = matrix::app_data::bridgeLabelsFile(shared);
        QVERIFY(touch(bobsFile, "bob"));

        signInAliceAt(app, QStringLiteral("Alice_other"));
        app.auth()->logout();
        QTRY_VERIFY(!app.accountSwitching());

        QFile file(bobsFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("bob"));
    }

    void aNonLiveAccountWhoseSignInIsStillBeingWrittenTakesTheLiveRemoval()
    {
        namespace la = lightning::accounts;
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        la::SessionPool *pool = app.sessionPoolForTest();
        QTRY_COMPARE_WITH_TIMEOUT(int(pool->liveState(kBob)),
                                  int(AccountActivityModel::Running), kHostStartTimeoutMs);
        const QString device =
            app.settings()->accountRecord(kCarol).value(QStringLiteral("deviceId")).toString();
        QVERIFY(!device.isEmpty());
        la::HostSignInRetirements::process().parkUnsaved(la::HostSignInRetirements::UnsavedHandback{
            kCarol, device, la::custody::accessTokenDigest(QStringLiteral("carol-older")), {},
            la::SessionTokenPair{QStringLiteral("carol-parked"), QString()}});
        app.settings()->setKeepRunningFor(kCarol, true);
        QTRY_VERIFY_WITH_TIMEOUT((pool->admitNowForTest(), pool->holdsSignIn(kCarol)),
                                 kHostStartTimeoutMs);
        QVERIFY2(!pool->holdsStore(kCarol), "a host started: not the case under test");

        app.removeAccount(kCarol);

        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY2(!steps.contains(QStringLiteral("removal:not-live ") + kCarol),
                 "today's removal ran while a keyring write of the account was pending");
        QVERIFY(steps.contains(QStringLiteral("removal:3-retire ") + kCarol));
        QVERIFY2(app.settings()->hasSavedAccount(kCarol),
                 "the record went before the pending write answered");
        QVERIFY(!la::HostSignInRetirements::process().hasUnsaved(kCarol));
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kCarol), 20000);
        QVERIFY(!pool->holdsSignIn(kCarol));
        QVERIFY(!la::HostSignInRetirements::process().hasUnsaved(kCarol));
        QCOMPARE(bridge.callsFor(QStringLiteral("createBackground"), kCarol), 0);
    }

    void aRemovalCancelsAnOpenAndASwitchWaitingForIt()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        bridge.setRetireDelayMs(300);
        NotificationManager *notifications = app.notificationsForTest();
        Q_EMIT notifications->openRequestedForAccount(
            kBob, QStringLiteral("!general:mock.local"), QStringLiteral("$e"),
            QString());
        Q_EMIT notifications->openRequested(QStringLiteral("!general:mock.local"),
                                            QStringLiteral("$e"), QString());
        QVERIFY(app.hasParkedOpenForTest());
        QTRY_VERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("switch:release ") + kBob));

        app.removeAccount(kBob);
        QVERIFY(!app.hasParkedOpenForTest());
        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY(stepIndex(steps, QStringLiteral("open:cancelled ") + kBob)
                < stepIndex(steps, QStringLiteral("removal:2-identity ") + kBob));

        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);
        QTest::qWait(200);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
        QVERIFY(!app.liveAccountsStepsForTest().contains(
            QStringLiteral("switch:free ") + kBob));
    }

    void theLiveRemovalDeletesOnlyAfterTheRegistryAgrees()
    {
        const QString source = sourceOf(QStringLiteral("src/app/AppController.cpp"));
        QVERIFY(!source.isEmpty());
        const QString body = bodyOf(
            source, QStringLiteral("void AppController::finishLiveBackgroundRemoval("));
        QVERIFY(!body.isEmpty());
        QVERIFY(!body.contains(QStringLiteral("removeAccountLocalState(")));
        QVERIFY(!body.contains(QStringLiteral("waitForRustRetirement")));
        const qsizetype freeCheck = body.indexOf(QStringLiteral("if (!storeFree)"));
        const qsizetype remove = body.indexOf(
            QStringLiteral("removeAccountLocalStateFiles(identity, &refused)"));
        const qsizetype refusedCheck = body.indexOf(QStringLiteral("if (refused)"));
        const qsizetype record = body.indexOf(QStringLiteral("m_accounts->removeAccount("));
        QVERIFY(freeCheck >= 0);
        QVERIFY(remove > freeCheck);
        QVERIFY(refusedCheck > remove);
        QVERIFY(record > refusedCheck);

        const QString files = bodyOf(
            source, QStringLiteral("bool AppController::removeAccountLocalStateFiles("));
        QVERIFY(!files.isEmpty());
        const qsizetype claim = files.indexOf(QStringLiteral("claimForDeletion("));
        const qsizetype deleted = files.indexOf(QStringLiteral("removeAccountRustState("));
        QVERIFY(claim >= 0);
        QVERIFY(deleted > claim);
    }

    void aFailedServerSignOutStillRemovesAndSaysSo()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        bridge.setLogoutResult(QStringLiteral("failed"));
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 10000);
        QVERIFY(!QFileInfo::exists(markerOf(identity)));
        QVERIFY(anyErrorContains(errors, QStringLiteral("signing it out on the server did not finish")));
        QVERIFY(!anyErrorContains(errors, QStringLiteral("Removal did not finish")));
    }

    void aSwitchToAnAccountBeingRemovedIsRefused()
    {
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        bridge.setRetireDelayMs(300);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);
        app.switchToAccount(kBob);

        QVERIFY(anyErrorContains(errors, QStringLiteral("being removed")));
        QVERIFY(!app.accountSwitching());
        QVERIFY(!app.liveAccountsStepsForTest().contains(
            QStringLiteral("switch:release ") + kBob));
        QTRY_VERIFY_WITH_TIMEOUT(!app.settings()->hasSavedAccount(kBob), 20000);
        QCOMPARE(app.settings()->activeAccountUserId(), kAlice);
    }

    // ── The store registry enforcing (live accounts on a Rust build). The
    // mock backend never turns it on, so these cases do, directly. One-way
    // per process: they run LAST, and remove the records they add. ──

    void aRefusedStoreDeletionKeepsTheRecord()
    {
        auto &registry = lightning::accounts::StoreOwnershipRegistry::process();
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, {}));
        const auto identity = identityOf(app, kCarol);
        QVERIFY(identity.isValid());
        plantStoreFile(identity);
        registry.setEnforced(true);
        const quint64 holder = registry.recordOpen(
            identity.rustStorePath, identity.slug,
            lightning::accounts::StoreOwnershipRegistry::Role::Host);
        registry.noteReleasing(holder);
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kCarol);

        QVERIFY(app.settings()->hasSavedAccount(kCarol));
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        QVERIFY(anyErrorContains(errors, QStringLiteral("Removal did not finish")));
        QVERIFY(app.liveAccountsStepsForTest().contains(
            QStringLiteral("removal:refused ") + kCarol));
        QVERIFY(!app.accountActivity()->isRemoved(kCarol));

        registry.noteClosed(holder, true);
        app.removeAccount(kCarol);
        QVERIFY(!app.settings()->hasSavedAccount(kCarol));
        QVERIFY(!QFileInfo::exists(markerOf(identity)));
        registry.clearForTest();
    }

    void aLiveRemovalTheRegistryRefusesDeletesNothing()
    {
        auto &registry = lightning::accounts::StoreOwnershipRegistry::process();
        registry.setEnforced(true);
        ScriptedLabsBridge bridge;
        BridgeScope bridgeScope(&bridge);
        LabsEnvScope env(true);
        AppController app(AppController::MockBackend, false, inMemoryKeyring());
        saveAccountsAndSignInAlice(app);
        QVERIFY(startLiveAccounts(app, bridge, { kBob }));
        const auto identity = identityOf(app, kBob);
        plantStoreFile(identity);
        quint64 holder = 0;
        bridge.setLogoutHook([&holder, identity](const QString &uid) {
            if (uid == kBob && holder == 0) {
                holder = lightning::accounts::StoreOwnershipRegistry::process()
                             .recordOpen(identity.rustStorePath, identity.slug,
                                         lightning::accounts::StoreOwnershipRegistry::
                                             Role::Foreground);
            }
        });
        QSignalSpy errors(&app, &AppController::errorReported);

        app.removeAccount(kBob);

        QTRY_VERIFY_WITH_TIMEOUT(
            anyErrorContains(errors, QStringLiteral("Removal did not finish")), 20000);
        const QStringList steps = app.liveAccountsStepsForTest();
        QVERIFY(steps.contains(QStringLiteral("removal:4-released ") + kBob));
        QVERIFY(steps.contains(QStringLiteral("removal:5-refused ") + kBob));
        QVERIFY(!steps.contains(QStringLiteral("removal:6-record ") + kBob));
        QVERIFY(app.settings()->hasSavedAccount(kBob));
        QVERIFY(QFileInfo::exists(markerOf(identity)));
        QVERIFY(!app.accountActivity()->isRemoved(kBob));
        registry.clearForTest();
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(RemovalOrderTest)
#include "RemovalOrderTest.moc"
