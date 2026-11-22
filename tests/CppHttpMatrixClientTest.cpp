#include "app/SettingsManager.h"
#include "matrix/CppHttpMatrixClient.h"
#include "storage/CacheStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <functional>
#include <utility>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");
const QString kAlice = QStringLiteral("@a:example.org");
const QString kBob = QStringLiteral("@b:example.org");

struct Request {
    QByteArray method;
    QByteArray path;
    QByteArray auth;
};

// A loopback homeserver. A request whose path starts with a held prefix is
// answered only when release() is called for that prefix.
class FakeHomeserver : public QObject
{
public:
    QList<Request> seen;
    QSet<QByteArray> held;
    std::function<void(QTcpSocket *, const Request &)> whoami;

    quint16 listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket] { serve(socket); });
                connect(socket, &QTcpSocket::disconnected, socket,
                        &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0) ? m_server.serverPort() : 0;
    }

    static void reply(QTcpSocket *socket, int code, const QByteArray &body)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + QByteArray::number(body.size())
                      + "\r\n\r\n" + body);
    }

    void release(const QByteArray &prefix)
    {
        held.remove(prefix);
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &[socket, request] : waiting) {
            if (request.path.startsWith(prefix) && socket)
                answer(socket, request);
            else
                m_waiting.append({ socket, request });
        }
    }

    int count(const QByteArray &prefix, const QByteArray &auth = {}) const
    {
        int n = 0;
        for (const auto &r : seen)
            n += r.path.startsWith(prefix) && (auth.isEmpty() || r.auth == auth);
        return n;
    }

private:
    void answer(QTcpSocket *socket, const Request &r)
    {
        if (r.path == "/_matrix/client/v3/account/whoami") {
            if (whoami)
                return whoami(socket, r);
            if (r.auth == "Bearer tokA")
                return reply(socket, 200, R"({"user_id":"@a:example.org"})");
            if (r.auth == "Bearer tokB")
                return reply(socket, 200, R"({"user_id":"@b:example.org"})");
            return reply(socket, 401, R"({"errcode":"M_UNKNOWN_TOKEN","error":"x"})");
        }
        if (r.path.startsWith("/_matrix/media/v3/upload"))
            return reply(socket, 200, R"({"content_uri":"mxc://example.org/up"})");
        if (r.path.contains("/send/"))
            return reply(socket, 200, R"({"event_id":"$sent"})");
        if (r.path.endsWith("/messages")) {
            return reply(socket, 200,
                         "{\"end\":\"t2\",\"chunk\":[{\"type\":\"m.room.message\","
                         "\"event_id\":\"$old\",\"sender\":\"@a:example.org\","
                         "\"origin_server_ts\":1,\"content\":{\"msgtype\":\"m.text\","
                         "\"body\":\"from A\"}}]}");
        }
        if (r.path == "/_matrix/client/v3/logout")
            return reply(socket, 200, "{}");
        if (r.path == "/_matrix/client/v3/sync")
            return reply(socket, 200, R"({"next_batch":"s1"})");
        reply(socket, 404, R"({"errcode":"M_UNRECOGNIZED","error":"x"})");
    }

    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            const qsizetype end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            qsizetype length = 0;
            Request r;
            const QList<QByteArray> lines = buffer.left(end).split('\n');
            for (const QByteArray &raw : lines) {
                const QByteArray line = raw.trimmed();
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(15).trimmed().toLongLong();
                else if (line.toLower().startsWith("authorization:"))
                    r.auth = line.mid(14).trimmed();
            }
            if (buffer.size() < end + 4 + length)
                return;
            const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
            r.method = first.value(0);
            r.path = first.value(1).split('?').value(0);
            buffer.remove(0, end + 4 + length);
            seen.append(r);
            bool hold = false;
            for (const QByteArray &prefix : std::as_const(held))
                hold = hold || r.path.startsWith(prefix);
            if (hold)
                m_waiting.append({ QPointer<QTcpSocket>(socket), r });
            else
                answer(socket, r);
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<std::pair<QPointer<QTcpSocket>, Request>> m_waiting;
};

QJsonObject message(const QString &id, const QString &sender, const QString &body)
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("m.room.message") },
        { QStringLiteral("event_id"), id },
        { QStringLiteral("sender"), sender },
        { QStringLiteral("origin_server_ts"), 1 },
        { QStringLiteral("content"),
          QJsonObject{ { QStringLiteral("msgtype"), QStringLiteral("m.text") },
                       { QStringLiteral("body"), body } } },
    };
}

