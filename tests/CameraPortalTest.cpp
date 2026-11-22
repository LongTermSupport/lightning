
#include "calls/CameraPortal.h"
#include "calls/PortalRequest.h"
#include "calls/ScreenCastPortal.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <unistd.h>

namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kCamera = "org.freedesktop.portal.Camera";
constexpr auto kRequest = "org.freedesktop.portal.Request";
constexpr auto kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr auto kSession = "org.freedesktop.portal.Session";
constexpr auto kProperties = "org.freedesktop.DBus.Properties";

class FakeCameraPortal : public QDBusVirtualObject
{
public:
    enum class Answer {
        InstantGrant,
        InstantDecline,
        LateGrantOnOtherPath,
        AccessError,
        OpenError,
        Hold,
        HoldReply,
        HoldReplyOnOtherPath,
    };
    enum class Cast {
        HoldCreateResponse,
        HoldSelectResponse,
        HoldStartResponse,
    };

    explicit FakeCameraPortal(QDBusConnection bus) : m_bus(bus) {}

    Answer answer = Answer::InstantGrant;
    QString errorName;
    int accessCalls = 0;
    int openCalls = 0;
    int fixtureErrors = 0;

    Cast cast = Cast::HoldSelectResponse;
    QVariantMap castProperties;
    QVariantMap lastSelectOptions;
    int getAllCalls = 0;
    int selectCalls = 0;
    int startCalls = 0;
    QStringList closedRequests;
    QStringList closedSessions;
    QString lastRequestPath;
    QString lastPredictedPath;
    QString lastSessionPath;
    QString caller;
    QDBusMessage heldCall;

    QString introspect(const QString &) const override { return {}; }

    void respond(const QString &path, uint code,
                 const QVariantMap &results = {})
    {
        QDBusMessage response = QDBusMessage::createTargetedSignal(
            caller, path, QString::fromLatin1(kRequest),
            QStringLiteral("Response"));
        response << code << results;
        m_bus.send(response);
    }

    void releaseHeldReply()
    {
        m_bus.send(heldCall.createReply(
            QVariant::fromValue(QDBusObjectPath(lastRequestPath))));
    }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &) override
    {
        if (message.interface() == QLatin1String(kRequest)
            && message.member() == QLatin1String("Close")) {
            closedRequests << message.path();
            m_bus.send(message.createReply());
            return true;
        }
        if (message.interface() == QLatin1String(kSession)
            && message.member() == QLatin1String("Close")) {
            closedSessions << message.path();
            m_bus.send(message.createReply());
            return true;
        }
        if (message.interface() == QLatin1String(kProperties)
            && message.member() == QLatin1String("GetAll")
            && message.arguments().value(0).toString()
                == QLatin1String(kScreenCast)) {
            ++getAllCalls;
            m_bus.send(message.createReply(QVariant::fromValue(castProperties)));
            return true;
        }
        if (message.interface() == QLatin1String(kScreenCast))
            return handleScreenCast(message);
        if (message.interface() != QLatin1String(kCamera))
            return false;

        if (message.member() == QLatin1String("AccessCamera")) {
            ++accessCalls;
            caller = message.service();
            if (answer == Answer::Hold || answer == Answer::HoldReply
                || answer == Answer::HoldReplyOnOtherPath) {
                const QString token =
                    qdbus_cast<QVariantMap>(message.arguments().value(0))
                        .value(QStringLiteral("handle_token")).toString();
                lastPredictedPath =
                    portal::predictedRequestPath(message.service(), token);
                lastRequestPath = answer == Answer::HoldReplyOnOtherPath
                    ? QStringLiteral("/org/freedesktop/portal/desktop/request/"
                                     "legacy/held")
                    : lastPredictedPath;
                if (answer == Answer::Hold) {
                    m_bus.send(message.createReply(
                        QVariant::fromValue(QDBusObjectPath(lastRequestPath))));
                } else {
                    message.setDelayedReply(true);
                    heldCall = message;
                }
                return true;
            }
            if (answer == Answer::AccessError) {
                m_bus.send(message.createErrorReply(
                    errorName, QStringLiteral("Camera access disabled")));
                return true;
            }
            const QVariantMap options =
                qdbus_cast<QVariantMap>(message.arguments().value(0));
            const QString token =
                options.value(QStringLiteral("handle_token")).toString();
            const bool otherPath =
                answer == Answer::LateGrantOnOtherPath;
            const QString requestPath =
                otherPath ? QStringLiteral(
                                "/org/freedesktop/portal/desktop/request/"
                                "legacy/unpredicted")
                          : portal::predictedRequestPath(message.service(),
                                                         token);
            if (requestPath.isEmpty() || !m_bus.send(message.createReply(
                    QVariant::fromValue(QDBusObjectPath(requestPath))))) {
                ++fixtureErrors;
                return false;
            }

            const uint code = answer == Answer::InstantDecline ? 1u : 0u;
            QDBusMessage response = QDBusMessage::createTargetedSignal(
                message.service(), requestPath, QString::fromLatin1(kRequest),
                QStringLiteral("Response"));
            response << code << QVariantMap();
            if (otherPath) {
                QDBusConnection bus = m_bus;
                QTimer::singleShot(200, [bus, response]() mutable {
                    bus.send(response);
                });
            } else {
                m_bus.send(response);
            }
            return true;
        }

        if (message.member() == QLatin1String("OpenPipeWireRemote")) {
            ++openCalls;
            if (answer == Answer::OpenError) {
                m_bus.send(message.createErrorReply(
                    errorName, QStringLiteral("Permission denied")));
                return true;
            }
            int fds[2] = {-1, -1};
            if (::pipe(fds) != 0)
                return false;
            m_bus.send(message.createReply(
                QVariant::fromValue(QDBusUnixFileDescriptor(fds[0]))));
            ::close(fds[0]);
            ::close(fds[1]);
            return true;
        }
        return false;
    }

