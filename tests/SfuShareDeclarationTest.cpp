#include "calls/CallFrameCryptor.h"
#include "calls/RtcController.h"
#include "calls/SfuCallController.h"
#include "calls/SfuMediaEngine.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/RtcSession.h"

#include <QLoggingCategory>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <gst/gst.h>

namespace {
class DeclaringClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    void sfuAddTrackFor(quint64, int, const QString &cid, const QString &name,
                        int, int, int, bool, bool) override
    {
        if (name == QLatin1String("screen"))
            calls << QStringLiteral("add:") + cid;
    }
    void sfuAddShareWithBackupFor(quint64, const QString &cid,
                                  const QString &backupCid, int, int,
                                  bool) override
    {
        calls << QStringLiteral("backup:") + cid + QLatin1Char('+') + backupCid;
    }
    void sfuLocalDescriptionFor(quint64, int, const QString &kind,
                                const QString &target, const QString &) override
    {
        if (kind == QLatin1String("offer")
            && target == QLatin1String("publisher"))
            calls << QStringLiteral("offer");
    }
    QStringList calls;
};

class SessionClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;
    bool supportsMatrixRtc() const override { return true; }
    quint64 rtcSession(const QString &, bool) override
    {
        return lastSessionOp = ++ops;
    }
    quint64 ops = 1000;
    quint64 lastSessionOp = 0;
};

QString ringOf(const QString &user, const QString &device)
{
    return user + QChar(0x1f) + device;
}

QVariantMap sfuRow(const QString &identity, const QString &sid,
                   bool disconnected = false)
{
    QVariantMap row{{QStringLiteral("identity"), identity},
                    {QStringLiteral("sid"), sid},
                    {QStringLiteral("tracks"), QVariantList()}};
    if (disconnected)
        row.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
    return row;
}

// A connected call with a real engine, an RTC membership view and keys
// delivered the way the to-device handler delivers them.
struct KeyedCall
{
    SessionClient client;
    RtcController rtc;
    SfuCallController call;
    SfuMediaEngine engine;
    const QString room = QStringLiteral("!keys:x");

    KeyedCall()
    {
        rtc.setClient(&client);
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMediaEngine(&engine);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
    }
    void members(const QList<QPair<QString, QString>> &devices)
    {
        RtcSessionData session;
        session.roomId = room;
        for (const auto &device : devices) {
            RtcParticipant member;
            member.userId = device.first;
            member.deviceId = device.second;
            member.rtcIdentity = device.first + QLatin1Char(':') + device.second;
            member.intent = QStringLiteral("audio");
            member.membershipEventId = QStringLiteral("$m") + member.rtcIdentity;
            member.wireFormat = QStringLiteral("session");
            session.participants.append(member);
        }
        rtc.refresh(room);
        Q_EMIT client.rtcSessionReceived(client.lastSessionOp, session);
    }
    void key(const QString &user, const QString &device, int index)
    {
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onMediaKeyReceived", Qt::DirectConnection,
            Q_ARG(QString, room), Q_ARG(QString, user), Q_ARG(QString, device),
            Q_ARG(int, index),
            Q_ARG(QString, QString::fromLatin1(
                               QByteArray(16, char('a' + index)).toBase64()))));
    }
};
}

