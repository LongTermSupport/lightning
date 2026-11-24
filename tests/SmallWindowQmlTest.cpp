// Small windows: users keep Lightning in a quarter of the screen. The real
// production window (Main.qml on the mock backend) is driven at the sizes a
// quarter screen gives, and every control that matters must lie inside the
// surface that is meant to hold it:
//
//   * each Settings section's controls stay inside the content viewport
//     horizontally (a section may scroll DOWN, never sideways);
//   * the pickers that anchor to the composer (GIF, emoji, stickers) keep
//     every control inside their own panel, and the panel inside the window;
//   * the window's own floor (Main.qml minimumWidth/minimumHeight) is the
//     size those cases are measured at.
//
// Offscreen geometry only: it is not a physical GUI pass.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QScopedPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/CallParticipantModel.h"
#include "calls/CallStageState.h"
#include "calls/OneToOneDm.h"
#include "calls/SfuCallController.h"
#include "matrix/MockMatrixClient.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;

bool isControl(const QByteArray &cls)
{
    return cls.contains("Button") || cls.contains("TextField")
        || cls.contains("ComboBox") || cls.contains("CheckBox")
        || cls.contains("Switch") || cls.contains("Slider")
        || cls.contains("TextArea");
}
}

class SmallWindowQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        for (QQuickItem *child : parent->childItems())
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    // Every visible control under `it` whose box, in `space`'s coordinates,
    // leaves [0, space.width] horizontally. Plain clips are NOT applied on
    // purpose (the point is what a clip would be hiding); a nested scroller is.
    static void controlsOutsideHorizontally(QQuickItem *it, QQuickItem *space,
                                            QStringList &out)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        if (isControl(cls) && it->width() > 0 && it->height() > 0) {
            QRectF r = space->mapRectFromItem(
                it, QRectF(0, 0, it->width(), it->height()));
            for (QQuickItem *a = it->parentItem(); a && a != space;
                 a = a->parentItem())
                if (a->inherits("QQuickFlickable"))
                    r = r.intersected(space->mapRectFromItem(
                        a, QRectF(0, 0, a->width(), a->height())));
            if (r.isEmpty())
                return;
            if (r.left() < -1.0 || r.right() > space->width() + 1.0)
                out << QStringLiteral("%1 %2 [%3..%4 of %5]")
                           .arg(QString::fromLatin1(cls), it->objectName())
                           .arg(qRound(r.left())).arg(qRound(r.right()))
                           .arg(qRound(space->width()));
        }
        for (QQuickItem *c : it->childItems())
            controlsOutsideHorizontally(c, space, out);
    }

    static void controlsOutsideBox(QQuickItem *it, const QRectF &box,
                                   QStringList &out)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        if (isControl(cls) && it->width() > 0 && it->height() > 0) {
            QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
            for (QQuickItem *a = it->parentItem(); a; a = a->parentItem())
                if (a->inherits("QQuickFlickable"))
                    r = r.intersected(a->mapRectToScene(
                        QRectF(0, 0, a->width(), a->height())));
            if (!r.isEmpty() && !box.adjusted(-1, -1, 1, 1).contains(r))
                out << QStringLiteral("%1 %2 [%3,%4 %5x%6]")
                           .arg(QString::fromLatin1(cls), it->objectName())
                           .arg(qRound(r.x())).arg(qRound(r.y()))
                           .arg(qRound(r.width())).arg(qRound(r.height()));
        }
        for (QQuickItem *c : it->childItems())
            controlsOutsideBox(c, box, out);
    }

    void setMirrored(bool rtl)
    {
        QQmlProperty mirror(m_window, QStringLiteral("LayoutMirroring.enabled"),
                            qmlContext(m_window));
        QVERIFY(mirror.isValid());
        QVERIFY(mirror.write(rtl));
        QCoreApplication::processEvents();
    }

    // The panel's last header action: the outermost visible IconButton whose
    // centre lies in the panel's top header band.
    QQuickItem *trailingHeaderButton(QQuickItem *panel, bool rtl) const
    {
        if (!panel)
            return nullptr;
        const QRectF p = panel->mapRectToScene(
            QRectF(0, 0, panel->width(), panel->height()));
        const qreal bandBottom = p.top() + 64;
        QQuickItem *best = nullptr;
        qreal bestX = 0;
        QList<QQuickItem *> stack{ panel };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            if (!it->isVisible() || it->opacity() <= 0.01)
                continue;
            if (QByteArray(it->metaObject()->className()).startsWith("IconButton")
                && it->width() > 0) {
                const QRectF r = it->mapRectToScene(
                    QRectF(0, 0, it->width(), it->height()));
                const qreal x = rtl ? -r.left() : r.right();
                if (r.center().y() < bandBottom && (!best || x > bestX)) {
                    best = it;
                    bestX = x;
                }
            }
            stack.append(it->childItems());
        }
        return best;
    }

    // The visible IconButton in the same header band nearest `trailing` on its
    // inner side, or null.
    QQuickItem *innerHeaderNeighbour(QQuickItem *panel, QQuickItem *trailing,
                                     bool rtl) const
    {
        const QRectF t = trailing->mapRectToScene(
            QRectF(0, 0, trailing->width(), trailing->height()));
        QQuickItem *best = nullptr;
        qreal bestGap = 0;
        QList<QQuickItem *> stack{ panel };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            if (!it->isVisible() || it->opacity() <= 0.01)
                continue;
            if (it != trailing
                && QByteArray(it->metaObject()->className()).startsWith("IconButton")
                && it->width() > 0) {
                const QRectF r = it->mapRectToScene(
                    QRectF(0, 0, it->width(), it->height()));
                const qreal gap = rtl ? r.left() - t.right() : t.left() - r.right();
                if (qAbs(r.center().y() - t.center().y()) < 1.0 && gap >= 0
                    && (!best || gap < bestGap)) {
                    best = it;
                    bestGap = gap;
                }
            }
            stack.append(it->childItems());
        }
        return best;
    }

    // Every visible item under `it` that leaves `panel` (vertically only where
    // a scroller shows it), every cut unelided text, and every control.
    static void filterPanelFaults(QQuickItem *it, QQuickItem *panel, qreal top,
                                  qreal bottom, QStringList &out,
                                  QList<QQuickItem *> &controls)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        const QRectF r = panel->mapRectFromItem(
            it, QRectF(0, 0, it->width(), it->height()));
        if (it->width() > 0 && it->height() > 0
            && qMin(r.bottom(), bottom) > qMax(r.top(), top)) {
            if (r.left() < -0.5 || r.right() > panel->width() + 0.5
                || r.top() < -0.5 || r.bottom() > panel->height() + 0.5)
                out << QStringLiteral("%1 %2 at [%3..%4] x [%5..%6]")
                           .arg(QString::fromLatin1(cls), it->objectName())
                           .arg(qRound(r.left())).arg(qRound(r.right()))
                           .arg(qRound(r.top())).arg(qRound(r.bottom()));
            if (it->inherits("QQuickText") && it->width() + 0.5 < it->implicitWidth()
                && it->property("elide").toInt() == 0
                && it->property("wrapMode").toInt() == 0)
                out << QStringLiteral("text '%1' cut at %2 of %3")
                           .arg(it->property("text").toString())
                           .arg(it->width()).arg(it->implicitWidth());
            if (isControl(cls))
                controls << it;
        }
        if (it->inherits("QQuickFlickable")) {
            top = qMax(top, r.top());
            bottom = qMin(bottom, r.bottom());
        }
        for (QQuickItem *c : it->childItems())
            filterPanelFaults(c, panel, top, bottom, out, controls);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("small-window-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy createdSpy(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        m_window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        m_window->setWidth(640);
        m_window->setHeight(360);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_controller;
    }

    void theWindowFloorIsTheSizeTheCasesAssume()
    {
        QCOMPARE(m_window->minimumWidth(), 640);
        QCOMPARE(m_window->minimumHeight(), 360);
    }

    void everySettingsSectionStaysInsideItsViewportAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
    }

    void everySettingsSectionStaysInsideItsViewportAtTheFloor()
    {
        QFETCH(QSize, size);
        m_window->resize(size);
        m_controller->showSettings();
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QStringList failures;
        const QStringList sections = {
            QStringLiteral("account"),       QStringLiteral("privacy"),
            QStringLiteral("sessions"),      QStringLiteral("appearance"),
            QStringLiteral("layout"),        QStringLiteral("chat"),
            QStringLiteral("notifications"), QStringLiteral("shortcuts"),
            QStringLiteral("sound"),         QStringLiteral("labs"),
            QStringLiteral("updates"),       QStringLiteral("about") };
        int measured = 0;
        for (const QString &section : sections) {
            m_controller->showSettingsSection(section);
            QCoreApplication::processEvents();
            QTest::qWait(250);
            QQuickItem *flick = item("settingsContentFlick");
            QVERIFY2(flick, "settings content viewport not found");
            auto *content = flick->property("contentItem").value<QQuickItem *>();
            QVERIFY(content);
            QStringList outside;
            controlsOutsideHorizontally(content, flick, outside);
            ++measured;
            for (const QString &o : outside)
                failures << section + QLatin1String(": ") + o;
        }
        QCOMPARE(measured, sections.size());
        QVERIFY2(failures.isEmpty(),
                 qPrintable(QStringLiteral("%1x%2 -> ").arg(size.width())
                                .arg(size.height())
                            + failures.join(QStringLiteral("; "))));
        m_controller->showMain();
        QCoreApplication::processEvents();
    }

    void thePickersKeepEveryControlInsideTheirPanelAndTheWindow_data()
    {
        QTest::addColumn<QString>("opener");
        QTest::addColumn<QString>("popupName");
        QTest::newRow("gif") << "openGifPicker" << "composerGifPicker";
        QTest::newRow("emoji") << "openEmojiPicker" << "composerEmojiPicker";
        QTest::newRow("sticker") << "openStickerPicker" << "composerStickerPicker";
    }

    void thePickersKeepEveryControlInsideTheirPanelAndTheWindow()
    {
        QFETCH(QString, opener);
        QFETCH(QString, popupName);
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(200);
        QQuickItem *composer = item("messageComposer");
        QVERIFY2(composer, "composer not found");
        if (opener == QLatin1String("openEmojiPicker"))
            QVERIFY(QMetaObject::invokeMethod(composer, "openEmojiPicker",
                                              Q_ARG(QVariant, QVariant(false))));
        else
            QVERIFY(QMetaObject::invokeMethod(composer, opener.toLatin1()));
        QTest::qWait(400);
        QObject *popup = m_window->findChild<QObject *>(popupName);
        QVERIFY2(popup, qPrintable(popupName + QLatin1String(" not found")));
        QTRY_VERIFY2(popup->property("opened").toBool(), "picker did not open");
        auto *panel = popup->property("contentItem").value<QQuickItem *>();
        QVERIFY(panel);
        QTest::qWait(300);
        const QRectF pr = panel->mapRectToScene(
            QRectF(0, 0, panel->width(), panel->height()));
        QVERIFY2(QRectF(0, 0, m_window->width(), m_window->height())
                     .adjusted(-1, -1, 1, 1).contains(pr),
                 qPrintable(popupName + QStringLiteral(" panel is outside the "
                            "window: %1,%2 %3x%4").arg(pr.x()).arg(pr.y())
                            .arg(pr.width()).arg(pr.height())));
        QStringList outside;
        controlsOutsideHorizontally(panel, panel, outside);
        QVERIFY2(outside.isEmpty(),
                 qPrintable(popupName + QLatin1String(": ")
                            + outside.join(QStringLiteral("; "))));
        QMetaObject::invokeMethod(popup, "close");
        QTest::qWait(100);
    }

    void theCallPanelNeverPushesTheComposerOutOfAShortWindow()
    {
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(200);
        QQuickItem *pane = item("timelinePane");
        QQuickItem *header = item("roomHeaderBand");
        QQuickItem *callBar = item("timelineCallHeaderBar");
        QQuickItem *composer = item("messageComposer");
        QVERIFY(pane && header && callBar && composer);
        QVariant tallest;
        QVERIFY(QMetaObject::invokeMethod(pane, "clampCallPanelHeight",
                                          Q_RETURN_ARG(QVariant, tallest),
                                          Q_ARG(QVariant, QVariant(100000))));
        const qreal used = header->height() + callBar->height() + 9
                           + tallest.toReal() + composer->height();
        QVERIFY2(used <= pane->height(),
                 qPrintable(QStringLiteral("header %1 + call bar %2 + panel %3 "
                                           "+ composer %4 = %5 > pane %6")
                                .arg(header->height()).arg(callBar->height())
                                .arg(tallest.toReal()).arg(composer->height())
                                .arg(used).arg(pane->height())));
    }

    void theCallControlsNeverDrawOverTheParticipantFaces_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("collapsed");
        QTest::newRow("640x720") << QSize(640, 720) << false;
        QTest::newRow("640x360") << QSize(640, 360) << false;
        QTest::newRow("854x480") << QSize(854, 480) << false;
        QTest::newRow("1280x720") << QSize(1280, 720) << false;
        QTest::newRow("960x540") << QSize(960, 540) << false;
        QTest::newRow("1000x600") << QSize(1000, 600) << false;
        QTest::newRow("1000x600 collapsed") << QSize(1000, 600) << true;
        QTest::newRow("640x720 collapsed") << QSize(640, 720) << true;
    }

    void theCallControlsNeverDrawOverTheParticipantFaces()
    {
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        QFETCH(QSize, size);
        QFETCH(bool, collapsed);
        const QString room = QStringLiteral("!general:mock.local");
        m_controller->showMain();
        m_controller->setCurrentRoomId(room);
        m_window->resize(size);
        SfuCallController *call = m_controller->groupCall();
        call->setMembershipForTest(room, QString());
        call->setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT call->stateChanged();
        QVariantList people;
        for (int i = 0; i < 6; ++i) {
            QVariantMap person;
            person.insert(QStringLiteral("identity"),
                          QStringLiteral("person%1").arg(i));
            person.insert(QStringLiteral("sid"), QStringLiteral("PA_%1").arg(i));
            if (i == 0) {
                QVariantMap track;
                track.insert(QStringLiteral("source"),
                             QStringLiteral("screen_share"));
                track.insert(QStringLiteral("sid"), QStringLiteral("TR_share"));
                track.insert(QStringLiteral("muted"), false);
                person.insert(QStringLiteral("tracks"), QVariantList{track});
            }
            people << person;
        }
        call->ingestParticipantsForTest(people);
        call->stageState()->restoreShare(QStringLiteral("TR_share"));
        QList<CallParticipantRow> rows;
        for (int i = 0; i < 6; ++i) {
            CallParticipantRow person;
            person.identity = QStringLiteral("person%1").arg(i);
            person.sid = QStringLiteral("PA_%1").arg(i);
            person.displayName = person.identity;
            rows << person;
        }
        call->participantModel()->applyParticipants(rows);
        QTRY_VERIFY(item("callHeaderRow") != nullptr);
        QQuickItem *stage = item("timelineCallStage");
        QVERIFY(stage);
        QObject *pane = item("timelinePane");
        QVERIFY(pane);
        pane->setProperty("callPanelCollapsed", collapsed);
        QTest::qWait(250);

        QQuickItem *row = item("callHeaderRow");
        QQuickItem *controls = item("callStageControls");
        QQuickItem *bar = findItem(controls, QStringLiteral("callHeaderBarRow"));
        QVERIFY(row && controls && bar);
        QVERIFY2(collapsed
                     || stage->property("effectiveLayout").toString()
                            == QLatin1String("spotlight"),
                 "no spotlight, so the faces are not in play");
        const auto shown = [this](const char *name) {
            QQuickItem *it = item(name);
            return it && it->isVisible() && it->width() > 0
                && it->width() + 1 >= it->implicitWidth();
        };
        const bool facesDrawn = shown("callHeaderBubblesHost")
            || shown("callStrip") || shown("callStripBubblesHost");
        const QRectF rowBox = stage->mapRectToScene(
            QRectF(0, 0, stage->width(), stage->height()));
        QStringList faults;
        const auto box = [](QQuickItem *it) {
            return it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        };
        QList<QPair<QString, QRectF>> cells;
        for (QQuickItem *c : row->childItems()) {
            if (!c->isVisible() || c->width() <= 0 || c->height() <= 0
                || c == controls->parentItem())
                continue;
            const QString name = c->objectName().isEmpty()
                ? QString::fromLatin1(c->metaObject()->className())
                : c->objectName();
            cells.append({name, box(c)});
        }
        for (QQuickItem *c : bar->childItems())
            if (c->isVisible() && c->width() > 0 && c->height() > 0) {
                const QRectF r = box(c);
                cells.append({c->objectName().isEmpty()
                                  ? QStringLiteral("control")
                                  : c->objectName(), r});
            }
        for (int i = 0; i < cells.size(); ++i) {
            const QRectF a = cells.at(i).second;
            if (a.left() < rowBox.left() - 1 || a.right() > rowBox.right() + 1)
                faults << QStringLiteral("%1 [%2..%3] leaves the stage [%4..%5]")
                              .arg(cells.at(i).first).arg(a.left())
                              .arg(a.right()).arg(rowBox.left())
                              .arg(rowBox.right());
            for (int j = i + 1; j < cells.size(); ++j) {
                const QRectF overlap = a.intersected(cells.at(j).second);
                if (overlap.width() > 1 && overlap.height() > 1)
                    faults << QStringLiteral("%1 overlaps %2 by %3 px")
                                  .arg(cells.at(i).first, cells.at(j).first)
                                  .arg(overlap.width());
            }
        }
        if (!collapsed) {
            call->setStreamInfoVisible(true);
            QQuickItem *info = item("callStageStreamInfo");
            QTRY_VERIFY(info && info->isVisible());
            const QRectF ib = box(info);
            for (const auto &cell : cells) {
                const QRectF overlap = ib.intersected(cell.second);
                if (overlap.width() > 1 && overlap.height() > 1)
                    faults << QStringLiteral("Stream Info covers %1").arg(cell.first);
            }
            call->setStreamInfoVisible(false);
        }
        pane->setProperty("callPanelCollapsed", false);
        call->setCallStateForTest(SfuCallController::State::Idle);
        Q_EMIT call->stateChanged();
        QVERIFY2(faults.isEmpty(), qPrintable(faults.join(QStringLiteral("; "))));
        QVERIFY2(collapsed || facesDrawn, "the faces went nowhere");
#endif
    }

    void theCallTypeMenuIsReachableWhenTheCallButtonFolds()
    {
        // The mock has no call lane until a backend exists; then a 1:1 DM has.
        class Engine : public CallMediaBackend
        {
        public:
            using CallMediaBackend::CallMediaBackend;
            void createOffer(const QString &) override {}
            void createAnswer(const QString &, const QString &) override {}
            void setRemoteAnswer(const QString &, const QString &) override {}
            void addRemoteCandidate(const QString &, const QString &,
                                    const QString &, int) override {}
            void setIceServers(const QStringList &, const QString &,
                               const QString &) override {}
            void close(const QString &) override {}
            bool supportsVideo() const override { return true; }
        };
        Engine engine;
        m_controller->calls()->setMediaBackend(&engine);
        const auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        QString dm;
        for (const RoomInfo &room : mock->rooms())
            if (!calls::oneToOneDmPeer(room, true).isEmpty()) {
                dm = room.id;
                break;
            }
        QVERIFY(!dm.isEmpty());
        m_controller->showMain();
        m_controller->setCurrentRoomId(dm);
        m_window->resize(640, 360);
        QTest::qWait(150);
        QObject *button = m_window->findChild<QObject *>(
            QStringLiteral("startVoiceCallButton"));
        QVERIFY(button);
        QVERIFY2(button->property("available").toBool(),
                 "this room offers no call, so nothing can fold");
        QVERIFY2(button->property("folded").toBool(),
                 "the floor did not fold the call button: nothing measured");
        QQuickItem *overflow = item("roomHeaderOverflowButton");
        QVERIFY(overflow);
        QObject *menu = m_window->findChild<QObject *>(
            QStringLiteral("roomHeaderOverflowMenu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *row =
            menu->findChild<QObject *>(QStringLiteral("overflowCallOptions"));
        QVERIFY(row);
        QVERIFY(row->property("visible").toBool());
        QMetaObject::invokeMethod(row, "triggered");
        QMetaObject::invokeMethod(menu, "close");
        QObject *format = m_window->findChild<QObject *>(
            QStringLiteral("callFormatMenu"));
        QVERIFY(format);
        QTRY_VERIFY(format->property("opened").toBool());
        const qreal x = format->property("x").toReal();
        const qreal width = format->property("width").toReal();
        const QRectF placed = overflow->mapRectToScene(QRectF(
            x, format->property("y").toReal(), width,
            format->property("height").toReal()));
        QVERIFY2(placed.left() >= 0 && placed.right() <= m_window->width()
                     && placed.top() >= 0
                     && placed.bottom() <= m_window->height(),
                 qPrintable(QStringLiteral("the call menu sits at %1,%2 %3x%4")
                                .arg(placed.x()).arg(placed.y())
                                .arg(placed.width()).arg(placed.height())));
        QMetaObject::invokeMethod(format, "close");
        QTRY_VERIFY(!format->property("visible").toBool());
        m_controller->calls()->setMediaBackend(nullptr);
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
    }

    void theFindBarKeepsEveryControlInsideItsCardAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
    }

    void theFindBarKeepsEveryControlInsideItsCardAtTheFloor()
    {
        QFETCH(QSize, size);
        m_controller->showMain();
        m_window->resize(size);
        QCoreApplication::processEvents();
        QTest::qWait(200);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);
        QVERIFY(QMetaObject::invokeMethod(pane, "openFind"));
        QTest::qWait(300);
        QQuickItem *bar = item("timelineFindBar");
        QVERIFY(bar && bar->isVisible());
        QStringList outside;
        controlsOutsideHorizontally(bar, bar, outside);
        QVERIFY2(outside.isEmpty(), qPrintable(outside.join(QStringLiteral("; "))));
        QQuickItem *field = item("timelineFindField");
        QVERIFY(field);
        QVERIFY2(field->width() >= 100,
                 qPrintable(QStringLiteral("the find field is %1 px wide")
                                .arg(field->width())));
        QMetaObject::invokeMethod(pane, "closeFind");
        QTest::qWait(100);
    }

    void everyMainDialogFitsTheWindowAtTheFloor_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("open");
        const auto row = [](const char *type, const char *open) {
            QTest::newRow(type) << QString::fromLatin1(type)
                                << QString::fromLatin1(open);
        };
        row("JumpToDateDialog", "openDialog()");
        row("SendLaterDialog", "openFor({})");
        row("LeaveRoomConfirmDialog",
            "openFor('!r:mock.local', 'Room', 12, 'public', false)");
        row("ExportRoomDialog", "openDialog()");
        row("DiscoverJoinDialog", "openDialog('browse')");
        row("ForwardSelectionDialog", "openDialog()");
        row("QrLoginDialog", "openDialog()");
        row("InvitePeopleDialog", "openFor('!general:mock.local')");
        row("CreatePollDialog", "openDialog()");
        row("NewConversationDialog", "openDialog('room', {})");
        row("RoomUpgradeDialog", "openFor('!general:mock.local')");
        row("SpaceSettingsDialog", "openFor('!space:mock.local')");
        row("AddWidgetDialog", "openDialog()");
        row("MessageSearchDialog", "openDialog('')");
        row("StatusDialog", "openForEdit()");
        row("ChatBackgroundDialog", "openForDefault()");
        row("PolicyListDialog", "openFor('!r:mock.local', 'Room')");
    }

    void everyMainDialogFitsTheWindowAtTheFloor()
    {
        QFETCH(QString, type);
        QFETCH(QString, open);
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QQmlComponent component(m_engine);
        component.setData(QStringLiteral(
            "import QtQuick\nimport QtQuick.Controls\nimport MatrixClient\n"
            "Item { anchors.fill: parent\n  %1 { id: dlg; objectName: 'dlg' }\n}\n")
                .arg(type).toUtf8(), QUrl(QStringLiteral("dialog-under-test.qml")));
        QScopedPointer<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *host = qobject_cast<QQuickItem *>(owner.data());
        QVERIFY(host);
        host->setParentItem(m_window->contentItem());
        host->setWidth(m_window->width());
        host->setHeight(m_window->height());
        QQmlExpression expr(qmlContext(host), host,
                            QStringLiteral("dlg.%1").arg(open));
        expr.evaluate();
        QVERIFY2(!expr.hasError(), qPrintable(expr.error().toString()));
        QObject *dlg = host->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(dlg);
        QTRY_VERIFY_WITH_TIMEOUT(dlg->property("opened").toBool(), 1500);
        QTest::qWait(350);
        const qreal x = dlg->property("x").toReal();
        const qreal y = dlg->property("y").toReal();
        const qreal w = dlg->property("width").toReal();
        const qreal h = dlg->property("height").toReal();
        const QRectF box(x, y, w, h);
        const QRectF win(0, 0, m_window->width(), m_window->height());
        QStringList stray;
        for (const char *part : { "contentItem", "footer", "header" }) {
            auto *it = dlg->property(part).value<QQuickItem *>();
            if (it)
                controlsOutsideBox(it, QRectF(x, y, w, h), stray);
        }
        QMetaObject::invokeMethod(dlg, "close");
        QTest::qWait(150);
        QVERIFY2(stray.isEmpty(),
                 qPrintable(type + QLatin1String(" has controls outside its "
                            "box: ") + stray.join(QStringLiteral("; "))));
        QVERIFY2(win.adjusted(-1, -1, 1, 1).contains(box),
                 qPrintable(QStringLiteral("%1 is %2,%3 %4x%5 in a %6x%7 window")
                                .arg(type).arg(x).arg(y).arg(w).arg(h)
                                .arg(win.width()).arg(win.height())));
    }

    void theImageViewerChromeStaysInsideTheWindowAtTheFloor()
    {
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QObject *viewer = m_window->findChild<QObject *>(
            QStringLiteral("imageViewerOverlay"));
        QVERIFY2(viewer, "image viewer not found");
        QVariantList entries;
        for (int i = 0; i < 6; ++i) {
            QVariantMap e;
            e.insert(QStringLiteral("row"), i);
            e.insert(QStringLiteral("mediaKey"), QStringLiteral("mock-%1").arg(i));
            e.insert(QStringLiteral("filename"), QStringLiteral("photo-%1.png").arg(i));
            e.insert(QStringLiteral("sender"), QStringLiteral("Alice"));
            e.insert(QStringLiteral("mime"), QStringLiteral("image/png"));
            e.insert(QStringLiteral("httpUrl"), QString());
            e.insert(QStringLiteral("isImage"), true);
            e.insert(QStringLiteral("isVideo"), false);
            e.insert(QStringLiteral("isVisual"), true);
            e.insert(QStringLiteral("thumbAvailable"), false);
            e.insert(QStringLiteral("size"), 1234);
            e.insert(QStringLiteral("timestamp"), 1.7e12 + i);
            entries.append(e);
        }
        QVERIFY(QMetaObject::invokeMethod(viewer, "openAt",
                                          Q_ARG(QVariant, entries),
                                          Q_ARG(QVariant, 2)));
        QTest::qWait(500);
        for (const char *name : { "viewerCloseButton", "viewerToolbar" }) {
            QQuickItem *chrome = item(name);
            QVERIFY2(chrome && chrome->isVisible() && chrome->width() > 0,
                     name);
        }
        QStringList stray;
        controlsOutsideBox(m_window->contentItem(),
                           QRectF(0, 0, m_window->width(), m_window->height()),
                           stray);
        QStringList viewerOnly;
        for (const QString &s : stray)
            if (s.contains(QLatin1String("viewer"), Qt::CaseInsensitive))
                viewerOnly << s;
        QMetaObject::invokeMethod(viewer, "close");
        QTest::qWait(150);
        QVERIFY2(viewerOnly.isEmpty(), qPrintable(viewerOnly.join(QStringLiteral("; "))));
    }

    void theRoomInfoPanelStaysInsideTheWindowAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
        QTest::newRow("960x540") << QSize(960, 540);
    }

    void theRoomInfoPanelStaysInsideTheWindowAtTheFloor()
    {
        QFETCH(QSize, size);
        m_controller->showMain();
        m_window->resize(size);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);
        QVERIFY(pane->setProperty("infoOpen", true));
        QTest::qWait(500);
        QStringList stray;
        controlsOutsideBox(m_window->contentItem(),
                           QRectF(0, 0, m_window->width(), m_window->height()),
                           stray);
        QQuickItem *panel = item("roomInfoPanel");
        const bool shown = panel && panel->isVisible() && panel->width() > 100;
        const qreal paneW = pane->width();
        const qreal panelW = panel ? panel->width() : 0;
        QQuickItem *roomColumn = item("roomColumn");
        const bool roomHidden = roomColumn && !roomColumn->isVisible();
        QQuickItem *drop = item("chatDropArea");
        QVERIFY(drop);
        const qreal dropW = drop->width();
        pane->setProperty("infoOpen", false);
        QTest::qWait(150);
        QVERIFY(!roomHidden || dropW == 0);
        QVERIFY2(shown, qPrintable(QStringLiteral("Room Information is not "
                                                  "shown at %1x%2")
                                       .arg(size.width()).arg(size.height())));
        if (paneW < 700)
            QVERIFY2(roomHidden && qAbs(panelW - paneW) < 1.0,
                     "a narrow pane should hand itself to Room Information");
        QVERIFY2(stray.isEmpty(), qPrintable(stray.join(QStringLiteral("; "))));
    }

    void theLoginScreenStaysInsideTheWindowAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
    }

    void theLoginScreenStaysInsideTheWindowAtTheFloor()
    {
        QFETCH(QSize, size);
        m_window->resize(size);
        m_controller->showLogin();
        QCoreApplication::processEvents();
        QTest::qWait(400);
        QStringList stray;
        controlsOutsideBox(m_window->contentItem(),
                           QRectF(0, 0, m_window->width(), m_window->height()),
                           stray);
        const QSize sz = size;
        const bool ok = m_controller->showMain();
        QTest::qWait(200);
        QVERIFY2(stray.isEmpty(),
                 qPrintable(QStringLiteral("%1x%2: ").arg(sz.width()).arg(sz.height())
                            + stray.join(QStringLiteral("; "))));
        QVERIFY(ok);
    }

    void theAccountSwitcherFooterStaysInsideTheWindowAtTheFloor()
    {
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(
            "import QtQuick\nimport QtQuick.Controls\nimport MatrixClient\n"
            "Item { anchors.fill: parent\n"
            "  AccountMenu { id: dlg; objectName: 'dlg'; x: 86; y: parent.height - 60; margins: 12 }\n}\n"),
            QUrl(QStringLiteral("account-menu-under-test.qml")));
        QScopedPointer<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *host = qobject_cast<QQuickItem *>(owner.data());
        QVERIFY(host);
        host->setParentItem(m_window->contentItem());
        host->setWidth(m_window->width());
        host->setHeight(m_window->height());
        QObject *menu = host->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(), 1500);
        QTest::qWait(400);
        auto *content = menu->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const QRectF win(0, 0, m_window->width(), m_window->height());
        const QRectF cr = content->mapRectToScene(
            QRectF(0, 0, content->width(), content->height()));
        QStringList stray;
        controlsOutsideBox(content, win, stray);
        QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(150);
        QVERIFY2(win.adjusted(-1, -1, 1, 1).contains(cr),
                 qPrintable(QStringLiteral("switcher content is %1,%2 %3x%4 in a "
                                           "%5x%6 window").arg(cr.x()).arg(cr.y())
                                .arg(cr.width()).arg(cr.height())
                                .arg(win.width()).arg(win.height())));
        QVERIFY2(stray.isEmpty(), qPrintable(stray.join(QStringLiteral("; "))));
    }

    void everyPaneHeadersLastActionSitsOnTheRoomHeadersRail_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("rtl");
        for (const QSize &sz : { QSize(640, 360), QSize(960, 540),
                                 QSize(1280, 720) })
            for (bool rtl : { false, true })
                QTest::newRow(qPrintable(QStringLiteral("%1x%2 %3")
                                             .arg(sz.width()).arg(sz.height())
                                             .arg(QLatin1String(rtl ? "rtl" : "ltr"))))
                    << sz << rtl;
    }

    void everyPaneHeadersLastActionSitsOnTheRoomHeadersRail()
    {
        QFETCH(QSize, size);
        QFETCH(bool, rtl);
        m_controller->showMain();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        m_controller->settings()->setSpacesRailVisible(false);
        m_window->resize(size);
        setMirrored(rtl);
        QTest::qWait(250);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);

        struct Seen { QString name; qreal gutter, w, h, cy, radius; };
        QList<Seen> seen;
        QList<QPair<QString, qreal>> gaps;
        QStringList missing;
        QQuickItem *band = item("roomHeaderBand");
        QVERIFY(band);
        const auto measure = [&](const QString &name, QQuickItem *edge,
                                 QQuickItem *button) {
            if (!edge || !button || !edge->isVisible() || !button->isVisible()
                || button->width() <= 0) {
                missing << name;
                return;
            }
            const QRectF e = edge->mapRectToScene(
                QRectF(0, 0, edge->width(), edge->height()));
            const QRectF b = button->mapRectToScene(
                QRectF(0, 0, button->width(), button->height()));
            if (edge != band && name != QLatin1String("roomList")
                && button->property("iconName").toString() != QLatin1String("close"))
                missing << name + QStringLiteral(" (last action is not its close)");
            if (QQuickItem *inner = innerHeaderNeighbour(edge, button, rtl)) {
                const QRectF n = inner->mapRectToScene(
                    QRectF(0, 0, inner->width(), inner->height()));
                gaps.append({ name, rtl ? n.left() - b.right() : b.left() - n.right() });
            }
            seen.append({ name, rtl ? b.left() - e.left() : e.right() - b.right(),
                          b.width(), b.height(), b.center().y() - e.top(),
                          button->property("radius").toReal() });
        };

        QQuickItem *overflow = item("roomHeaderOverflowButton");
        QVERIFY(overflow);
        const qreal overflowX = overflow->mapToItem(
            band, QPointF(overflow->width() / 2, 0)).x();
        QVERIFY2(rtl == (overflowX < band->width() / 2.0),
                 qPrintable(QStringLiteral("the header did not mirror: overflow "
                                           "at %1 of %2").arg(overflowX)
                                .arg(band->width())));

        measure(QStringLiteral("room"), band, overflow);
        int expected = 1;
        QQuickItem *railActions = item("railHiddenActions");
        QVERIFY(railActions);
        if (railActions->isVisible()) {
            measure(QStringLiteral("roomList"), railActions->parentItem(),
                    item("headerAccountTile"));
            ++expected;
        } else {
            QVERIFY2(size.width() < 1280, "the room list header was never shown");
        }

        QVERIFY(QMetaObject::invokeMethod(pane, "toggleThreadSurface"));
        QTest::qWait(300);
        measure(QStringLiteral("thread"), item("threadPanel"),
                trailingHeaderButton(item("threadPanel"), rtl));
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleThreadSurface"));
        QTest::qWait(150);

        QVERIFY(QMetaObject::invokeMethod(pane, "toggleSearchPanel"));
        QTest::qWait(300);
        measure(QStringLiteral("search"), item("roomSearchPanel"),
                trailingHeaderButton(item("roomSearchPanel"), rtl));
        QVERIFY(QMetaObject::invokeMethod(item("roomSearchPanel"), "beginFilters"));
        QTest::qWait(300);
        measure(QStringLiteral("searchFilters"), item("roomSearchPanel"),
                trailingHeaderButton(item("roomSearchPanel"), rtl));
        item("roomSearchPanel")->setProperty("filterEditing", false);
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleSearchPanel"));
        QTest::qWait(150);

        QQuickItem *infoPanel = item("roomInfoPanel");
        QVERIFY(infoPanel);
        // The mock has no pinned events, so the panel refuses that section.
        for (const char *section : { "overview", "people", "media", "widgets" }) {
            infoPanel->setProperty("section", QString::fromLatin1(section));
            pane->setProperty("infoOpen", true);
            QTest::qWait(400);
            measure(QStringLiteral("roomInfo/") + QLatin1String(section),
                    infoPanel, trailingHeaderButton(infoPanel, rtl));
            pane->setProperty("infoOpen", false);
            QTest::qWait(100);
        }

        m_controller->showSettings();
        QTest::qWait(300);
        measure(QStringLiteral("settings"), item("settingsHeaderBar"),
                item("settingsCloseButton"));
        m_controller->showMain();
        expected += 8;

        m_controller->settings()->setSpacesRailVisible(true);
        setMirrored(false);
        QTest::qWait(150);

        QVERIFY(!seen.isEmpty());
        const Seen ref = seen.first();
        QStringList faults;
        for (const Seen &h : seen) {
            const QString got = QStringLiteral("%1: gutter %2 box %3x%4 centre %5 "
                                               "radius %6")
                                    .arg(h.name).arg(h.gutter).arg(h.w).arg(h.h)
                                    .arg(h.cy).arg(h.radius);
            if (qAbs(h.gutter - ref.gutter) > 0.5 || qAbs(h.w - ref.w) > 0.5
                || qAbs(h.h - ref.h) > 0.5 || qAbs(h.cy - ref.cy) > 0.5
                || (h.name != QLatin1String("roomList")
                    && qAbs(h.radius - ref.radius) > 0.5))
                faults << got;
        }
        for (const QString &m : missing)
            faults << m + QStringLiteral(": not shown");
        QVERIFY2(faults.isEmpty(),
                 qPrintable(QStringLiteral("room header: gutter %1 box %2x%3 "
                                           "centre %4 radius %5; differs: ")
                                .arg(ref.gutter).arg(ref.w).arg(ref.h).arg(ref.cy)
                                .arg(ref.radius)
                            + faults.join(QStringLiteral("; "))));
        QCOMPARE(seen.size(), expected);

        // Search and the four Room information sections each have an action
        // beside their last one; the room header too unless it folded them.
        QStringList gapNames;
        for (const auto &g : gaps)
            gapNames << g.first;
        QVERIFY2(!gaps.isEmpty()
                     && (size.width() < 960 || gaps.first().first == QLatin1String("room")),
                 qPrintable(gapNames.join(QStringLiteral(", "))));
        QStringList spacing;
        for (const auto &g : gaps)
            if (qAbs(g.second - gaps.first().second) > 0.5)
                spacing << QStringLiteral("%1: %2").arg(g.first).arg(g.second);
        QVERIFY2(spacing.isEmpty(),
                 qPrintable(QStringLiteral("%1 actions %2 px apart; differs: ")
                                .arg(gaps.first().first).arg(gaps.first().second)
                            + spacing.join(QStringLiteral("; "))));
        for (const char *n : { "search", "roomInfo/overview", "roomInfo/people",
                               "roomInfo/media", "roomInfo/widgets" })
            QVERIFY2(gapNames.contains(QLatin1String(n)),
                     qPrintable(QStringLiteral("%1 has no inner action; measured: %2")
                                    .arg(QLatin1String(n),
                                         gapNames.join(QStringLiteral(", ")))));
    }

    void theRightmostHeaderTooltipsStayInsideTheWindow_data()
    {
        QTest::addColumn<bool>("rtl");
        QTest::addColumn<bool>("settings");
        QTest::newRow("room ltr") << false << false;
        QTest::newRow("room rtl") << true << false;
        QTest::newRow("settings ltr") << false << true;
        QTest::newRow("settings rtl") << true << true;
    }

    void theRightmostHeaderTooltipsStayInsideTheWindow()
    {
        QFETCH(bool, rtl);
        QFETCH(bool, settings);
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        if (settings)
            m_controller->showSettings();
        else
            m_controller->showMain();
        m_window->resize(640, 360);
        setMirrored(rtl);
        QTest::qWait(250);
        QQuickItem *button = item(settings ? "settingsCloseButton"
                                           : "roomHeaderOverflowButton");
        QVERIFY(button && button->isVisible());
        QQmlExpression show(qmlContext(button), button,
                            QStringLiteral("ToolTip.show(ToolTip.text, 5000), "
                                           "ToolTip.toolTip"));
        auto *tip = show.evaluate().value<QObject *>();
        QVERIFY2(!show.hasError(), qPrintable(show.error().toString()));
        QVERIFY(tip);
        QTRY_VERIFY(tip->property("opened").toBool());
        auto *content = tip->property("contentItem").value<QQuickItem *>();
        QVERIFY(content && content->parentItem());
        QQuickItem *box = content->parentItem();
        const QRectF r = box->mapRectToScene(
            QRectF(0, 0, box->width(), box->height()));
        QQmlExpression(qmlContext(button), button,
                       QStringLiteral("ToolTip.hide()")).evaluate();
        setMirrored(false);
        m_controller->showMain();
        QVERIFY2(r.width() > 0
                     && QRectF(0, 0, m_window->width(), m_window->height())
                            .adjusted(-1, -1, 1, 1).contains(r),
                 qPrintable(QStringLiteral("tooltip at %1,%2 %3x%4")
                                .arg(r.x()).arg(r.y()).arg(r.width())
                                .arg(r.height())));
    }

    void anOpenThreadsHeaderFitsTheNarrowestPanel_data()
    {
        QTest::addColumn<bool>("rtl");
        QTest::newRow("ltr") << false;
        QTest::newRow("rtl") << true;
    }

    void anOpenThreadsHeaderFitsTheNarrowestPanel()
    {
        QFETCH(bool, rtl);
        const QString room = QStringLiteral("!general:mock.local");
        m_controller->showMain();
        m_controller->setCurrentRoomId(room);
        m_window->resize(1280, 720);
        const int widthBefore = m_controller->settings()->sidePanelWidth();
        m_controller->settings()->setSidePanelWidth(
            SettingsManager::sidePanelMinWidth());
        setMirrored(rtl);
        QTRY_VERIFY(m_controller->timeline()->rowCount() > 2);
        QString root;
        const auto findRoot = [&] {
            for (const TimelineEvent &e : m_controller->timeline()->events())
                if (e.isThreadRoot
                    || e.body == QLatin1String("Anyone up for a mock deploy tomorrow?"))
                    root = e.eventId;
            return !root.isEmpty();
        };
        QTRY_VERIFY2(findRoot(), "the room has no thread to open");
        ThreadController *thread = m_controller->thread();
        thread->openList(room);
        thread->openThread(room, root);
        QTRY_VERIFY(thread->active());
        QTRY_VERIFY(thread->followSupported());
        QTest::qWait(300);

        QQuickItem *panel = item("threadPanel");
        QQuickItem *close = item("threadCloseButton");
        QVERIFY(panel && close && close->parentItem());
        QQuickItem *header = close->parentItem()->parentItem();
        QVERIFY(header);
        const qreal panelWidth = panel->width();
        const QRectF band = header->mapRectToScene(
            QRectF(0, 0, header->width(), header->height()));
        QList<QQuickItem *> parts{ close };
        for (QQuickItem *c : close->parentItem()->childItems().first()->childItems())
            if (c->isVisible() && c->width() > 0)
                parts << c;
        QStringList faults;
        const auto name = [](QQuickItem *it) {
            return it->objectName().isEmpty()
                ? QString::fromLatin1(it->metaObject()->className())
                : it->objectName();
        };
        const auto box = [](QQuickItem *it) {
            return it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        };
        for (int i = 0; i < parts.size(); ++i) {
            const QRectF a = box(parts.at(i));
            if (!band.adjusted(-0.5, -0.5, 0.5, 0.5).contains(a))
                faults << name(parts.at(i)) + QStringLiteral(" leaves the header");
            const QByteArray cls = parts.at(i)->metaObject()->className();
            if (cls.startsWith("IconButton") && (qAbs(a.width() - 34) > 0.5
                                                 || qAbs(a.height() - 34) > 0.5))
                faults << name(parts.at(i))
                              + QStringLiteral(" squeezed to %1x%2")
                                    .arg(a.width()).arg(a.height());
            if (cls.contains("Label")
                && parts.at(i)->width() + 0.5 < parts.at(i)->implicitWidth()
                && parts.at(i)->property("elide").toInt() == 0)
                faults << name(parts.at(i)) + QStringLiteral(" is cut, not elided");
            for (int j = i + 1; j < parts.size(); ++j) {
                const QRectF o = a.intersected(box(parts.at(j)));
                if (o.width() > 0.5 && o.height() > 0.5)
                    faults << QStringLiteral("%1 overlaps %2 by %3 px")
                                  .arg(name(parts.at(i)), name(parts.at(j)))
                                  .arg(o.width());
            }
        }
        int reachable = 0;
        QObject *more = m_window->findChild<QObject *>(
            QStringLiteral("threadHeaderOverflowMenu"));
        if (more) {
            QMetaObject::invokeMethod(more, "open");
            QTRY_VERIFY(more->property("opened").toBool());
        }
        for (const char *action : { "threadFollowButton", "threadMarkReadButton",
                                    "threadPlacementButton" }) {
            QQuickItem *inline_ = item(action);
            QObject *row = more ? more->findChild<QObject *>(
                                      QLatin1String(action) + QStringLiteral("Item"))
                                : nullptr;
            if ((inline_ && inline_->isVisible())
                || (row && row->property("visible").toBool()
                    && item("threadHeaderOverflowButton")
                    && item("threadHeaderOverflowButton")->isVisible()))
                ++reachable;
        }
        if (more) {
            QMetaObject::invokeMethod(more, "close");
            QTRY_VERIFY(!more->property("visible").toBool());
        }

        thread->close();
        thread->closeList();
        m_controller->settings()->setSidePanelWidth(widthBefore);
        setMirrored(false);
        QTest::qWait(150);
        QCOMPARE(qRound(panelWidth), 260);
        QVERIFY2(parts.size() >= 5, "the header shows less than it should");
        QCOMPARE(reachable, 3);
        QVERIFY2(faults.isEmpty(), qPrintable(faults.join(QStringLiteral("; "))));
    }

    void theSearchFiltersFitTheNarrowestPanel_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("rtl");
        for (int w : { 260, 320 })
            for (bool rtl : { false, true })
                QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(w).arg(
                    QLatin1String(rtl ? "rtl" : "ltr"))))
                    << w << rtl;
    }

    void theSearchFiltersFitTheNarrowestPanel()
    {
        QFETCH(int, width);
        QFETCH(bool, rtl);
        m_controller->showMain();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        m_window->resize(1280, 720);
        const int widthBefore = m_controller->settings()->sidePanelWidth();
        m_controller->settings()->setSidePanelWidth(width);
        setMirrored(rtl);
        QTest::qWait(200);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleSearchPanel"));
        QTest::qWait(300);
        QQuickItem *panel = item("roomSearchPanel");
        QVERIFY(panel && panel->isVisible());
        QVERIFY(QMetaObject::invokeMethod(panel, "beginFilters"));

        QStringList faults;
        int controlCount = 0;
        const qreal panelWidth = panel->width();
        for (const char *section : { "", "from" }) {
            panel->setProperty("memberSection", QString::fromLatin1(section));
            QTest::qWait(300);
            QList<QQuickItem *> controls;
            QStringList found;
            filterPanelFaults(panel, panel, -1e9, 1e9, found, controls);
            controlCount = qMax(controlCount, int(controls.size()));
            const auto box = [panel](QQuickItem *it) {
                return panel->mapRectFromItem(
                    it, QRectF(0, 0, it->width(), it->height()));
            };
            const auto within = [](QQuickItem *a, QQuickItem *b) {
                for (QQuickItem *p = b; p; p = p->parentItem())
                    if (p == a)
                        return true;
                return false;
            };
            for (int i = 0; i < controls.size(); ++i)
                for (int j = i + 1; j < controls.size(); ++j) {
                    QQuickItem *a = controls.at(i);
                    QQuickItem *b = controls.at(j);
                    if (within(a, b) || within(b, a))
                        continue;
                    const QRectF o = box(a).intersected(box(b));
                    if (o.width() > 0.5 && o.height() > 0.5)
                        found << QStringLiteral("%1 overlaps %2")
                                     .arg(QString::fromLatin1(a->metaObject()->className()),
                                          QString::fromLatin1(b->metaObject()->className()));
                }
            for (const QString &f : found)
                faults << QStringLiteral("[%1] %2").arg(
                    QLatin1String(*section ? section : "collapsed"), f);
        }

        panel->setProperty("memberSection", QString());
        panel->setProperty("filterEditing", false);
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleSearchPanel"));
        m_controller->settings()->setSidePanelWidth(widthBefore);
        setMirrored(false);
        QTest::qWait(150);
        QCOMPARE(qRound(panelWidth), width);
        // Header ×, two member buttons, two date fields, six types, pinned,
        // three footer buttons.
        QVERIFY2(controlCount >= 15,
                 qPrintable(QStringLiteral("only %1 controls measured")
                                .arg(controlCount)));
        QVERIFY2(faults.isEmpty(), qPrintable(faults.join(QStringLiteral("; "))));
    }
};

QTEST_MAIN(SmallWindowQmlTest)
#include "SmallWindowQmlTest.moc"