private:
    bool handleScreenCast(const QDBusMessage &message)
    {
        caller = message.service();
        const QVariantMap options =
            qdbus_cast<QVariantMap>(message.arguments().constLast());
        const QString token =
            options.value(QStringLiteral("handle_token")).toString();
        lastRequestPath = portal::predictedRequestPath(caller, token);
        if (message.member() == QLatin1String("SelectSources")) {
            ++selectCalls;
            lastSelectOptions = options;
        } else if (message.member() == QLatin1String("Start")) {
            ++startCalls;
        } else if (message.member() != QLatin1String("CreateSession")) {
            return false;
        }
        m_bus.send(message.createReply(
            QVariant::fromValue(QDBusObjectPath(lastRequestPath))));
        if (message.member() == QLatin1String("CreateSession")) {
            lastSessionPath =
                QStringLiteral("/org/freedesktop/portal/desktop/session/%1")
                    .arg(options.value(QStringLiteral("session_handle_token"))
                             .toString());
            if (cast != Cast::HoldCreateResponse)
                respond(lastRequestPath, 0,
                        {{QStringLiteral("session_handle"), lastSessionPath}});
        } else if (message.member() == QLatin1String("SelectSources")
                   && cast != Cast::HoldSelectResponse) {
            respond(lastRequestPath, 0);
        }
        return true;
    }

    QDBusConnection m_bus;
};

}

class CameraPortalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void thePredictedPathFollowsThePortalSpec();
    void anInstantGrantIsNotLost();
    void anInstantDeclineIsReported();
    void aPortalThatUsesAnotherPathIsStillFollowed();
    void aRefusalIsNeverMistakenForAMissingPortal();
    void aRefusalOverTheBusIsReportedAsNotAllowed_data();
    void aRefusalOverTheBusIsReportedAsNotAllowed();
    void cancellingAPendingCameraRequestClosesItOnce_data();
    void cancellingAPendingCameraRequestClosesItOnce();
    void aCameraRequestThatTimesOutIsClosed();
    void aSessionCreatedAfterCancellingIsClosed();
    void cancellingAnOpenPickerClosesItsRequestAndSession();

private:
    QProcess m_daemon;
    FakeCameraPortal *m_fake = nullptr;
};

