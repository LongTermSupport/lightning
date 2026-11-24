#include "app/FollowUpController.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
const QString kRoom = QStringLiteral("!room:example.org");
const QString kOther = QStringLiteral("!other:example.org");

QJsonObject readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
}

class FollowUpControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void marksPersistIdsStateAndTimeOnly()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("acct/follow-ups.json"));
        FollowUpController c;
        c.setClock([] { return qint64(1000); });
        c.openFor(path);
        c.setState(kRoom, QStringLiteral("$a"), QString(), QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("$b"), QStringLiteral("$root"),
                   QStringLiteral("waiting"));
        QCOMPARE(c.stateOf(kRoom, QStringLiteral("$a")), QStringLiteral("reply"));
        QCOMPARE(c.stateOf(kRoom, QStringLiteral("$b")), QStringLiteral("waiting"));

        const QJsonObject root = readFile(path);
        QCOMPARE(root.keys(), (QStringList{ QStringLiteral("items"), QStringLiteral("version") }));
        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 2);
        const QStringList allowed{ QStringLiteral("at"), QStringLiteral("event"),
                                   QStringLiteral("room"), QStringLiteral("root"),
                                   QStringLiteral("state") };
        for (const QJsonValue &v : items)
            for (const QString &k : v.toObject().keys())
                QVERIFY2(allowed.contains(k), qPrintable(k));
        QCOMPARE(items.at(1).toObject().value(QStringLiteral("root")).toString(),
                 QStringLiteral("$root"));

        FollowUpController again;
        again.openFor(path);
        QCOMPARE(again.openCount(), 2);
        QCOMPARE(again.stateOf(kRoom, QStringLiteral("$b")), QStringLiteral("waiting"));
        QCOMPARE(again.items().first().toMap().value(QStringLiteral("threadRootId")).toString(),
                 QStringLiteral("$root"));
    }

    void doneRemovesTheMarkAndTheLastOneTheFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("follow-ups.json"));
        FollowUpController c;
        c.openFor(path);
        c.setState(kRoom, QStringLiteral("$a"), QString(), QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("$a"), QString(), QStringLiteral("waiting"));
        QCOMPARE(c.openCount(), 1);
        QCOMPARE(c.stateOf(kRoom, QStringLiteral("$a")), QStringLiteral("waiting"));
        QVERIFY(QFile::exists(path));
        c.markDone(kRoom, QStringLiteral("$a"));
        QCOMPARE(c.stateOf(kRoom, QStringLiteral("$a")), QString());
        QCOMPARE(c.openCount(), 0);
        QVERIFY(!QFile::exists(path));
    }

    void compositeAndMalformedIdsAreRefused()
    {
        QTemporaryDir dir;
        FollowUpController c;
        c.openFor(dir.filePath(QStringLiteral("follow-ups.json")));
        const QString threadTimeline = kRoom + QStringLiteral("\x1f" "thread" "\x1f") + QStringLiteral("$root");
        const QString contextTimeline = kRoom + QStringLiteral("\x1f" "ctx" "\x1f") + QStringLiteral("$e");
        c.setState(threadTimeline, QStringLiteral("$a"), QString(), QStringLiteral("reply"));
        c.setState(contextTimeline, QStringLiteral("$a"), QString(), QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("a"), QString(), QStringLiteral("reply"));
        c.setState(QStringLiteral("room"), QStringLiteral("$a"), QString(), QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("$a"), QStringLiteral("root"), QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("$a"), QString(), QStringLiteral("done"));
        c.setState(kRoom, QStringLiteral("$a"), QString(), QString());
        QCOMPARE(c.openCount(), 0);
    }

    void theListIsBounded()
    {
        QTemporaryDir dir;
        FollowUpController c;
        c.openFor(dir.filePath(QStringLiteral("follow-ups.json")));
        for (int i = 0; i < FollowUpController::kMaxEntries + 5; ++i)
            c.setState(kRoom, QStringLiteral("$e%1").arg(i), QString(), QStringLiteral("reply"));
        QCOMPARE(c.openCount(), FollowUpController::kMaxEntries);
    }

    void aReplyOffersCompletionButNeverCompletesIt()
    {
        QTemporaryDir dir;
        FollowUpController c;
        c.openFor(dir.filePath(QStringLiteral("follow-ups.json")));
        c.setState(kRoom, QStringLiteral("$ask"), QString(), QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("$inThread"), QStringLiteral("$root"),
                   QStringLiteral("reply"));
        c.setState(kRoom, QStringLiteral("$theirs"), QString(), QStringLiteral("waiting"));
        QSignalSpy offered(&c, &FollowUpController::completionOffered);

        c.noteOwnMessage(kOther, QStringLiteral("$ask"), QString());
        c.noteOwnMessage(kRoom, QStringLiteral("$theirs"), QString());
        c.noteOwnMessage(kRoom, QString(), QString());
        QCOMPARE(offered.size(), 0);

        c.noteOwnMessage(kRoom, QStringLiteral("$ask"), QString());
        QCOMPARE(offered.size(), 1);
        QVERIFY(c.replied(kRoom, QStringLiteral("$ask")));
        QCOMPARE(c.stateOf(kRoom, QStringLiteral("$ask")), QStringLiteral("reply"));

        c.noteOwnMessage(kRoom, QString(), QStringLiteral("$root"));
        QCOMPARE(offered.size(), 2);
        QCOMPARE(offered.last().at(1).toString(), QStringLiteral("$inThread"));
        QCOMPARE(c.openCount(), 3);
        QVERIFY(!c.replied(kRoom, QStringLiteral("$theirs")));

        c.markDone(kRoom, QStringLiteral("$ask"));
        QVERIFY(!c.replied(kRoom, QStringLiteral("$ask")));
        QCOMPARE(c.openCount(), 2);
    }

    void aClosedControllerHoldsAndWritesNothing()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("follow-ups.json"));
        FollowUpController c;
        c.openFor(path);
        c.setState(kRoom, QStringLiteral("$a"), QString(), QStringLiteral("reply"));
        c.close();
        QCOMPARE(c.openCount(), 0);
        QCOMPARE(c.filePath(), QString());
        c.setState(kRoom, QStringLiteral("$b"), QString(), QStringLiteral("reply"));
        QCOMPARE(c.openCount(), 0);
        QCOMPARE(readFile(path).value(QStringLiteral("items")).toArray().size(), 1);
    }
};

QTEST_GUILESS_MAIN(FollowUpControllerTest)
#include "FollowUpControllerTest.moc"
