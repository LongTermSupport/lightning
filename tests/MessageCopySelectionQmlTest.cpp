#include <QtTest/QtTest>

#include <memory>

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "matrix/MockMatrixClient.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
const QString kRoom = QStringLiteral("!general:mock.local");
const QString kEvent = QStringLiteral("$copysel");
const QString kBody = QStringLiteral("hello brave new world");
}

class MessageCopySelectionQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
    };

    static QVariantMap fixture(AppController &controller)
    {
        QVariantMap f;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            f.insert(QString::fromUtf8(it.value()), QVariant{});
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), kEvent);
        f.insert(QStringLiteral("itemId"), QStringLiteral("uid-copysel"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@alice:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Alice"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("A"));
        f.insert(QStringLiteral("body"), kBody);
        f.insert(QStringLiteral("eventType"), 0);
        f.insert(QStringLiteral("status"), 0);
        f.insert(QStringLiteral("isOwn"), false);
        f.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
        f.insert(QStringLiteral("redacted"), false);
        f.insert(QStringLiteral("edited"), false);
        f.insert(QStringLiteral("isEncrypted"), false);
        f.insert(QStringLiteral("isDecrypted"), false);
        f.insert(QStringLiteral("undecryptable"), false);
        f.insert(QStringLiteral("errorKind"), QString{});
        f.insert(QStringLiteral("isImage"), false);
        f.insert(QStringLiteral("isFile"), false);
        f.insert(QStringLiteral("isVideo"), false);
        f.insert(QStringLiteral("isAudio"), false);
        f.insert(QStringLiteral("isSticker"), false);
        f.insert(QStringLiteral("mediaIsVoice"), false);
        f.insert(QStringLiteral("mediaDurationMs"), 0);
        f.insert(QStringLiteral("mediaWidth"), 0);
        f.insert(QStringLiteral("mediaHeight"), 0);
        f.insert(QStringLiteral("mediaSize"), 0);
        f.insert(QStringLiteral("mediaSourceAvailable"), false);
        f.insert(QStringLiteral("mediaThumbAvailable"), false);
        f.insert(QStringLiteral("mediaKey"), QString{});
        f.insert(QStringLiteral("mediaFilename"), QString{});
        f.insert(QStringLiteral("mediaUrl"), QUrl{});
        f.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        f.insert(QStringLiteral("mediaMimetype"), QString{});
        f.insert(QStringLiteral("reactions"), QVariantList{});
        f.insert(QStringLiteral("replyToEventId"), QString{});
        f.insert(QStringLiteral("isThreadRoot"), false);
        f.insert(QStringLiteral("mentionsMe"), false);
        f.insert(QStringLiteral("mentionsRoom"), false);
        f.insert(QStringLiteral("isLocalEcho"), false);
        return f;
    }

    static bool seedTimeline(AppController &controller)
    {
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return false;
        controller.setCurrentRoomId(kRoom);
        TimelineEvent e;
        e.eventId = kEvent;
        e.itemId = QStringLiteral("uid-copysel");
        e.roomId = kRoom;
        e.sender = QStringLiteral("@alice:mock.local");
        e.senderDisplayName = QStringLiteral("Alice");
        e.body = kBody;
        e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-600);
        e.type = TimelineEvent::TextMessage;
        e.status = TimelineEvent::Sent;
        mock->resetTimelineForTest(kRoom, { e }, 0);
        return QTest::qWaitFor([&] {
            return !controller.timeline()->copyTextForEvent(kEvent).isEmpty();
        }, kSignalTimeoutMs);
    }

    bool createDelegate(AppController &controller, Delegate &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture(controller));
        QSignalSpy createdSpy(out.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(700, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(640);
        out.window->show();
        QTest::qWaitForWindowExposed(out.window.get());
        return true;
    }

    static QQuickItem *bodyOf(const Delegate &d)
    {
        QQuickItem *found = nullptr;
        QTest::qWaitFor([&] {
            found = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            return found != nullptr;
        }, kSignalTimeoutMs);
        return found;
    }

    static void rightClick(const Delegate &d, QQuickItem *body)
    {
        const QPointF p = body->mapToScene(
            QPointF(body->width() / 2, body->height() / 2));
        QTest::mouseClick(d.window.get(), Qt::RightButton, Qt::NoModifier,
                          p.toPoint());
    }

    static QObject *openMenu(const Delegate &d)
    {
        auto *menu = d.root->findChild<QObject *>(
            QStringLiteral("messageContextMenu"));
        if (!menu)
            return nullptr;
        QTest::qWaitFor([&] { return menu->property("opened").toBool(); },
                        kSignalTimeoutMs);
        return menu->property("opened").toBool() ? menu : nullptr;
    }

    static QObject *item(QObject *menu, const char *name)
    {
        return menu->findChild<QObject *>(QLatin1String(name));
    }

    static QString firstVisibleItem(QObject *menu)
    {
        QString best;
        qreal bestY = 1e12;
        const auto items = menu->findChildren<QQuickItem *>();
        for (QQuickItem *it : items) {
            if (!it->objectName().endsWith(QLatin1String("MenuItem"))
                    || !it->isVisible())
                continue;
            const qreal y = it->mapToScene(QPointF(0, 0)).y();
            if (y < bestY) {
                bestY = y;
                best = it->objectName();
            }
        }
        return best;
    }

    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("message-copy-selection-qml-test"));
    }

    void withASelectionTheMenuLeadsWithCopySelection()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(seedTimeline(controller));
        Delegate d;
        QVERIFY(createDelegate(controller, d));
        QQuickItem *body = bodyOf(d);
        QVERIFY(body);
        QVERIFY(QMetaObject::invokeMethod(body, "select", Q_ARG(int, 6),
                                          Q_ARG(int, 11)));
        QCOMPARE(body->property("selectedText").toString(),
                 QStringLiteral("brave"));
        rightClick(d, body);
        QObject *menu = openMenu(d);
        QVERIFY2(menu, "the context menu did not open on a right click");

        QObject *sel = item(menu, "copySelectionMenuItem");
        QVERIFY2(sel, "there is no Copy selection item");
        QVERIFY2(sel->property("visible").toBool(),
                 "Copy selection is hidden although text is selected");
        QCOMPARE(sel->property("text").toString(),
                 QStringLiteral("Copy selection"));
        QCOMPARE(firstVisibleItem(menu), QStringLiteral("copySelectionMenuItem"));
        QCOMPARE(sel->property("accel").toString(), QStringLiteral("Ctrl+C"));

        QObject *whole = item(menu, "copyTextMenuItem");
        QVERIFY(whole);
        QVERIFY2(whole->property("accel").toString().isEmpty(),
                 "Copy text still carries the Ctrl+C hint beside Copy selection");

        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));
        QMetaObject::invokeMethod(sel, "triggered");
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("brave"));
    }

    void withoutASelectionTheMenuIsUnchanged()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(seedTimeline(controller));
        Delegate d;
        QVERIFY(createDelegate(controller, d));
        QQuickItem *body = bodyOf(d);
        QVERIFY(body);
        rightClick(d, body);
        QObject *menu = openMenu(d);
        QVERIFY2(menu, "the context menu did not open on a right click");

        QObject *sel = item(menu, "copySelectionMenuItem");
        QVERIFY2(sel, "there is no Copy selection item");
        QVERIFY2(!sel->property("visible").toBool(),
                 "Copy selection is offered with nothing selected");
        QObject *whole = item(menu, "copyTextMenuItem");
        QVERIFY(whole);
        QCOMPARE(whole->property("text").toString(), QStringLiteral("Copy text"));
        QCOMPARE(whole->property("accel").toString(), QStringLiteral("Ctrl+C"));
        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));
        QMetaObject::invokeMethod(whole, "triggered");
        QCOMPARE(QGuiApplication::clipboard()->text(), kBody);
    }
};

QTEST_MAIN(MessageCopySelectionQmlTest)
#include "MessageCopySelectionQmlTest.moc"