void CameraPortalTest::initTestCase()
{
    const QString daemon = QStandardPaths::findExecutable("dbus-daemon");
    if (daemon.isEmpty())
        QSKIP("dbus-daemon not found; this suite needs a private bus");

    QStringList args{QStringLiteral("--nofork"),
                     QStringLiteral("--print-address=1")};
    const QString packaged = QFileInfo(daemon).absolutePath()
        + QStringLiteral("/../share/dbus-1/session.conf");
    if (QFileInfo::exists(packaged))
        args << QStringLiteral("--config-file=%1")
                    .arg(QFileInfo(packaged).canonicalFilePath());
    else
        args << QStringLiteral("--session");
    m_daemon.start(daemon, args);
    QVERIFY(m_daemon.waitForStarted(5000));
    QVERIFY(m_daemon.waitForReadyRead(5000));
    const QByteArray address = m_daemon.readLine().trimmed();
    QVERIFY(!address.isEmpty());

    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    QVERIFY(QDBusConnection::sessionBus().isConnected());

    QDBusConnection fakeBus = QDBusConnection::connectToBus(
        QString::fromLatin1(address), QStringLiteral("fake-portal"));
    QVERIFY(fakeBus.isConnected());
    QVERIFY(fakeBus.registerService(QString::fromLatin1(kPortalService)));
    m_fake = new FakeCameraPortal(fakeBus);
    QVERIFY(fakeBus.registerVirtualObject(QString::fromLatin1(kPortalPath),
                                          m_fake, QDBusConnection::SubPath));
}

void CameraPortalTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-portal"));
    delete m_fake;
    m_fake = nullptr;
    if (m_daemon.state() != QProcess::NotRunning) {
        m_daemon.kill();
        m_daemon.waitForFinished(3000);
    }
}

void CameraPortalTest::thePredictedPathFollowsThePortalSpec()
{
    QCOMPARE(portal::predictedRequestPath(QStringLiteral(":1.189"),
                                          QStringLiteral("lightprobe27560")),
             QStringLiteral("/org/freedesktop/portal/desktop/request/1_189/"
                            "lightprobe27560"));
    QVERIFY(portal::predictedRequestPath(QString(), QStringLiteral("t"))
                .isEmpty());
    QVERIFY(portal::predictedRequestPath(QStringLiteral(":1.2"), QString())
                .isEmpty());
}

void CameraPortalTest::anInstantGrantIsNotLost()
{
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    QSignalSpy failed(&portal, &CameraPortal::failed);
    QSignalSpy cancelled(&portal, &CameraPortal::cancelled);
    m_fake->answer = FakeCameraPortal::Answer::InstantGrant;
    const int opensBefore = m_fake->openCalls;

    portal.requestAccess();
    const int fixtureErrorsBefore = m_fake->fixtureErrors;

    QVERIFY2(ready.wait(5000),
             "the portal granted the camera in the same breath as its reply, "
             "and CameraPortal missed the grant");
    QCOMPARE(m_fake->fixtureErrors, fixtureErrorsBefore);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(cancelled.count(), 0);
    QCOMPARE(m_fake->openCalls, opensBefore + 1);
    const int fd = ready.takeFirst().at(0).toInt();
    QVERIFY(fd >= 0);
    ::close(fd);
}

void CameraPortalTest::anInstantDeclineIsReported()
{
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    QSignalSpy cancelled(&portal, &CameraPortal::cancelled);
    m_fake->answer = FakeCameraPortal::Answer::InstantDecline;

    portal.requestAccess();

    QVERIFY2(cancelled.wait(5000),
             "a stored 'no' arrives as fast as a stored 'yes' and must not "
             "be lost either");
    QCOMPARE(ready.count(), 0);
}

void CameraPortalTest::aPortalThatUsesAnotherPathIsStillFollowed()
{
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    m_fake->answer = FakeCameraPortal::Answer::LateGrantOnOtherPath;

    portal.requestAccess();

    QVERIFY2(ready.wait(5000),
             "a portal older than 0.9 returns a path nobody predicted; the "
             "returned path must still be followed");
    ::close(ready.takeFirst().at(0).toInt());
}

void CameraPortalTest::aRefusalIsNeverMistakenForAMissingPortal()
{
    const auto access = [](const char *name) {
        return CameraPortal::failureCategory(QString::fromLatin1(name), false);
    };
    const auto open = [](const char *name) {
        return CameraPortal::failureCategory(QString::fromLatin1(name), true);
    };
    for (const char *refusal : { "org.freedesktop.portal.Error.NotAllowed",
                                 "org.freedesktop.portal.Error.Cancelled",
                                 "org.freedesktop.DBus.Error.AccessDenied" }) {
        QCOMPARE(access(refusal), QStringLiteral("not_allowed"));
        QCOMPARE(open(refusal), QStringLiteral("not_allowed"));
    }
    for (const char *absent :
         { "org.freedesktop.DBus.Error.ServiceUnknown",
           "org.freedesktop.DBus.Error.UnknownMethod",
           "org.freedesktop.DBus.Error.UnknownInterface",
           "org.freedesktop.DBus.Error.UnknownObject",
           "org.freedesktop.DBus.Error.NameHasNoOwner" }) {
        QCOMPARE(access(absent), QStringLiteral("no_portal"));
        QCOMPARE(open(absent), QStringLiteral("no_portal"));
    }
    QCOMPARE(access("org.freedesktop.portal.Error.Failed"),
             QStringLiteral("portal_failed"));
    QCOMPARE(open("org.freedesktop.portal.Error.Failed"),
             QStringLiteral("no_pipewire_remote"));
    QCOMPARE(access("org.freedesktop.DBus.Error.NoReply"),
             QStringLiteral("portal_failed"));
}