QJsonObject replacement(const QString &id, const QString &sender,
                        const QString &target, const QString &body)
{
    QJsonObject ev = message(id, sender, QStringLiteral("* ") + body);
    QJsonObject content = ev.value(QStringLiteral("content")).toObject();
    content.insert(QStringLiteral("m.new_content"),
                   QJsonObject{ { QStringLiteral("msgtype"), QStringLiteral("m.text") },
                                { QStringLiteral("body"), body } });
    content.insert(QStringLiteral("m.relates_to"),
                   QJsonObject{ { QStringLiteral("rel_type"), QStringLiteral("m.replace") },
                                { QStringLiteral("event_id"), target } });
    ev.insert(QStringLiteral("content"), content);
    return ev;
}

QJsonObject syncWith(const QJsonArray &events)
{
    return QJsonObject{
        { QStringLiteral("next_batch"), QStringLiteral("s1") },
        { QStringLiteral("rooms"),
          QJsonObject{ { QStringLiteral("join"),
                         QJsonObject{ { kRoom,
                                        QJsonObject{ { QStringLiteral("timeline"),
                                                       QJsonObject{ { QStringLiteral("events"),
                                                                      events } } } } } } } } },
    };
}

}

class CppHttpMatrixClientTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_home;
    FakeHomeserver *m_server = nullptr;
    QString m_url;

    static QString bodyOf(const CppHttpMatrixClient &client, const QString &eventId)
    {
        for (const auto &e : client.timeline(kRoom)) {
            if (e.eventId == eventId)
                return e.body;
        }
        return QStringLiteral("<missing>");
    }

    bool restoreAs(SettingsManager &settings, CppHttpMatrixClient &client,
                   const QString &userId, const QString &token)
    {
        if (!settings.saveSession(m_url, userId, QStringLiteral("DEV"), token))
            return false;
        QSignalSpy ok(&client, &MatrixClient::loginSucceeded);
        if (!client.restoreSession() || !ok.wait(5000))
            return false;
        client.m_rooms.insert(kRoom, RoomInfo{});
        client.m_rooms[kRoom].id = kRoom;
        return client.currentUserId() == userId;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("cpp-http-client-test"));
    }

    void init()
    {
        QSettings raw;
        raw.clear();
        raw.sync();
        for (const QString &user : { kAlice, kBob }) {
            CacheStore cache;
            if (cache.openFor(user)) {
                cache.clearAll();
                cache.close();
            }
        }
        m_server = new FakeHomeserver;
        const quint16 port = m_server->listen();
        QVERIFY(port != 0);
        m_url = QStringLiteral("http://127.0.0.1:%1").arg(port);
    }

    void cleanup()
    {
        delete m_server;
        m_server = nullptr;
    }

    void aReplacementAppliesOnlyWhenTheOriginalsSenderWroteIt()
    {
        CppHttpMatrixClient client(nullptr);
        client.m_userId = kAlice;
        client.m_loggedIn = true;
        client.openCacheFor(kAlice);
        QVERIFY(client.m_cache);

        QJsonObject stateEdit = replacement(QStringLiteral("$e4"), kAlice,
                                            QStringLiteral("$a"), QStringLiteral("state"));
        stateEdit.insert(QStringLiteral("state_key"), QString());
        QJsonObject noNewContent = replacement(QStringLiteral("$e5"), kAlice,
                                               QStringLiteral("$a"), QStringLiteral("x"));
        QJsonObject content = noNewContent.value(QStringLiteral("content")).toObject();
        content.remove(QStringLiteral("m.new_content"));
        noNewContent.insert(QStringLiteral("content"), content);
        QJsonObject encrypted = replacement(QStringLiteral("$e6"), kAlice,
                                            QStringLiteral("$a"), QStringLiteral("enc"));
        encrypted.insert(QStringLiteral("type"), QStringLiteral("m.room.encrypted"));

        client.handleSyncResponse(syncWith({
            message(QStringLiteral("$a"), kAlice, QStringLiteral("original")),
            message(QStringLiteral("$r"), kBob, QStringLiteral("bob's")),
            replacement(QStringLiteral("$e1"), kBob, QStringLiteral("$a"),
                        QStringLiteral("forged by bob")),
            stateEdit, noNewContent, encrypted,
        }));
        QCOMPARE(bodyOf(client, QStringLiteral("$a")), QStringLiteral("original"));
        const auto cached = client.m_cache->loadTimeline(kRoom);
        for (const auto &e : cached) {
            if (e.eventId == QLatin1String("$a"))
                QCOMPARE(e.body, QStringLiteral("original"));
        }

        client.handleSyncResponse(syncWith({
            replacement(QStringLiteral("$e2"), kAlice, QStringLiteral("$a"),
                        QStringLiteral("alice's edit")),
            replacement(QStringLiteral("$e3"), kAlice, QStringLiteral("$e2"),
                        QStringLiteral("edit of an edit")),
        }));
        QCOMPARE(bodyOf(client, QStringLiteral("$a")), QStringLiteral("alice's edit"));

        client.handleSyncResponse(syncWith({
            QJsonObject{ { QStringLiteral("type"), QStringLiteral("m.room.redaction") },
                         { QStringLiteral("event_id"), QStringLiteral("$x") },
                         { QStringLiteral("sender"), kBob },
                         { QStringLiteral("redacts"), QStringLiteral("$r") } },
            replacement(QStringLiteral("$e7"), kBob, QStringLiteral("$r"),
                        QStringLiteral("back from redaction")),
        }));
        QCOMPARE(bodyOf(client, QStringLiteral("$r")), QString());
    }

    void aDelayedUploadDoesNotSendAsTheNextAccount()
    {
        SettingsManager settings;
        CppHttpMatrixClient client(&settings);
        QVERIFY(restoreAs(settings, client, kAlice, QStringLiteral("tokA")));
        QFile file(m_home.path() + QStringLiteral("/note.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("hello");
        file.close();

        m_server->held.insert("/_matrix/media/v3/upload");
        client.sendFile(kRoom, file.fileName());
        QTRY_COMPARE(m_server->count("/_matrix/media/v3/upload"), 1);
        QVERIFY(client.detachSession());
        QVERIFY(restoreAs(settings, client, kBob, QStringLiteral("tokB")));

        m_server->release("/_matrix/media/v3/upload");
        QTest::qWait(300);
        QCOMPARE(m_server->count("/_matrix/client/v3/rooms/"), 0);
        QVERIFY(client.timeline(kRoom).isEmpty());

        client.sendFile(kRoom, file.fileName());
        QTRY_COMPARE(m_server->count("/_matrix/client/v3/rooms/", "Bearer tokB"), 1);
    }

    void aDelayedHistoryPageDoesNotReachTheNextAccount()
    {
        SettingsManager settings;
        CppHttpMatrixClient client(&settings);
        QVERIFY(restoreAs(settings, client, kAlice, QStringLiteral("tokA")));
        client.m_rooms[kRoom].prevBatchToken = QStringLiteral("t1");
        m_server->held.insert("/_matrix/client/v3/rooms/");
        client.loadOlderMessages(kRoom);
        QTRY_COMPARE(m_server->count("/_matrix/client/v3/rooms/"), 1);
        QVERIFY(client.detachSession());
        QVERIFY(restoreAs(settings, client, kBob, QStringLiteral("tokB")));
        QVERIFY(!client.paginating(kRoom));

        m_server->release("/_matrix/client/v3/rooms/");
        QTest::qWait(300);
        QVERIFY(client.timeline(kRoom).isEmpty());

        client.m_rooms[kRoom].prevBatchToken = QStringLiteral("t1");
        client.loadOlderMessages(kRoom);
        QTRY_COMPARE(bodyOf(client, QStringLiteral("$old")), QStringLiteral("from A"));
    }

    void aDelayedWhoamiDoesNotReplaceTheNextAccount()
    {
        SettingsManager settings;
        CppHttpMatrixClient client(&settings);
        QVERIFY(settings.saveSession(m_url, kAlice, QStringLiteral("DEV"),
                                     QStringLiteral("tokA")));
        m_server->held.insert("/_matrix/client/v3/account/whoami");
        QVERIFY(client.restoreSession());
        QTRY_COMPARE(m_server->count("/_matrix/client/v3/account/whoami"), 1);
        QVERIFY(client.detachSession());
        m_server->held.clear();
        QVERIFY(restoreAs(settings, client, kBob, QStringLiteral("tokB")));

        QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
        m_server->release("/_matrix/client/v3/account/whoami");
        QTest::qWait(300);
        QCOMPARE(client.currentUserId(), kBob);
        QCOMPARE(succeeded.count(), 0);
    }

    void aSwitchWaitsForASignOutInFlight()
    {
        SettingsManager settings;
        CppHttpMatrixClient client(&settings);
        QVERIFY(restoreAs(settings, client, kAlice, QStringLiteral("tokA")));
        m_server->held.insert("/_matrix/client/v3/logout");
        QSignalSpy out(&client, &MatrixClient::loggedOut);
        client.logout();
        QTRY_COMPARE(m_server->count("/_matrix/client/v3/logout"), 1);
        QVERIFY(!client.detachSession());
        QVERIFY(!client.restoreSession());
        m_server->release("/_matrix/client/v3/logout");
        QTRY_COMPARE(out.count(), 1);
        QVERIFY(!settings.hasSession());

        QVERIFY(restoreAs(settings, client, kBob, QStringLiteral("tokB")));
        QVERIFY(settings.hasSession());
    }

    void aTransientRestoreFailureKeepsTheSession_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("unreachable");
        QTest::newRow("unreachable") << 0 << QByteArray() << true;
        QTest::newRow("server-error") << 500 << QByteArray(R"({"errcode":"M_UNKNOWN"})") << false;
        QTest::newRow("malformed") << 200 << QByteArray("<html>gateway</html>") << false;
        QTest::newRow("proxy-401") << 401 << QByteArray("denied") << false;
    }

    void aTransientRestoreFailureKeepsTheSession()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, unreachable);
        SettingsManager settings;
        QString url = m_url;
        if (unreachable) {
            QTcpServer closed;
            QVERIFY(closed.listen(QHostAddress::LocalHost, 0));
            url = QStringLiteral("http://127.0.0.1:%1").arg(closed.serverPort());
        }
        QVERIFY(settings.saveSession(url, kAlice, QStringLiteral("DEV"),
                                     QStringLiteral("tokA")));
        {
            CacheStore cache;
            QVERIFY(cache.openFor(kAlice));
            RoomInfo room;
            room.id = kRoom;
            room.name = QStringLiteral("Cached");
            cache.saveRoom(room);
            TimelineEvent e;
            e.eventId = QStringLiteral("$cached");
            e.roomId = kRoom;
            e.sender = kAlice;
            e.body = QStringLiteral("kept");
            e.type = TimelineEvent::TextMessage;
            cache.appendEvent(e);
            cache.close();
        }
        m_server->whoami = [status, body](QTcpSocket *socket, const Request &) {
            FakeHomeserver::reply(socket, status, body);
        };

        CppHttpMatrixClient client(&settings);
        QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QVERIFY(client.restoreSession());
        QVERIFY(succeeded.wait(10000));
        QCOMPARE(failed.count(), 0);
        QVERIFY(client.isLoggedIn());
        QCOMPARE(client.currentUserId(), kAlice);
        QVERIFY(settings.hasSession());
        QCOMPARE(bodyOf(client, QStringLiteral("$cached")), QStringLiteral("kept"));

        if (!unreachable) {
            m_server->whoami = {};
            client.startSync();
            QTRY_VERIFY(m_server->count("/_matrix/client/v3/sync", "Bearer tokA") > 0);
            client.stopSync();
        }
    }

    void aRevokedTokenEndsTheSession_data()
    {
        QTest::addColumn<bool>("soft");
        QTest::newRow("hard") << false;
        QTest::newRow("soft") << true;
    }

    void aRevokedTokenEndsTheSession()
    {
        QFETCH(bool, soft);
        SettingsManager settings;
        QVERIFY(settings.saveSession(m_url, kAlice, QStringLiteral("DEV"),
                                     QStringLiteral("dead")));
        m_server->whoami = [soft](QTcpSocket *socket, const Request &) {
            FakeHomeserver::reply(socket, 401,
                soft ? R"({"errcode":"M_UNKNOWN_TOKEN","error":"x","soft_logout":true})"
                     : R"({"errcode":"M_UNKNOWN_TOKEN","error":"x"})");
        };
        CppHttpMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QVERIFY(client.restoreSession());
        QVERIFY(failed.wait(5000));
        QVERIFY(!client.isLoggedIn());
        QCOMPARE(settings.hasSession(), soft);
    }
};

QTEST_MAIN(CppHttpMatrixClientTest)
#include "CppHttpMatrixClientTest.moc"
