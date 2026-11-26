// "Remove recent messages" through the real profile card and member list
// menus, against a real AppController(MockBackend).

#include <QtTest/QtTest>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/OpenRoomPermissions.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/TimelineModel.h"
#include "ZoneDataForTests.h"

namespace {

constexpr int kTimeoutMs = 10000;
const QString kRoom = QStringLiteral("!general:mock.local");
const QString kName = QStringLiteral("Spam <b>Bot</b>");

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1000
    height: 760
    visible: true
    color: AppTheme.background
    property alias panel: info
    property alias card: card
    property alias bar: bar

    RoomInfoPanel {
        id: info
        width: 360
        height: 700
    }
    MemberProfilePopover {
        id: card
        parent: Overlay.overlay
    }
    BulkRedactionBar {
        id: bar
        x: 380
        y: 700
        width: 600
        timelineModel: app.timeline
    }
}
)QML";

}

class RemoveRecentMessagesQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_app = nullptr;
    MockMatrixClient *m_mock = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    int m_serial = 0;

    QObject *find(const QString &name) const
    {
        return m_root->findChild<QObject *>(name);
    }
    QObject *inCard(const QString &name) const
    {
        QObject *c = card();
        return c ? c->findChild<QObject *>(name) : nullptr;
    }
    QObject *inPanel(const QString &name) const
    {
        QObject *p = panel();
        return p ? p->findChild<QObject *>(name) : nullptr;
    }
    // Both popovers host a dialog; whichever is open is the one in use.
    QObject *dialog() const
    {
        const auto all = m_root->findChildren<QObject *>(
            QStringLiteral("removeRecentDialog"));
        for (QObject *d : all) {
            if (d->property("visible").toBool())
                return d;
        }
        return nullptr;
    }
    QObject *inDialog(const QString &name) const
    {
        QObject *d = dialog();
        return d ? d->findChild<QObject *>(name) : nullptr;
    }
    QObject *bulk() const { return m_app->bulkRedaction(); }
    QObject *card() const
    {
        return QQmlProperty::read(m_root, QStringLiteral("card")).value<QObject *>();
    }
    QObject *panel() const
    {
        return QQmlProperty::read(m_root, QStringLiteral("panel")).value<QObject *>();
    }

    void setGrant(bool canRedactOthers)
    {
        QVariantMap grant;
        grant.insert(QStringLiteral("ownMembership"), QStringLiteral("joined"));
        grant.insert(QStringLiteral("canSendMessages"), true);
        grant.insert(QStringLiteral("canRedactOthers"), canRedactOthers);
        m_mock->setOwnPermissionsForTest(kRoom, grant);
        auto *facts = m_app->openRoomPermissions();
        QTRY_VERIFY_WITH_TIMEOUT(facts->redactKnown()
                                     && facts->canRedactOthers() == canRedactOthers,
                                 kTimeoutMs);
    }

    struct Seeded {
        QString userId;
        QStringList messages;
        QStringList state;
        QString alreadyRedacted;
    };

    Seeded seed(int messageCount = 5)
    {
        Seeded out;
        out.userId = QStringLiteral("@spam%1:mock.local").arg(++m_serial);
        const auto add = [&](TimelineEvent::Type type, bool redacted) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$purge-%1-%2")
                            .arg(m_serial)
                            .arg(out.messages.size() + out.state.size() + 10 * redacted);
            e.sender = out.userId;
            e.senderDisplayName = kName;
            e.type = type;
            e.body = type == TimelineEvent::StateChange ? QString()
                                                        : QStringLiteral("buy now");
            e.redacted = redacted;
            e.timestamp = QDateTime::currentDateTimeUtc();
            m_mock->appendEventForTest(kRoom, e);
            return e.eventId;
        };
        out.state.append(add(TimelineEvent::StateChange, false));
        for (int i = 0; i < messageCount; ++i)
            out.messages.append(add(TimelineEvent::TextMessage, false));
        out.state.append(add(TimelineEvent::StateChange, false));
        out.alreadyRedacted = add(TimelineEvent::TextMessage, true);
        TimelineModel *model = m_app->timeline();
        Q_UNUSED(model);
        for (int i = 0; i < 250 && !modelHas(out.messages.last()); ++i)
            QTest::qWait(20);
        return out;
    }

    bool modelHas(const QString &eventId) const
    {
        for (const TimelineEvent &e : m_app->timeline()->events()) {
            if (e.eventId == eventId)
                return true;
        }
        return false;
    }

    bool redactedInModel(const QString &eventId) const
    {
        for (const TimelineEvent &e : m_app->timeline()->events()) {
            if (e.eventId == eventId)
                return e.redacted;
        }
        return false;
    }

    QVariantMap member(const QString &userId) const
    {
        QVariantMap m;
        m.insert(QStringLiteral("userId"), userId);
        m.insert(QStringLiteral("displayName"), kName);
        return m;
    }

    // Opens the profile card and its overflow menu; returns the menu item.
    QObject *cardMenuItem(const QString &userId)
    {
        QObject *c = card();
        if (!c || !QMetaObject::invokeMethod(c, "openFor",
                                             Q_ARG(QVariant, QVariant(member(userId))),
                                             Q_ARG(QVariant, QVariant())))
            return nullptr;
        QObject *overflow = inCard(QStringLiteral("profileOverflowButton"));
        if (!overflow || !QMetaObject::invokeMethod(overflow, "clicked"))
            return nullptr;
        QCoreApplication::processEvents();
        return inCard(QStringLiteral("profileRemoveRecentButton"));
    }

    QObject *openDialogFromCard(const QString &userId)
    {
        QObject *item = cardMenuItem(userId);
        if (!item || !item->property("visible").toBool())
            return nullptr;
        QMetaObject::invokeMethod(item, "triggered");
        for (int i = 0; i < 100 && !dialog(); ++i)
            QTest::qWait(20);
        return dialog();
    }

    QString question() const
    {
        QObject *label = inDialog(QStringLiteral("removeRecentQuestion"));
        return label ? label->property("text").toString() : QString();
    }

    QString status() const
    {
        QObject *label = find(QStringLiteral("bulkRedactionStatus"));
        return label ? label->property("text").toString() : QString();
    }

    void confirm()
    {
        QObject *button = inDialog(QStringLiteral("removeRecentConfirm"));
        QVERIFY(button);
        QTRY_VERIFY_WITH_TIMEOUT(button->property("enabled").toBool(), kTimeoutMs);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    }

    void dismissBar()
    {
        if (QObject *cancel = find(QStringLiteral("bulkRedactionCancel")))
            QMetaObject::invokeMethod(cancel, "clicked");
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("running").toBool(), kTimeoutMs);
        QMetaObject::invokeMethod(bulk(), "cancel");
    }

