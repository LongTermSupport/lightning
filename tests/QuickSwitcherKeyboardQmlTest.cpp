
#include <QtTest/QtTest>

#include <QAccessible>
#include <QFile>
#include <QGuiApplication>
#include <QClipboard>
#include <QScopeGuard>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AccountManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/QuickSwitcherModel.h"

namespace {

QStringList s_announcements;

void captureAnnouncement(QAccessibleEvent *event)
{
    if (event->type() == QAccessible::Announcement)
        s_announcements.append(static_cast<QAccessibleAnnouncementEvent *>(event)->message());
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 340
    visible: true
    color: AppTheme.background

    QuickSwitcher { id: switcher; objectName: "switcher" }
    function openSwitcher() { switcher.open() }
    function openCommand() { switcher.openCommandMode() }
}
)QML";

}

class QuickSwitcherKeyboardQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QObject *switcher() const
    {
        return m_root->findChild<QObject *>(QStringLiteral("switcher"));
    }

    QQuickItem *field() const
    {
        return m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherField"));
    }

    QQuickItem *commandRow(int index) const
    {
        auto *list = m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        if (!list)
            return nullptr;
        QMetaObject::invokeMethod(list, "forceLayout");
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }

    int commandCount() const
    {
        auto *list = m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        return list ? list->property("count").toInt() : -1;
    }

    QStringList commandTitles() const
    {
        QStringList titles;
        const QVariantList rows =
            switcher()->property("commandRows").value<QVariantList>();
        for (const QVariant &row : rows) {
            const QVariantMap map = row.toMap();
            const QString label = map.value(QStringLiteral("label"))
                                      .toString();
            titles << (label.isEmpty()
                           ? map.value(QStringLiteral("name")).toString()
                           : label);
        }
        return titles;
    }

    void closeSwitcher()
    {
        QMetaObject::invokeMethod(switcher(), "close");
        QTRY_VERIFY(!switcher()->property("visible").toBool());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("quick-switcher-keyboard-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("quickswitcherscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    QQuickItem *resultList() const
    {
        return m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherList"));
    }

    bool currentRowVisible() const
    {
        QQuickItem *list = resultList();
        QMetaObject::invokeMethod(list, "forceLayout");
        const int cur = list->property("currentIndex").toInt();
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, cur));
        if (!item)
            return false;
        const qreal top = item->y() - list->property("contentY").toReal();
        return top >= -1 && top + item->height() <= list->height() + 1;
    }

    void configuredKeysNavigateWhileTheQueryKeepsFocus()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 2);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(field()->hasActiveFocus());
        QCOMPARE(field()->property("text").toString(), QString());
        QCOMPARE(m_controller->shortcuts()->setBinding(QStringLiteral("dialog.nextItem"),
                                                       QStringLiteral("Ctrl+N")), QString());
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QCOMPARE(m_controller->shortcuts()->resetToDefault(QStringLiteral("dialog.nextItem")), QString());
        closeSwitcher();

        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *commands = m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherCommandList"));
        QTRY_VERIFY(commands->property("count").toInt() > 2);
        QVERIFY(switcher()->property("commandMode").toBool());
        QVERIFY(field()->hasActiveFocus());
        QCOMPARE(m_controller->shortcuts()->dialogActionForKey(Qt::Key_J, Qt::ControlModifier),
                 QStringLiteral("dialog.nextItem"));
        commands->setProperty("currentIndex", -1);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(commands->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QCOMPARE(commands->property("currentIndex").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(commands->property("currentIndex").toInt(), 0);
        QVERIFY(field()->hasActiveFocus());
        closeSwitcher();
    }

    void modifiedArrowsAndReturnHonorDialogBindings_data()
    {
        QTest::addColumn<bool>("commandMode");
        QTest::newRow("rooms") << false;
        QTest::newRow("commands") << true;
    }

    void modifiedArrowsAndReturnHonorDialogBindings()
    {
        QFETCH(bool, commandMode);
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("dialog.nextItem"));
            shortcuts->resetToDefault(QStringLiteral("dialog.close"));
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.nextItem"),
                                      QStringLiteral("Ctrl+Up")), QString());
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.close"),
                                      QStringLiteral("Ctrl+Return")), QString());
        QMetaObject::invokeMethod(m_root, commandMode ? "openCommand" : "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *list = commandMode
            ? m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherCommandList"))
            : resultList();
        QVERIFY(list);
        QTRY_VERIFY(list->property("count").toInt() > 2);
        list->setProperty("currentIndex", 0);
        QTest::keyClick(m_window, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(field()->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
    }

    void modifiedReturnClosesWithoutRunningTheCommand()
    {
        auto *shortcuts = m_controller->shortcuts();
        const auto restore = qScopeGuard([&] {
            shortcuts->resetToDefault(QStringLiteral("dialog.close"));
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QCOMPARE(shortcuts->setBinding(QStringLiteral("dialog.close"),
                                      QStringLiteral("Ctrl+Return")), QString());
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        const QVariantList rows = switcher()->property("commandRows").value<QVariantList>();
        int discoverIndex = -1;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).toMap().value(QStringLiteral("id")).toString()
                == QStringLiteral("discover-rooms"))
                discoverIndex = i;
        }
        QVERIFY(discoverIndex >= 0);
        auto *list = m_root->findChild<QQuickItem *>(QStringLiteral("quickSwitcherCommandList"));
        QVERIFY(list);
        list->setProperty("currentIndex", discoverIndex);
        QSignalSpy discovery(switcher(), SIGNAL(discoverRequested(QString)));
        QVERIFY(discovery.isValid());
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        QCOMPARE(discovery.count(), 0);
    }

    void closeCopiesSelectedQueryTextBeforeDismissing()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_G);
        QCOMPARE(field()->property("text").toString(), QStringLiteral("g"));
        QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(field()->property("selectedText").toString(), QStringLiteral("g"));
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("g"));
        QVERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_Right);
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());

        QCOMPARE(m_controller->shortcuts()->setBinding(QStringLiteral("dialog.close"),
                                                       QStringLiteral("Alt+L")), QString());
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(switcher()->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_L, Qt::AltModifier);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        QCOMPARE(m_controller->shortcuts()->resetToDefault(QStringLiteral("dialog.close")), QString());
    }

    void tabWrapAroundScrollsTheFirstRowIntoView()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        (void) m_window->grabWindow();
        QQuickItem *list = resultList();
        QVERIFY(list);
        QTRY_VERIFY(list->property("count").toInt() > 0);
        const int count = list->property("count").toInt();
        const qreal viewH = list->height();
        qInfo() << "rows" << count << "viewport" << viewH;
        QVERIFY2(count * 48 > viewH * 1.5, "mock list does not overflow the viewport");

        QMetaObject::invokeMethod(switcher(), "selectRow", Q_ARG(QVariant, count - 1));
        (void) m_window->grabWindow();
        QTRY_VERIFY(currentRowVisible());
        QVERIFY(list->property("contentY").toReal() > list->property("originY").toReal());

        QTest::keyClick(m_window, Qt::Key_Tab);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QTRY_COMPARE(list->property("contentY").toReal(),
                     list->property("originY").toReal());
        QVERIFY(currentRowVisible());
        QMetaObject::invokeMethod(switcher(), "close");
    }

    MockMatrixClient *mock() const
    {
        return qobject_cast<MockMatrixClient *>(m_controller->findChild<MatrixClient *>());
    }

    QString roomAt(int row) const
    {
        return m_controller->quickSwitcher()->resultAt(row).value(QStringLiteral("roomId"))
            .toString();
    }

    int rowOfRoom(const QString &roomId) const
    {
        for (int row = 0; row < m_controller->quickSwitcher()->rowCount(); ++row)
            if (roomAt(row) == roomId)
                return row;
        return -1;
    }

    void liveRoomChangesKeepEnterOnTheSelectedRoom()
    {
        QVERIFY(mock());
        QStringList left;
        const auto restore = qScopeGuard([&] {
            for (const QString &id : left)
                mock()->setRoomMembershipForTest(id, RoomInfo::Joined);
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QQuickItem *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 3);
        list->setProperty("currentIndex", 2);
        const QString selected = roomAt(2);
        const QString above = roomAt(0);

        left << above;
        mock()->setRoomMembershipForTest(above, RoomInfo::Left);
        QTRY_COMPARE(rowOfRoom(above), -1);
        QCOMPARE(roomAt(list->property("currentIndex").toInt()), selected);

        const int at = list->property("currentIndex").toInt();
        left << selected;
        mock()->setRoomMembershipForTest(selected, RoomInfo::Left);
        QTRY_COMPARE(rowOfRoom(selected), -1);
        QCOMPARE(list->property("currentIndex").toInt(), at);
    }

    void liveResultChangesAreAnnouncedOnceAndNameAVanishedSelection()
    {
        QVERIFY(mock());
        QStringList left;
        s_announcements.clear();
        QAccessible::setActive(true);
        const auto previous = QAccessible::installUpdateHandler(captureAnnouncement);
        const auto restore = qScopeGuard([&] {
            QAccessible::installUpdateHandler(previous);
            QAccessible::setActive(false);
            for (const QString &id : left)
                mock()->setRoomMembershipForTest(id, RoomInfo::Joined);
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QQuickItem *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 4);
        list->setProperty("currentIndex", 3);
        const QString selected = roomAt(3);
        const QString name = m_controller->quickSwitcher()->resultAt(3)
                                 .value(QStringLiteral("name")).toString();
        QVERIFY(!name.isEmpty());
        QTest::qWait(900);
        QVERIFY2(s_announcements.isEmpty(), qPrintable(s_announcements.join(" | ")));

        const QString first = roomAt(0);
        const QString second = roomAt(1);
        left << first << second;
        mock()->setRoomMembershipForTest(first, RoomInfo::Left);
        mock()->setRoomMembershipForTest(second, RoomInfo::Left);
        QTRY_COMPARE(s_announcements.size(), 1);
        QTest::qWait(900);
        QCOMPARE(s_announcements.size(), 1);
        QVERIFY2(s_announcements.first().contains(name), qPrintable(s_announcements.first()));
        QVERIFY(!s_announcements.first().contains(QStringLiteral("no longer listed")));
        QCOMPARE(switcher()->property("lastResultAnnouncement").toString(),
                 s_announcements.first());

        left << selected;
        mock()->setRoomMembershipForTest(selected, RoomInfo::Left);
        QTRY_COMPARE(s_announcements.size(), 2);
        QVERIFY2(s_announcements.last().startsWith(name + QStringLiteral(" is no longer listed.")),
                 qPrintable(s_announcements.last()));

        field()->setProperty("text", QStringLiteral("zz"));
        field()->setProperty("text", QStringLiteral("zzq-no-such-room"));
        QTRY_COMPARE(s_announcements.size(), 3);
        QTest::qWait(900);
        QCOMPARE(s_announcements.size(), 3);
        QVERIFY2(!s_announcements.last().contains(QStringLiteral("no longer listed")),
                 qPrintable(s_announcements.last()));
    }

    void aCommandRebuildKeepsEnterOnTheSelectedAction()
    {
        SettingsManager *settings = m_controller->settings();
        const int preset = settings->layoutPreset();
        const auto restore = qScopeGuard([&] {
            settings->setLayoutPreset(preset);
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QMetaObject::invokeMethod(m_root, "openCommand");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        auto *commands = m_root->findChild<QQuickItem *>(
            QStringLiteral("quickSwitcherCommandList"));
        QTRY_VERIFY(commands->property("count").toInt() > 4);
        const auto idAt = [this](int row) {
            return switcher()->property("commandRows").value<QVariantList>().value(row)
                .toMap().value(QStringLiteral("id")).toString();
        };
        commands->setProperty("currentIndex", 4);
        const QString selected = idAt(4);
        QVERIFY(!selected.isEmpty());

        settings->setLayoutPreset(preset == 1 ? 0 : 1);
        QTRY_VERIFY(commandTitles().contains(preset == 1 ? QStringLiteral("Use compact layout")
                                                         : QStringLiteral("Use Lightning layout")));
        QCOMPARE(commands->property("currentIndex").toInt(), 4);
        QCOMPARE(idAt(4), selected);

        field()->setProperty("text", QStringLiteral("theme"));
        QTRY_COMPARE(commands->property("currentIndex").toInt(), 0);
    }

    void aPastedRoomIdOpensItAndAnUnknownAddressOnlyPreviews()
    {
        QVERIFY(mock());
        const auto restore = qScopeGuard([&] {
            QMetaObject::invokeMethod(switcher(), "close");
        });
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QQuickItem *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 0);
        QString target;
        for (int row = 0; row < m_controller->quickSwitcher()->rowCount(); ++row) {
            const QVariantMap r = m_controller->quickSwitcher()->resultAt(row);
            if (r.value(QStringLiteral("category")).toString() == QStringLiteral("room")
                && r.value(QStringLiteral("name")).toString() != r.value(QStringLiteral("roomId"))) {
                target = r.value(QStringLiteral("roomId")).toString();
                break;
            }
        }
        QVERIFY(!target.isEmpty());
        field()->setProperty("text", target);
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QCOMPARE(roomAt(0), target);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        QTRY_COMPARE(m_controller->currentRoomId(), target);

        QSignalSpy links(m_controller, SIGNAL(matrixLinkRequested(QString)));
        const QStringList joinsBefore = mock()->joinedTargets;
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        field()->setProperty("text", QStringLiteral("#nowhere:example.org"));
        QTRY_COMPARE(list->property("count").toInt(), 1);
        QCOMPARE(m_controller->quickSwitcher()->resultAt(0).value(QStringLiteral("category"))
                     .toString(), QStringLiteral("address"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        QCOMPARE(links.count(), 1);
        QCOMPARE(links.first().first().toString(), QStringLiteral("#nowhere:example.org"));
        QCOMPARE(m_controller->currentRoomId(), target);
        QCOMPARE(mock()->joinedTargets, joinsBefore);
    }

    void pageKeysMoveTheSelectionAndKeepItVisible()
    {
        QMetaObject::invokeMethod(m_root, "openSwitcher");
        QTRY_VERIFY(switcher()->property("opened").toBool());
        (void) m_window->grabWindow();
        QQuickItem *list = resultList();
        QTRY_VERIFY(list->property("count").toInt() > 0);
        const int count = list->property("count").toInt();
        QVariant stepV;
        QMetaObject::invokeMethod(switcher(), "pageRows",
                                  Q_RETURN_ARG(QVariant, stepV),
                                  Q_ARG(QVariant, QVariant::fromValue<QObject *>(list)));
        const int step = stepV.toInt();
        QVERIFY(step >= 1 && step < count - 1);

        QMetaObject::invokeMethod(switcher(), "selectRow", Q_ARG(QVariant, 0));
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QTRY_COMPARE(list->property("currentIndex").toInt(), step);
        QVERIFY(currentRowVisible());
        QTest::keyClick(m_window, Qt::Key_PageUp);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_End, Qt::ControlModifier);
        QTRY_COMPARE(list->property("currentIndex").toInt(), count - 1);
        QTRY_VERIFY(currentRowVisible());
        QTest::keyClick(m_window, Qt::Key_Home, Qt::ControlModifier);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(currentRowVisible());
        QMetaObject::invokeMethod(switcher(), "close");
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QuickSwitcherKeyboardQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "QuickSwitcherKeyboardQmlTest.moc"