void CameraPortalTest::aRefusalOverTheBusIsReportedAsNotAllowed_data()
{
    QTest::addColumn<int>("answer");
    QTest::addColumn<QString>("errorName");
    QTest::addColumn<QString>("category");
    QTest::newRow("lockdown at AccessCamera")
        << int(FakeCameraPortal::Answer::AccessError)
        << QStringLiteral("org.freedesktop.portal.Error.NotAllowed")
        << QStringLiteral("not_allowed");
    QTest::newRow("access denied at AccessCamera")
        << int(FakeCameraPortal::Answer::AccessError)
        << QStringLiteral("org.freedesktop.DBus.Error.AccessDenied")
        << QStringLiteral("not_allowed");
    QTest::newRow("no Camera interface")
        << int(FakeCameraPortal::Answer::AccessError)
        << QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod")
        << QStringLiteral("no_portal");
    QTest::newRow("refused at OpenPipeWireRemote")
        << int(FakeCameraPortal::Answer::OpenError)
        << QStringLiteral("org.freedesktop.portal.Error.NotAllowed")
        << QStringLiteral("not_allowed");
    QTest::newRow("no PipeWire at OpenPipeWireRemote")
        << int(FakeCameraPortal::Answer::OpenError)
        << QStringLiteral("org.freedesktop.portal.Error.Failed")
        << QStringLiteral("no_pipewire_remote");
}

void CameraPortalTest::aRefusalOverTheBusIsReportedAsNotAllowed()
{
    QFETCH(int, answer);
    QFETCH(QString, errorName);
    QFETCH(QString, category);
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    QSignalSpy failed(&portal, &CameraPortal::failed);
    QSignalSpy cancelled(&portal, &CameraPortal::cancelled);
    m_fake->answer = static_cast<FakeCameraPortal::Answer>(answer);
    m_fake->errorName = errorName;

    portal.requestAccess();

    QVERIFY(failed.wait(5000));
    QCOMPARE(failed.takeFirst().at(0).toString(), category);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(cancelled.count(), 0);
    QVERIFY(!portal.busy());
    m_fake->answer = FakeCameraPortal::Answer::InstantGrant;
}

void CameraPortalTest::cancellingAPendingCameraRequestClosesItOnce_data()
{
    QTest::addColumn<int>("answer");
    QTest::addColumn<bool>("cancelBeforeReply");
    QTest::addColumn<bool>("byDestruction");
    QTest::newRow("after the reply") << int(FakeCameraPortal::Answer::Hold)
                                     << false << false;
    QTest::newRow("before the reply")
        << int(FakeCameraPortal::Answer::HoldReply) << true << false;
    QTest::newRow("before a reply naming another path")
        << int(FakeCameraPortal::Answer::HoldReplyOnOtherPath) << true << false;
    QTest::newRow("after a reply naming another path")
        << int(FakeCameraPortal::Answer::HoldReplyOnOtherPath) << false
        << false;
    QTest::newRow("by destruction") << int(FakeCameraPortal::Answer::Hold)
                                    << false << true;
}