private slots:
    void initTestCase()
    {
        ensureZoneDataForTests();
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("remove-recent-messages-qml-test"));
        QSettings().clear();
        m_app = new AppController(AppController::MockBackend);
        m_mock = m_app->findChild<MockMatrixClient *>();
        QVERIFY(m_mock);
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_app);
        QSignalSpy loginSpy(m_app->auth(), &AuthManager::loginSucceeded);
        m_app->auth()->login(QStringLiteral("https://mock.local"),
                             QStringLiteral("alice"),
                             QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kTimeoutMs));
        QTRY_VERIFY(m_app->loggedIn());
        m_app->setCurrentRoomId(kRoom);
        QTRY_COMPARE_WITH_TIMEOUT(m_app->timeline()->roomId(), kRoom, kTimeoutMs);

        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("scene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_app;
    }

    void cleanup()
    {
        if (QObject *open = dialog())
            QMetaObject::invokeMethod(open, "close");
        if (QObject *c = card())
            QMetaObject::invokeMethod(c, "close");
        m_mock->setUserWalkPageSizeForTest(100);
        m_mock->setHoldRedactionsForTest(false);
        dismissBar();
    }

    void hiddenWithoutPowerShownWithIt()
    {
        const Seeded s = seed();
        setGrant(false);
        QObject *item = cardMenuItem(s.userId);
        QVERIFY2(item, "the profile card has no Remove recent messages item");
        QVERIFY2(!item->property("visible").toBool(),
                 "offered to a viewer who cannot redact other people's events");
        QVERIFY(QMetaObject::invokeMethod(panel(), "openMemberMenu",
                                          Q_ARG(QVariant, QVariant(member(s.userId))),
                                          Q_ARG(QVariant, QVariant::fromValue(panel())),
                                          Q_ARG(QVariant, QVariant(0)),
                                          Q_ARG(QVariant, QVariant(0))));
        QObject *listItem = inPanel(QStringLiteral("roomMemberMenuRemoveRecent"));
        QVERIFY2(listItem, "the member list menu has no Remove recent messages item");
        QVERIFY(!listItem->property("visible").toBool());

        setGrant(true);
        QVERIFY(QMetaObject::invokeMethod(panel(), "openMemberMenu",
                                          Q_ARG(QVariant, QVariant(member(s.userId))),
                                          Q_ARG(QVariant, QVariant::fromValue(panel())),
                                          Q_ARG(QVariant, QVariant(0)),
                                          Q_ARG(QVariant, QVariant(0))));
        QVERIFY2(listItem->property("visible").toBool(),
                 "the member list hides it from a moderator");
        QVERIFY(QMetaObject::invokeMethod(listItem, "triggered"));
        QVERIFY2(bulk()->property("purge").toBool(),
                 "the member list's item did not start a count");
        QTRY_VERIFY2_WITH_TIMEOUT(dialog() != nullptr,
                                  qPrintable(QStringLiteral("dialogs: %1, in panel: %2")
                                      .arg(m_root->findChildren<QObject *>(
                                               QStringLiteral("removeRecentDialog")).size())
                                      .arg(inPanel(QStringLiteral("removeRecentDialog")) != nullptr)),
                                  kTimeoutMs);
        QVERIFY2(inPanel(QStringLiteral("removeRecentDialog"))->property("visible").toBool(),
                 "the member list opened some other dialog");
        QMetaObject::invokeMethod(dialog(), "close");
        QTRY_VERIFY(dialog() == nullptr);

        item = cardMenuItem(s.userId);
        QVERIFY(item);
        QVERIFY2(item->property("visible").toBool(),
                 "the profile card hides it from a moderator");
    }

    void countsAndSaysAtLeastWhenTheWalkIsCapped()
    {
        setGrant(true);
        const Seeded s = seed();
        const int callsBefore = m_mock->redactionCallsForTest();
        QObject *dlg = openDialogFromCard(s.userId);
        QVERIFY2(dlg, "the card's menu did not open the confirmation");
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        QVERIFY2(question().startsWith(QStringLiteral("Remove 5 recent message")),
                 qPrintable(question()));
        QVERIFY2(question().endsWith(QStringLiteral(" from ") + kName + QLatin1Char('?')),
                 qPrintable(question()));
        QObject *label = inDialog(QStringLiteral("removeRecentQuestion"));
        QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));

        QObject *keep = inDialog(QStringLiteral("removeRecentKeepState"));
        QVERIFY(keep);
        QVERIFY2(keep->property("checked").toBool(), "state events are not kept by default");
        QVERIFY(QMetaObject::invokeMethod(keep, "toggle"));
        QVERIFY(QMetaObject::invokeMethod(keep, "toggled"));
        QTRY_VERIFY2(question().startsWith(QStringLiteral("Remove 7 recent message")),
                     qPrintable(question()));
        QVERIFY(QMetaObject::invokeMethod(dlg, "close"));
        QTRY_VERIFY(!dlg->property("visible").toBool());
        QCOMPARE(m_mock->redactionCallsForTest(), callsBefore);
        QVERIFY(!bulk()->property("running").toBool());
        QVERIFY2(!bulk()->property("purge").toBool(), "closing left a purge armed");

        // Ten pages of one event: the walk stops well short of the room's start.
        m_mock->setUserWalkPageSizeForTest(1);
        dlg = openDialogFromCard(s.userId);
        QVERIFY(dlg);
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        QVERIFY2(question().startsWith(QStringLiteral("Remove at least 5 recent message")),
                 qPrintable(question()));
        QVERIFY2(keep->property("checked").toBool(), "reopening kept the last choice");
    }

    void keepStateEventsByDefaultRemovesOnlyMessages()
    {
        setGrant(true);
        const Seeded s = seed();
        QVERIFY(openDialogFromCard(s.userId));
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        QObject *reason = inDialog(QStringLiteral("removeRecentReason"));
        QVERIFY(reason);
        reason->setProperty("text", QStringLiteral("spam wave"));
        confirm();
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("running").toBool(), kTimeoutMs);
        for (const QString &id : s.messages)
            QVERIFY2(redactedInModel(id), qPrintable(id));
        for (const QString &id : s.state)
            QVERIFY2(!redactedInModel(id), "a state event was removed by default");
        QCOMPARE(bulk()->property("total").toInt(), 5);
        QCOMPARE(bulk()->property("failed").toInt(), 0);
        QCOMPARE(m_mock->lastRedactionReasonForTest(), QStringLiteral("spam wave"));
        QCOMPARE(status(), QStringLiteral("Removed 5 of 5."));
        QVERIFY2(!find(QStringLiteral("bulkRedactionLookFurther"))->property("visible").toBool(),
                 "offered to look further back after a walk that reached the room's start");
    }

    void aCappedWalkOffersToLookFurtherBack()
    {
        setGrant(true);
        const Seeded s = seed();
        m_mock->setUserWalkPageSizeForTest(1);
        QVERIFY(openDialogFromCard(s.userId));
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        QVERIFY2(question().startsWith(QStringLiteral("Remove at least 5 recent message")),
                 qPrintable(question()));
        QVERIFY(m_mock->lastUserWalkFromForTest().isEmpty());
        confirm();
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("running").toBool(), kTimeoutMs);
        QCOMPARE(status(), QStringLiteral("Removed 5 of 5."));
        QObject *further = find(QStringLiteral("bulkRedactionLookFurther"));
        QVERIFY(further);
        QVERIFY2(further->property("visible").toBool(),
                 "a capped walk does not offer to look further back");

        const int walks = m_mock->userWalksForTest();
        const int calls = m_mock->redactionCallsForTest();
        QVERIFY(QMetaObject::invokeMethod(further, "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(dialog() != nullptr, kTimeoutMs);
        QCOMPARE(m_mock->userWalksForTest(), walks + 1);
        QVERIFY2(m_mock->lastUserWalkFromForTest().startsWith(QStringLiteral("mock:")),
                 "the further walk restarted from the live end");
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        QVERIFY2(question().contains(kName), qPrintable(question()));
        QCOMPARE(m_mock->redactionCallsForTest(), calls);
        QMetaObject::invokeMethod(dialog(), "close");
        QTRY_VERIFY(dialog() == nullptr);
        QCOMPARE(m_mock->redactionCallsForTest(), calls);
    }

    void cancelStopsAfterTheInFlightOne()
    {
        setGrant(true);
        const Seeded s = seed();
        QVERIFY(openDialogFromCard(s.userId));
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        m_mock->setHoldRedactionsForTest(true);
        const int callsBefore = m_mock->redactionCallsForTest();
        confirm();
        QTRY_COMPARE_WITH_TIMEOUT(m_mock->redactionCallsForTest(), callsBefore + 1, kTimeoutMs);
        QVERIFY(bulk()->property("running").toBool());
        QCOMPARE(status(), QStringLiteral("Removed 0 of 5"));

        QObject *cancel = find(QStringLiteral("bulkRedactionCancel"));
        QVERIFY(cancel);
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QVERIFY2(bulk()->property("running").toBool(),
                 "cancel abandoned the request already on the wire");
        m_mock->releaseHeldRedactionForTest();
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("running").toBool(), kTimeoutMs);
        QTest::qWait(700);
        QCOMPARE(m_mock->redactionCallsForTest(), callsBefore + 1);
        QCOMPARE(bulk()->property("completed").toInt(), 1);
        QVERIFY(redactedInModel(s.messages.first()));
        QVERIFY(!redactedInModel(s.messages.at(1)));
        QCOMPARE(status(), QStringLiteral("Stopped. Removed 1 of 5."));
    }

    void aPartialFailureIsReportedAsPartial()
    {
        setGrant(true);
        const Seeded s = seed();
        m_mock->failRedactionForTest(s.messages.at(1), QStringLiteral("forbidden"));
        m_mock->failRedactionForTest(s.messages.at(3), QStringLiteral("network"));
        QVERIFY(openDialogFromCard(s.userId));
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        confirm();
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("running").toBool(), kTimeoutMs);
        QCOMPARE(bulk()->property("failed").toInt(), 2);
        QCOMPARE(status(), QStringLiteral("Removed 3 of 5; 2 couldn't be removed."));
    }

    void powerLostAfterConfirmationIsRefusedAtExecution()
    {
        setGrant(true);
        const Seeded s = seed();
        QVERIFY(openDialogFromCard(s.userId));
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("purgeCounting").toBool(), kTimeoutMs);
        setGrant(false);
        confirm();
        QTRY_VERIFY_WITH_TIMEOUT(!bulk()->property("running").toBool(), kTimeoutMs);
        for (const QString &id : s.messages)
            QVERIFY(!redactedInModel(id));
        QCOMPARE(status(), QStringLiteral("Removed 0 of 5; 5 couldn't be removed."));
    }
};

QTEST_MAIN(RemoveRecentMessagesQmlTest)
#include "RemoveRecentMessagesQmlTest.moc"