class SfuShareDeclarationTest : public QObject
{
    Q_OBJECT

private:
    static bool standInAvailable()
    {
        return gst_init_check(nullptr, nullptr, nullptr)
            && SfuMediaEngine::firstMissingElement(
                   {QByteArrayLiteral("openh264enc"),
                    QByteArrayLiteral("h264parse")}).isEmpty();
    }
    static void useHardware(bool on)
    {
        SfuMediaEngine::clearShareHardwareEncoderFailedForTest();
        if (on) {
            qputenv("LIGHTNING_SHARE_ENCODER", "hardware");
            SfuMediaEngine::setShareHardwareEncoderForTest(
                SfuMediaEngine::ShareHardwareEncoder{
                    {"openh264enc", "bitrate=%1 gop-size=%2",
                     "constrained-baseline", false},
                    false});
        } else {
            qunsetenv("LIGHTNING_SHARE_ENCODER");
            SfuMediaEngine::setShareHardwareEncoderForTest(std::nullopt);
        }
    }

private slots:
    void initTestCase()
    {
        qputenv("LIGHTNING_SHARE_GPU", "0");
        QString whyNot;
        if (!SfuMediaEngine::runtimeAvailable(&whyNot))
            QSKIP(qPrintable(whyNot));
    }
    void cleanup() { useHardware(false); }

