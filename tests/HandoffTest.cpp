#include "app/AppController.h"
#include "privacy/Handoff.h"
#include "auth/AuthManager.h"
#include "models/AttachmentQueueModel.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace lightning::handoff;

namespace {

const QString kPermalink = QStringLiteral("https://matrix.to/#/!room:hs.example/$event?via=hs.example");

QVariantMap taskFields(const QString &notes, bool includeLink = false,
                       const QString &link = kPermalink)
{
    return {{QStringLiteral("kind"), QStringLiteral("task")},
            {QStringLiteral("title"), QStringLiteral("Follow up")},
            {QStringLiteral("notes"), notes},
            {QStringLiteral("link"), link},
            {QStringLiteral("includeLink"), includeLink},
            {QStringLiteral("uidSeed"), QStringLiteral("!room:hs.example\n$event")}};
}

QVariantMap eventFields(const QString &date, const QString &time, int minutes = 60)
{
    QVariantMap map = taskFields(QStringLiteral("Planning call"));
    map[QStringLiteral("kind")] = QStringLiteral("event");
    map[QStringLiteral("date")] = date;
    map[QStringLiteral("time")] = time;
    map[QStringLiteral("durationMinutes")] = minutes;
    return map;
}

// Both forms that leave: the clipboard text and the saved file.
QStringList payloads(const QVariantMap &map)
{
    const Fields f = fieldsFrom(map, QTimeZone::utc());
    return {plainText(f), ics(f, QDateTime::currentDateTimeUtc())};
}

QString line(const QString &ics, const QString &name)
{
    for (const QString &l : ics.split(QStringLiteral("\r\n"))) {
        if (l.startsWith(name + QLatin1Char(':')))
            return l.mid(name.size() + 1);
    }
    return {};
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    QFile f(dir.filePath(name));
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(bytes);
    return f.fileName();
}

}

class HandoffTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;

    static QString loginAndFirstRoom(AppController &controller)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"), QStringLiteral("unused"));
        if (!loginSpy.wait(5000))
            return {};
        for (int i = 0; i < 50 && controller.roomList()->rowCount() < 1; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() < 1)
            return {};
        return controller.roomList()
            ->data(controller.roomList()->index(0, 0), RoomListModel::RoomIdRole)
            .toString();
    }

    static QObject *loadDialog(QQmlEngine &engine, const QByteArray &type,
                               std::unique_ptr<QObject> &window)
    {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\nimport MatrixClient\n"
                          "ApplicationWindow { width: 800; height: 700; visible: true\n"
                          "  property alias dialog: d\n  " + type + " { id: d } }",
                          QUrl());
        window.reset(component.create());
        if (!window)
            qWarning() << component.errorString();
        return window ? window->property("dialog").value<QObject *>() : nullptr;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", m_home.filePath(QStringLiteral("c")).toUtf8());
        qputenv("XDG_DATA_HOME", m_home.filePath(QStringLiteral("d")).toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("handoff-test"));
    }

    void anAccessTokenNeverLeaves()
    {
        const QString notes = QStringLiteral(
            "see https://hs.example/_matrix/client/v3/sync?access_token=SECRET1 and "
            "https://other.example/x#access_token=SECRET2 then https://example.org/ok");
        QVariantMap map = taskFields(notes);
        map[QStringLiteral("title")] = QStringLiteral("t https://a.example/?access_token=SECRET3");
        for (const QString &out : payloads(map)) {
            QVERIFY2(!out.contains(QStringLiteral("SECRET")), qPrintable(out));
            QVERIFY2(!out.contains(QStringLiteral("access_token")), qPrintable(out));
            QVERIFY2(out.contains(QStringLiteral("https://example.org/ok")), qPrintable(out));
        }
    }

    void anAuthenticatedMediaUrlNeverLeaves()
    {
        const QString notes = QStringLiteral(
            "photo https://hs.example/_matrix/client/v1/media/download/hs.example/abc "
            "thumb https://hs.example/_matrix/media/v3/thumbnail/hs.example/abc "
            "raw mxc://hs.example/abc");
        for (const QString &out : payloads(taskFields(notes))) {
            QVERIFY2(!out.contains(QStringLiteral("/_matrix/")), qPrintable(out));
            QVERIFY2(!out.contains(QStringLiteral("mxc:")), qPrintable(out));
            QVERIFY2(out.contains(QStringLiteral("photo")), qPrintable(out));
        }
    }

    void aPermalinkLeavesOnlyWhenChosen()
    {
        for (const QString &out : payloads(taskFields(QStringLiteral("n")))) {
            QVERIFY2(!out.contains(QStringLiteral("matrix.to")), qPrintable(out));
            QVERIFY2(!out.contains(QStringLiteral("!room")), qPrintable(out));
        }
        const QStringList chosen = payloads(taskFields(QStringLiteral("n"), true));
        QVERIFY(chosen.at(0).contains(kPermalink));
        QCOMPARE(line(chosen.at(1), QStringLiteral("URL")), kPermalink);

        // Only a permalink rides in the link slot.
        for (const QString &bad : {QStringLiteral("https://evil.example/x"),
                                   QStringLiteral("javascript:alert(1)"),
                                   QStringLiteral("https://matrix.to/#/!r:hs?access_token=x")}) {
            for (const QString &out : payloads(taskFields(QStringLiteral("n"), true, bad)))
                QVERIFY2(!out.contains(bad), qPrintable(out));
        }
    }

    void aReplyCarriesNoTextOfTheMessageItQuotes()
    {
        const QString notes = QStringLiteral(
            "> <@bob:hs.example> the parent is private\n> second line\n\nmy own words");
        for (const QString &out : payloads(taskFields(notes))) {
            QVERIFY2(!out.contains(QStringLiteral("parent is private")), qPrintable(out));
            QVERIFY2(!out.contains(QStringLiteral("second line")), qPrintable(out));
            QVERIFY2(out.contains(QStringLiteral("my own words")), qPrintable(out));
        }
    }

    void anEventCanSendNoInvitation()
    {
        const QString out = payloads(eventFields(QStringLiteral("2026-12-01"),
                                                 QStringLiteral("09:30"))).at(1);
        QVERIFY(out.contains(QStringLiteral("BEGIN:VEVENT")));
        for (const char *prop : {"ORGANIZER", "ATTENDEE", "METHOD", "RSVP"})
            QVERIFY2(!out.contains(QLatin1String(prop)), prop);
        QCOMPARE(line(out, QStringLiteral("DTSTART")), QStringLiteral("20261201T093000Z"));
        QCOMPARE(line(out, QStringLiteral("DTEND")), QStringLiteral("20261201T103000Z"));
        QVERIFY(payloads(eventFields(QStringLiteral("2026-13-40"), QStringLiteral("09:30")))
                    .at(1).isEmpty());
    }

    void localTimeCrossesDaylightSavingCorrectly()
    {
        const QTimeZone vilnius(QByteArrayLiteral("Europe/Vilnius"));
        if (!vilnius.isValid())
            QSKIP("no tz database on this runner");
        const auto start = [&](const QString &date) {
            return line(ics(fieldsFrom(eventFields(date, QStringLiteral("10:00"), 90), vilnius),
                            QDateTime::currentDateTimeUtc()),
                        QStringLiteral("DTSTART"));
        };
        // EET (+2) the day before the 2026 switch, EEST (+3) the day after.
        QCOMPARE(start(QStringLiteral("2026-03-28")), QStringLiteral("20260328T080000Z"));
        QCOMPARE(start(QStringLiteral("2026-03-30")), QStringLiteral("20260330T070000Z"));
    }

    void reExportingTheSameMessageUpdatesInsteadOfDuplicating()
    {
        const QString a = line(payloads(taskFields(QStringLiteral("x"))).at(1), QStringLiteral("UID"));
        const QString b = line(payloads(taskFields(QStringLiteral("edited"))).at(1), QStringLiteral("UID"));
        QVERIFY(!a.isEmpty());
        QCOMPARE(a, b);
        QVariantMap other = taskFields(QStringLiteral("x"));
        other[QStringLiteral("uidSeed")] = QStringLiteral("!room:hs.example\n$other");
        QVERIFY(line(payloads(other).at(1), QStringLiteral("UID")) != a);
        QVERIFY(!a.contains(QStringLiteral("room")));
        QVERIFY(!a.contains(QStringLiteral("event")));
    }

    void icsTextIsEscapedAndFolded()
    {
        QVariantMap map = taskFields(QStringLiteral("a, b; c\\d\nnext ") + QString(120, QChar(0x017E)));
        const QString out = payloads(map).at(1);
        QVERIFY(out.contains(QStringLiteral("a\\, b\\; c\\\\d\\nnext")));
        QVERIFY(out.endsWith(QStringLiteral("END:VCALENDAR\r\n")));
        for (const QString &l : out.split(QStringLiteral("\r\n")))
            QVERIFY2(l.toUtf8().size() <= 75, qPrintable(l));
        QString unfolded = out;
        unfolded.replace(QStringLiteral("\r\n "), QString());
        QVERIFY(unfolded.contains(QString(120, QChar(0x017E))));
    }

    void savingWritesOnlyAnOwnerReadableLocalFile()
    {
        HandoffExporter exporter;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("task.ics"));
        QVERIFY(!exporter.saveFile(QUrl(QStringLiteral("https://example.org/task.ics")),
                                   taskFields(QStringLiteral("n"))));
        QVERIFY(exporter.saveFile(QUrl::fromLocalFile(path), taskFields(QStringLiteral("n"))));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().startsWith("BEGIN:VCALENDAR\r\n"));
