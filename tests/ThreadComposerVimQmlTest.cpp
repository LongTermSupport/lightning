#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/VimKeymap.h"
#include "stickers/StickerPackManager.h"
#include "threads/ThreadController.h"

class ThreadComposerVimQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    static constexpr auto kDevRoom = "!dev:lightning.example";

    static QQuickItem *find(QQuickItem *root, const QString &name)
    {
        if (root->objectName() == name)
            return root;
        for (QQuickItem *child : root->childItems()) {
            if (QQuickItem *hit = find(child, name))
                return hit;
        }
        return nullptr;
    }
    QQuickItem *item(const char *name) const
    {
        return find(m_window->contentItem(), QString::fromLatin1(name));
    }
    MockMatrixClient *mock() const
    {
        return m_controller->findChild<MockMatrixClient *>();
    }
    VimKeymap *vim() const { return m_controller->composer()->threadVim(); }

    void type(const QString &keys)
    {
        for (const QChar c : keys)
            QTest::keyClick(m_window, c.toLatin1());
    }

    void openThreadPanel()
    {
        m_controller->openRoom(QString::fromLatin1(kDevRoom));
        QTRY_COMPARE(m_controller->currentRoomId(), QString::fromLatin1(kDevRoom));
        QTRY_VERIFY(m_controller->timeline()->realEventCount() > 2);
        QString root;
        for (const TimelineEvent &e : m_controller->timeline()->events()) {
            if (e.isThreadRoot) {
                root = e.eventId;
                break;
            }
        }
        QVERIFY(!root.isEmpty());
        m_controller->thread()->openThread(QString::fromLatin1(kDevRoom), root);
        QTRY_VERIFY(m_controller->thread()->active());
        QTRY_VERIFY(m_controller->thread()->model()->rowCount() > 2);
        QTest::qWait(300);
    }

    QStringList threadRoots() const
    {
        QStringList roots;
        for (const TimelineEvent &e : m_controller->timeline()->events()) {
            if (e.isThreadRoot)
                roots << e.eventId;
        }
        return roots;
    }

    static QVariantMap emoticonPack(const QString &id, const QString &url)
    {
        return QVariantMap{
            { QStringLiteral("id"), id },
            { QStringLiteral("displayName"), id },
            { QStringLiteral("source"), QStringLiteral("user") },
            { QStringLiteral("images"), QVariantList{ QVariantMap{
                  { QStringLiteral("shortcode"), QStringLiteral("lc_bl") },
                  { QStringLiteral("url"), url },
                  { QStringLiteral("body"), id },
                  { QStringLiteral("mimetype"), QStringLiteral("image/png") },
                  { QStringLiteral("isEmoticon"), true },
                  { QStringLiteral("isSticker"), false } } } },
        };
    }

    // Types ":lc_b" into the thread composer and accepts the popup row with `url`.
    void pickThreadEmoticon(const QString &url)
    {
        auto *editor = item("threadComposerInput");
        QVERIFY(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        type(QStringLiteral("x :lc_b"));
        auto *popup = m_window->findChild<QObject *>(
            QStringLiteral("threadEmojiCompletionPopup"));
        QVERIFY(popup);
        QTRY_VERIFY(popup->property("visible").toBool());
        QVariant value = popup->property("completions");
        if (value.userType() == qMetaTypeId<QJSValue>())
            value = value.value<QJSValue>().toVariant();
        const QVariantList rows = value.toList();
        int at = -1;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).toMap().value(QStringLiteral("url")).toString() == url)
                at = i;
        }
        QVERIFY2(at >= 0, qPrintable(QStringLiteral("no row for %1 in %2 rows")
                                         .arg(url).arg(rows.size())));
        while (popup->property("currentIndex").toInt() < at)
            QTest::keyClick(m_window, Qt::Key_Down);
        QCOMPARE(popup->property("currentIndex").toInt(), at);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(editor->property("text").toString(), QStringLiteral("x :lc_bl: "));
    }

    QString sentThreadEmoticonUrl() const
    {
        return mock()->lastThreadSendSpecForTest()
            .value(QStringLiteral("emoticons")).toMap()
            .value(QStringLiteral(":lc_bl:")).toMap()
            .value(QStringLiteral("url")).toString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + "/data").toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("thread-vim-test"));
    }

    void init()
    {
        QSettings().clear();
        m_controller = new AppController(AppController::MockBackend, true);
        QVERIFY(mock());
        mock()->setScreenshotDemoMode(true);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://lightning.example"),
                                    QStringLiteral("alex"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(4000));
        QTRY_VERIFY(m_controller->roomList()->rowCount() > 0);
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_controller);
        QQmlComponent scene(m_engine);
        scene.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 1500
    height: 900
    visible: true
    MainScreen { anchors.fill: parent }
}
)QML", QUrl(QStringLiteral("thread-vim-scene.qml")));
        QObject *root = scene.create();
        QVERIFY2(root, qPrintable(scene.errorString()));
        m_window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanup()
    {
        delete m_window;
        delete m_engine;
        delete m_controller;
        m_window = nullptr;
        m_engine = nullptr;
        m_controller = nullptr;
    }

    void threadComposerEntersNormalModeAndSends()
    {
        m_controller->settings()->setVimComposer(true);
        openThreadPanel();
        auto *editor = item("threadComposerInput");
        auto *badge = item("threadVimMode");
        auto *label = item("threadVimModeLabel");
        QVERIFY(editor && badge && label);
        QTRY_VERIFY(badge->isVisible());
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        QCOMPARE(label->property("text").toString(), QStringLiteral("INSERT"));

        type(QStringLiteral("hello world"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello world"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(vim()->normal());
        QVERIFY(m_controller->thread()->active());
        QCOMPARE(label->property("text").toString(), QStringLiteral("NORMAL"));
        QVERIFY(editor->property("readOnly").toBool());
        QVERIFY(!editor->inputMethodQuery(Qt::ImEnabled).toBool());
        QVERIFY(!m_controller->composer()->vim()->normal());

        type(QStringLiteral("qz"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello world"));
        type(QStringLiteral("bdw"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello "));
        type(QStringLiteral("u"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello world"));

        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(mock()->lastThreadSendBodyForTest(), QStringLiteral("hello world"));
        QTRY_COMPARE(editor->property("text").toString(), QString());
        QVERIFY(!vim()->normal());
        QVERIFY(m_controller->thread()->active());
    }

    void theModeBadgeTakesNoWidthFromTheTextField()
    {
        openThreadPanel();
        auto *editor = item("threadComposerInput");
        auto *badge = item("threadVimMode");
        QVERIFY(editor && badge);
        QTRY_VERIFY(editor->width() > 0);
        const qreal widthOff = editor->width();

        m_controller->settings()->setVimComposer(true);
        QTRY_VERIFY(badge->isVisible());
        QTest::qWait(100);
        QCOMPARE(editor->width(), widthOff);

        const QRectF b = badge->mapRectToScene(
            QRectF(0, 0, badge->width(), badge->height()));
        const QRectF e = editor->mapRectToScene(
            QRectF(0, 0, editor->width(), editor->height()));
        QVERIFY2(!b.intersects(e),
                 qPrintable(QStringLiteral("badge %1,%2 %3x%4 overlaps the field %5,%6 %7x%8")
                                .arg(b.x()).arg(b.y()).arg(b.width()).arg(b.height())
                                .arg(e.x()).arg(e.y()).arg(e.width()).arg(e.height())));
    }

    void withVimOffEscapeStillClosesThePanel()
    {
        openThreadPanel();
        auto *editor = item("threadComposerInput");
        QVERIFY(editor);
        auto *badge = item("threadVimMode");
        QVERIFY(badge && !badge->isVisible());
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        type(QStringLiteral("ab"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(!vim()->normal());
        QTRY_VERIFY(!m_controller->thread()->active());
    }
    void eachThreadDraftSendsTheCustomEmojiPickedInIt()
    {
        m_controller->settings()->setComposerMode(QStringLiteral("plain"));
        openThreadPanel();
        const QStringList roots = threadRoots();
        QVERIFY2(roots.size() >= 2, "the demo room needs two thread roots");
        const QString packA = QStringLiteral("mxc://lightning.example/packA");
        const QString packB = QStringLiteral("mxc://lightning.example/packB");
        m_controller->stickers()->applySnapshotForTest(
            QString::fromLatin1(kDevRoom), false,
            { emoticonPack(QStringLiteral("packA"), packA),
              emoticonPack(QStringLiteral("packB"), packB) });
        auto *thread = m_controller->thread();
        const QString room = QString::fromLatin1(kDevRoom);

        thread->openThread(room, roots.at(0));
        QTRY_VERIFY(thread->active());
        QTRY_COMPARE(thread->rootEventId(), roots.at(0));
        pickThreadEmoticon(packB);
        thread->openThread(room, roots.at(1));
        QTRY_COMPARE(thread->rootEventId(), roots.at(1));
        QTRY_COMPARE(item("threadComposerInput")->property("text").toString(), QString());
        pickThreadEmoticon(packA);

        thread->openThread(room, roots.at(0));
        QTRY_COMPARE(thread->rootEventId(), roots.at(0));
        QTRY_VERIFY(thread->state() == ThreadController::Ready);
        auto *editor = item("threadComposerInput");
        QTRY_COMPARE(editor->property("text").toString(), QStringLiteral("x :lc_bl: "));
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(mock()->lastThreadSendRootForTest(), roots.at(0));
        QCOMPARE(sentThreadEmoticonUrl(), packB);

        thread->openThread(room, roots.at(1));
        QTRY_COMPARE(thread->rootEventId(), roots.at(1));
        QTRY_VERIFY(thread->state() == ThreadController::Ready);
        QTRY_COMPARE(editor->property("text").toString(), QStringLiteral("x :lc_bl: "));
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(mock()->lastThreadSendRootForTest(), roots.at(1));
        QCOMPARE(sentThreadEmoticonUrl(), packA);
    }
};

QTEST_MAIN(ThreadComposerVimQmlTest)
#include "ThreadComposerVimQmlTest.moc"