    void aSoftwareShareDeclaresAPlainTrackBeforeItsOffer()
    {
        useHardware(false);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(client.calls.contains(QStringLiteral("offer")),
                                 10000);
        QCOMPARE(client.calls.size() >= 2, true);
        QVERIFY2(client.calls.first().startsWith(QLatin1String("add:")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        engine.stop();
    }

    void aBackedUpShareDeclaresBothCodecsBeforeItsOffer()
    {
        if (!standInAvailable())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        useHardware(true);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(client.calls.contains(QStringLiteral("offer")),
                                 10000);
        const QString first = client.calls.first();
        QVERIFY2(first.startsWith(QLatin1String("backup:"))
                     && first.endsWith(QLatin1String("-vp8")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        for (const QString &entry : client.calls)
            QVERIFY2(!entry.startsWith(QLatin1String("add:")), qPrintable(entry));
        engine.stop();
    }

    void aClosedSourceNoticeIsWithdrawnByTheNextShareOrAStop_data()
    {
        QTest::addColumn<bool>("byStop");
        QTest::newRow("next share") << false;
        QTest::newRow("stop") << true;
    }

    // Only a call's first share is declared to the mock SFU, so each row is a fresh call.
    void aClosedSourceNoticeIsWithdrawnByTheNextShareOrAStop()
    {
        QFETCH(bool, byStop);
        useHardware(false);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        QSignalSpy notices(&call, &SfuCallController::callFailed);
        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(!client.calls.isEmpty(), 10000);
        QVERIFY2(client.calls.first().startsWith(QLatin1String("add:")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        Q_EMIT engine.publishFailed(client.calls.first().mid(4),
                                    QStringLiteral("screen_share_source_closed"));
        QVERIFY(!call.screenSharing());
        QCOMPARE(notices.size(), 1);
        QVERIFY(!notices.first().first().toString().isEmpty());

        notices.clear();
        if (byStop)
            call.stopScreenShare();
        else
            QVERIFY(call.startScreenShare(0));
        QVERIFY(!notices.isEmpty());
        QCOMPARE(notices.first().first().toString(), QString());
        engine.stop();
    }

    // A failure inside publishVideo() must not re-enter startScreenShare().
    void aBackupThatCannotStartDeclaresNothingAndEndsTheShareAfterwards()
    {
        if (!standInAvailable())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        useHardware(true);
        DeclaringClient client;
        SfuCallController call;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        call.setClient(&client);
        call.setMediaEngine(&engine);
        call.setCallStateForTest(SfuCallController::State::Connected);
        engine.start();
        engine.failNextShareBackupForTest();
        QVERIFY(call.startScreenShare(0));
        QVERIFY2(call.screenSharing(),
                 "the failure was handled inside startScreenShare()");
        QTRY_VERIFY_WITH_TIMEOUT(!call.screenSharing(), 5000);
        QVERIFY(client.calls.isEmpty());
        QVERIFY(SfuMediaEngine::shareHardwareEncoderFailedForTest());

        QVERIFY(call.startScreenShare(0));
        QTRY_VERIFY_WITH_TIMEOUT(client.calls.contains(QStringLiteral("offer")),
                                 10000);
        QVERIFY2(client.calls.first().startsWith(QLatin1String("add:")),
                 qPrintable(client.calls.join(QLatin1Char(' '))));
        engine.stop();
    }

    void sequentialSendersPastTheRingCapStillGetTheirKeys()
    {
        KeyedCall f;
        f.call.setInboundRingGraceMsForTest(0);
        auto *subscriber = new SfuMediaEngine(&f.call);
        f.call.attachSubscriberEngineForTest(1, subscriber);
        const QString a = QStringLiteral("@a:x");
        const QString aDev = QStringLiteral("ADEV");
        f.members({{a, aDev}});
        f.key(a, aDev, 0);
        f.call.ingestParticipantsForTest({sfuRow(a + QStringLiteral(":ADEV"),
                                                 QStringLiteral("PA_A"))});

        for (int i = 0; i < 80; ++i) {
            const QString user = QStringLiteral("@u%1:x").arg(i);
            const QString dev = QStringLiteral("D%1").arg(i);
            const QString identity = user + QLatin1Char(':') + dev;
            f.members({{a, aDev}, {user, dev}});
            f.call.ingestParticipantsForTest(
                {sfuRow(identity, QStringLiteral("PA_%1").arg(i))});
            f.call.ingestSubscriberParticipantsForTest(
                1, {sfuRow(identity, QStringLiteral("PB_%1").arg(i))});
            f.key(user, dev, 0);
            QVERIFY2(f.engine.recvCryptorFor(QStringLiteral("PA_%1").arg(i))
                         ->hasKey(0),
                     qPrintable(QStringLiteral("sender %1 of a long call has "
                                               "no key").arg(i)));
            QVERIFY2(subscriber->recvCryptorFor(QStringLiteral("PB_%1").arg(i))
                         ->hasKey(0),
                     qPrintable(QStringLiteral("sender %1 has no key on the "
                                               "second SFU").arg(i)));
            f.call.ingestParticipantsForTest(
                {sfuRow(identity, QStringLiteral("PA_%1").arg(i), true)});
            f.call.ingestSubscriberParticipantsForTest(
                1, {sfuRow(identity, QStringLiteral("PB_%1").arg(i), true)});
            f.members({{a, aDev}});
        }

        QVERIFY2(f.engine.recvCryptorFor(QStringLiteral("PA_A"))->hasKey(0),
                 "the sender present all along lost its key to a pruning");
        f.key(a, aDev, 1);
        QVERIFY(f.engine.recvCryptorFor(QStringLiteral("PA_A"))->hasKey(1));
        QVERIFY(subscriber->recvCryptorFor(ringOf(a, aDev))->hasKey(1));
        QVERIFY2(f.engine.receiveNameCountForTest() <= 2 * 64 + 4,
                 qPrintable(QString::number(f.engine.receiveNameCountForTest())));
        QVERIFY2(subscriber->receiveNameCountForTest() <= 2 * 64 + 4,
                 qPrintable(QString::number(
                     subscriber->receiveNameCountForTest())));
    }

    void aRecentKeyIsNotRetiredToAdmitAStranger()
    {
        KeyedCall f;
        const QString a = QStringLiteral("@a:x");
        f.members({{a, QStringLiteral("ADEV")}});
        f.key(a, QStringLiteral("ADEV"), 0);
        const QString early = QStringLiteral("@early:x");
        f.key(early, QStringLiteral("E"), 0);
        for (int i = 0; i < 62; ++i)
            f.key(QStringLiteral("@s%1:x").arg(i), QStringLiteral("S"), 0);
        f.members({{a, QStringLiteral("ADEV")}, {early, QStringLiteral("E")}});

        const QString stranger = QStringLiteral("@late:x");
        f.key(stranger, QStringLiteral("L"), 0);
        QVERIFY2(!f.engine.recvCryptorFor(ringOf(stranger, QStringLiteral("L")))
                      ->hasKey(0),
                 "a key from inside the grace period was retired for a "
                 "stranger");
        QVERIFY(f.engine.recvCryptorFor(ringOf(early, QStringLiteral("E")))
                    ->hasKey(0));
        QVERIFY(f.engine.recvCryptorFor(QStringLiteral("@s0:x") + QChar(0x1f)
                                        + QStringLiteral("S"))->hasKey(0));

        f.call.setInboundRingGraceMsForTest(0);
        f.key(stranger, QStringLiteral("L"), 0);
        QVERIFY2(f.engine.recvCryptorFor(ringOf(stranger, QStringLiteral("L")))
                     ->hasKey(0),
                 "departed senders' rings were never retired");
        QVERIFY(f.engine.recvCryptorFor(ringOf(a, QStringLiteral("ADEV")))
                    ->hasKey(0));
        QVERIFY2(f.engine.recvCryptorFor(ringOf(early, QStringLiteral("E")))
                     ->hasKey(0),
                 "a key that arrived before its membership was retired");
    }

    void reconnectsDoNotGrowTheReceiveRingMap()
    {
        KeyedCall f;
        const QString a = QStringLiteral("@a:x");
        const QString identity = QStringLiteral("@a:x:ADEV");
        f.members({{a, QStringLiteral("ADEV")}});
        f.key(a, QStringLiteral("ADEV"), 0);
        QString sid;
        QLoggingCategory::setFilterRules(
            QStringLiteral("lightning.calls.group.warning=false\n"
                           "lightning.calls.group.info=false"));
        for (int i = 0; i < 2000; ++i) {
            const QString previous = sid;
            sid = QStringLiteral("PA_%1").arg(i);
            if (i % 2 && !previous.isEmpty())
                f.call.ingestParticipantsForTest({sfuRow(identity, previous, true)});
            f.call.ingestParticipantsForTest({sfuRow(identity, sid)});
        }
        QLoggingCategory::setFilterRules(QString());
        QVERIFY2(f.engine.receiveNameCountForTest() <= 8,
                 qPrintable(QString::number(f.engine.receiveNameCountForTest())));
        QVERIFY2(f.engine.recvCryptorFor(sid)->hasKey(0),
                 "the active sender lost its key after its stream was rebound");

        const QString b = QStringLiteral("@b:x");
        f.members({{a, QStringLiteral("ADEV")}, {b, QStringLiteral("BDEV")}});
        f.key(b, QStringLiteral("BDEV"), 0);
        f.call.ingestParticipantsForTest(
            {sfuRow(QStringLiteral("@b:x:BDEV"), QStringLiteral("PB_1"))});
        QVERIFY2(f.engine.recvCryptorFor(QStringLiteral("PB_1"))->hasKey(0),
                 "a new sender got a detached ring after the reconnects");
    }

    void bindingAStreamRespectsTheRingCap()
    {
        SfuMediaEngine engine;
        for (int i = 0; i < SfuMediaEngine::kMaxReceiveRings + 50; ++i)
            engine.noteParticipantIdentity(QStringLiteral("PA_%1").arg(i),
                                           QStringLiteral("ring%1").arg(i));
        QCOMPARE(engine.receiveNameCountForTest(),
                 SfuMediaEngine::kMaxReceiveRings);
        engine.setInboundKey(QStringLiteral("ring0"), 0, QByteArray(16, 'k'));
        QVERIFY(engine.recvCryptorFor(QStringLiteral("PA_0"))->hasKey(0));
        engine.retainReceiveNames({QStringLiteral("ring0"),
                                   QStringLiteral("PA_0")});
        QCOMPARE(engine.receiveNameCountForTest(), 2);
        QVERIFY(engine.recvCryptorFor(QStringLiteral("PA_0"))->hasKey(0));
    }
};

QTEST_MAIN(SfuShareDeclarationTest)
#include "SfuShareDeclarationTest.moc"
