#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
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

    // An existing thread root, then a plain message to "Reply in thread" on.
    QStringList threadRoots() const
    {
        QString root;
        QString plain;
        for (const TimelineEvent &e : m_controller->timeline()->events()) {
            if (e.isThreadRoot && root.isEmpty())
                root = e.eventId;
            else if (!e.isThreadRoot && plain.isEmpty() && e.threadRootId.isEmpty()
                     && e.type == TimelineEvent::TextMessage && e.eventId.startsWith(u'$'))
                plain = e.eventId;
        }
        QStringList roots;
        if (!root.isEmpty() && !plain.isEmpty())
            roots << root << plain;
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

    void restoreDraftText(QQuickItem *editor)
    {
        if (editor->property("text").toString().isEmpty())
            editor->setProperty("text", QStringLiteral("x :lc_bl: "));
        QTRY_COMPARE(m_controller->thread()->text(), QStringLiteral("x :lc_bl: "));
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
        QVERIFY2(roots.size() >= 2, "the demo room needs a thread root and a plain message");
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
        // No saved account here, so unencrypted-room drafts are not kept: put the text back.
        restoreDraftText(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(mock()->lastThreadSendRootForTest(), roots.at(0));
        QCOMPARE(sentThreadEmoticonUrl(), packB);

        thread->openThread(room, roots.at(1));
        QTRY_COMPARE(thread->rootEventId(), roots.at(1));
        QTRY_VERIFY(thread->state() == ThreadController::Ready);
        restoreDraftText(editor);
        editor->forceActiveFocus();
        QTRY_VERIFY(editor->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(mock()->lastThreadSendRootForTest(), roots.at(1));
        QCOMPARE(sentThreadEmoticonUrl(), packA);
    }

    // The thread "+" menu is the only UI path to a thread poll; it must reach
    // the thread send path with the root, never the room composer.
    void threadPlusMenuSendsAPollIntoTheThread()
    {
        openThreadPanel();
        auto *thread = m_controller->thread();
        QTRY_VERIFY(thread->state() == ThreadController::Ready);
        const QString root = thread->rootEventId();
        const int before = mock()->pollCreatesForTest();
        const auto click = [this](QQuickItem *it) {
            const QPointF c = it->mapToScene(QPointF(it->width() / 2, it->height() / 2));
            QTest::mouseClick(m_window, Qt::LeftButton, {}, c.toPoint());
        };

        auto *plus = item("threadAttachButton");
        QVERIFY(plus);
        QTRY_VERIFY(plus->isEnabled() && plus->width() > 0);
        click(plus);
        auto *menu = m_window->findChild<QObject *>(QStringLiteral("threadAttachMenu"));
        QVERIFY2(menu, "the thread + has no menu");
        QTRY_VERIFY(menu->property("opened").toBool());
        auto *pollItem = m_window->findChild<QQuickItem *>(
            QStringLiteral("threadCreatePollMenuItem"));
        QVERIFY(pollItem);
        QTRY_VERIFY(pollItem->isVisible() && pollItem->isEnabled() && pollItem->width() > 0);
        click(pollItem);

        auto *dialog = m_window->findChild<QObject *>(QStringLiteral("threadCreatePollDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *content = dialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QQmlExpression fill(qmlContext(content), dialog, QStringLiteral(
            "questionField.text = 'Thread lunch?';"
            "answerModel.setProperty(0,'answerText','red');"
            "answerModel.setProperty(1,'answerText','blue');"
            "bumpAnswerRevision(); formValid"));
        QVERIFY(fill.evaluate().toBool());
        auto *submit = find(content, QStringLiteral("createPollSubmitButton"));
        QVERIFY(submit);
        QTRY_VERIFY(submit->isEnabled());
        click(submit);

        QTRY_COMPARE(mock()->pollCreatesForTest(), before + 1);
        QCOMPARE(mock()->lastPollThreadRootForTest(), root);
        QTRY_VERIFY2(!dialog->property("opened").toBool(),
                     "the acked poll did not retire the dialog");
        const auto hasPoll = [](const QList<TimelineEvent> &events) {
            for (const TimelineEvent &e : events) {
                if (e.type == TimelineEvent::Poll
                    && e.pollQuestion == QLatin1String("Thread lunch?"))
                    return true;
            }
            return false;
        };
        QTRY_VERIFY2(hasPoll(thread->model()->events()), "no local echo in the thread");
        QVERIFY2(!hasPoll(m_controller->timeline()->events()),
                 "the thread poll leaked into the main timeline");

        // Closed thread: refused, never retried as a room send.
        thread->close();
        bool queued = true;
        const QStringList answers{QStringLiteral("a"), QStringLiteral("b")};
        QVERIFY(QMetaObject::invokeMethod(
            thread, "createPoll", Q_RETURN_ARG(bool, queued),
            Q_ARG(QString, QStringLiteral("Q")),
            Q_ARG(QStringList, answers),
            Q_ARG(bool, false), Q_ARG(int, 1)));
        QVERIFY(!queued);
        QCOMPARE(mock()->pollCreatesForTest(), before + 1);
    }
};

QTEST_MAIN(ThreadComposerVimQmlTest)
#include "ThreadComposerVimQmlTest.moc"