#ifdef Q_OS_UNIX
        const auto perms = QFile::permissions(path);
        QVERIFY(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                           | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("task.ics")});
    }

    void copyAsTextPutsOnlyTheScrubbedTextOnTheClipboard()
    {
        HandoffExporter exporter;
        exporter.copyText(taskFields(QStringLiteral("hi mxc://hs.example/abc")));
        const QString text = QGuiApplication::clipboard()->text();
        QVERIFY2(text.startsWith(QStringLiteral("Follow up\n\nhi ")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("mxc:")));
        QVERIFY(!text.contains(QStringLiteral("matrix.to")));
    }

    void anUntrustedInboundFileIsRefused()
    {
        QTemporaryDir dir;
        const QString good = writeFile(dir, QStringLiteral("a.png"), "x");
        QVERIFY(inspectSharedFile(QUrl::fromLocalFile(good)).value(QStringLiteral("ok")).toBool());
        QCOMPARE(inspectSharedFile(QUrl::fromLocalFile(good)).value(QStringLiteral("name")).toString(),
                 QStringLiteral("a.png"));

        QList<QUrl> bad{QUrl(QStringLiteral("https://evil.example/a.png")),
                        QUrl(QStringLiteral("javascript:alert(1)")),
                        QUrl(QStringLiteral("file://evil.example/share/a.png")),
                        QUrl::fromLocalFile(dir.path()),
                        QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.png")))};
#ifdef Q_OS_UNIX
        const QString fifo = dir.filePath(QStringLiteral("pipe"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        bad << QUrl::fromLocalFile(fifo);
#endif
        for (const QUrl &url : bad) {
            const QVariantMap r = inspectSharedFile(url);
            QVERIFY2(!r.value(QStringLiteral("ok")).toBool(), qPrintable(url.toString()));
            QVERIFY(!r.value(QStringLiteral("reason")).toString().isEmpty());
        }
    }

    void theExportDialogLeavesTheLinkOutUntilChosen()
    {
        QQmlEngine engine;
        std::unique_ptr<QObject> window;
        QObject *dialog = loadDialog(engine, "HandoffDialog", window);
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "openFor",
            Q_ARG(QVariant, QStringLiteral("call at 5 https://hs.example/_matrix/media/v3/download/hs.example/x")),
            Q_ARG(QVariant, kPermalink), Q_ARG(QVariant, QStringLiteral("seed"))));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QString preview = dialog->property("previewText").toString();
        QVERIFY2(preview.contains(QStringLiteral("call at 5")), qPrintable(preview));
        QVERIFY2(!preview.contains(QStringLiteral("matrix.to")), qPrintable(preview));
        QVERIFY2(!preview.contains(QStringLiteral("/_matrix/")), qPrintable(preview));
        dialog->setProperty("includeLink", true);
        QVERIFY(dialog->property("previewText").toString().contains(kPermalink));
    }

    void cancellingASharedFileStagesNothing()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndFirstRoom(controller);
        QVERIFY(!roomId.isEmpty());
        controller.openRoom(roomId);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        std::unique_ptr<QObject> window;
        QObject *dialog = loadDialog(engine, "ShareTargetDialog", window);
        QVERIFY(dialog);

        QTemporaryDir dir;
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor",
                                          Q_ARG(QVariant, QUrl::fromLocalFile(dir.path()).toString())));
        QVERIFY(!dialog->property("ready").toBool());

        const QString file = writeFile(dir, QStringLiteral("notes.txt"), "hello");
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor",
                                          Q_ARG(QVariant, QUrl::fromLocalFile(file).toString())));
        QVERIFY(dialog->property("ready").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTest::qWait(50);
        QCOMPARE(controller.composer()->attachments()->rowCount(), 0);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("notes.txt")});
    }

    void aSharedSvgGoesThroughTheAttachmentScreenAndIsNotSent()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndFirstRoom(controller);
        QVERIFY(!roomId.isEmpty());
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        std::unique_ptr<QObject> window;
        QObject *dialog = loadDialog(engine, "ShareTargetDialog", window);
        QVERIFY(dialog);

        QTemporaryDir dir;
        const QString svg = writeFile(dir, QStringLiteral("x.svg"),
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"/>");
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFor",
                                          Q_ARG(QVariant, QUrl::fromLocalFile(svg).toString())));
        QVERIFY(dialog->property("ready").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "stageIn", Q_ARG(QVariant, roomId)));
        QCOMPARE(controller.currentRoomId(), roomId);
        auto *queue = controller.composer()->attachments();
        QTRY_COMPARE(queue->rowCount(), 1);
        const QModelIndex row = queue->index(0, 0);
        QCOMPARE(queue->data(row, AttachmentQueueModel::MimeRole).toString(),
                 QStringLiteral("image/svg+xml"));
        QVERIFY(!queue->data(row, AttachmentQueueModel::PreviewSourceRole).toString()
                     .endsWith(QStringLiteral(".svg")));
        // Queued for review, or refused by the SVG screen; never sending.
        const QString state = queue->data(row, AttachmentQueueModel::StateRole).toString();
        QVERIFY2(state == QLatin1String("queued") || state == QLatin1String("failed"),
                 qPrintable(state));
    }
};

QTEST_MAIN(HandoffTest)
#include "HandoffTest.moc"