void CameraPortalTest::cancellingAPendingCameraRequestClosesItOnce()
{
    QFETCH(int, answer);
    QFETCH(bool, cancelBeforeReply);
    QFETCH(bool, byDestruction);
    m_fake->answer = static_cast<FakeCameraPortal::Answer>(answer);
    m_fake->closedRequests.clear();
    const int accessBefore = m_fake->accessCalls;
    const int opensBefore = m_fake->openCalls;
    auto *portal = new CameraPortal;
    QSignalSpy ready(portal, &CameraPortal::ready);

    portal->requestAccess();
    QTRY_COMPARE(m_fake->accessCalls, accessBefore + 1);
    const bool heldReply = m_fake->answer != FakeCameraPortal::Answer::Hold;
    if (heldReply && !cancelBeforeReply) {
        m_fake->releaseHeldReply();
        QTest::qWait(100);
    }
    if (byDestruction) {
        delete portal;
        portal = nullptr;
    } else {
        portal->cancel();
        portal->cancel();
    }
    if (heldReply && cancelBeforeReply)
        m_fake->releaseHeldReply();

    QTRY_COMPARE(m_fake->closedRequests.count(m_fake->lastRequestPath), 1);
    m_fake->respond(m_fake->lastRequestPath, 0);
    QTest::qWait(200);
    QCOMPARE(m_fake->closedRequests.count(m_fake->lastRequestPath), 1);
    if (m_fake->lastRequestPath != m_fake->lastPredictedPath
        && !cancelBeforeReply) {
        QCOMPARE(m_fake->closedRequests.count(m_fake->lastPredictedPath), 0);
    }
    QCOMPARE(m_fake->openCalls, opensBefore);
    QCOMPARE(ready.count(), 0);
    delete portal;
    m_fake->answer = FakeCameraPortal::Answer::InstantGrant;
}

void CameraPortalTest::aCameraRequestThatTimesOutIsClosed()
{
    m_fake->answer = FakeCameraPortal::Answer::Hold;
    m_fake->closedRequests.clear();
    CameraPortal portal;
    portal.setRequestTimeoutMsForTest(300);
    QSignalSpy failed(&portal, &CameraPortal::failed);

    portal.requestAccess();
    QVERIFY(failed.wait(5000));
    QCOMPARE(failed.takeFirst().at(0).toString(), QStringLiteral("timeout"));
    QTRY_COMPARE(m_fake->closedRequests.count(m_fake->lastRequestPath), 1);
    m_fake->answer = FakeCameraPortal::Answer::InstantGrant;
}

void CameraPortalTest::aSessionCreatedAfterCancellingIsClosed()
{
    m_fake->lastSessionPath.clear();
    m_fake->cast = FakeCameraPortal::Cast::HoldCreateResponse;
    m_fake->castProperties = {{QStringLiteral("AvailableCursorModes"), 2u}};
    m_fake->closedRequests.clear();
    m_fake->closedSessions.clear();
    const int selectsBefore = m_fake->selectCalls;
    const int startsBefore = m_fake->startCalls;
    ScreenCastPortal portal;
    QSignalSpy ready(&portal, &ScreenCastPortal::ready);

    portal.requestShare(ScreenCastPortal::Monitor | ScreenCastPortal::Window);
    QTRY_VERIFY(!m_fake->lastSessionPath.isEmpty());
    QTest::qWait(100);
    const QString createRequest = m_fake->lastRequestPath;
    const QString session = m_fake->lastSessionPath;
    portal.cancel();
    m_fake->respond(createRequest, 0,
                    {{QStringLiteral("session_handle"), session}});

    QTRY_COMPARE(m_fake->closedSessions.count(session), 1);
    QCOMPARE(m_fake->closedRequests.count(createRequest), 1);
    QTest::qWait(200);
    QCOMPARE(m_fake->selectCalls, selectsBefore);
    QCOMPARE(m_fake->startCalls, startsBefore);
    QCOMPARE(ready.count(), 0);
}

void CameraPortalTest::cancellingAnOpenPickerClosesItsRequestAndSession()
{
    m_fake->lastSessionPath.clear();
    m_fake->cast = FakeCameraPortal::Cast::HoldStartResponse;
    m_fake->castProperties = {{QStringLiteral("AvailableCursorModes"), 2u}};
    m_fake->closedRequests.clear();
    m_fake->closedSessions.clear();
    const int startsBefore = m_fake->startCalls;
    ScreenCastPortal portal;

    portal.requestShare(ScreenCastPortal::Monitor | ScreenCastPortal::Window);
    QTRY_COMPARE(m_fake->startCalls, startsBefore + 1);
    QTest::qWait(100);
    const QString picker = m_fake->lastRequestPath;
    portal.cancel();
    portal.cancel();

    QTRY_COMPARE(m_fake->closedRequests.count(picker), 1);
    QTRY_COMPARE(m_fake->closedSessions.count(m_fake->lastSessionPath), 1);
    QTest::qWait(200);
    QCOMPARE(m_fake->closedRequests.count(picker), 1);
    QCOMPARE(m_fake->closedSessions.count(m_fake->lastSessionPath), 1);
}

QTEST_GUILESS_MAIN(CameraPortalTest)
#include "CameraPortalTest.moc"
