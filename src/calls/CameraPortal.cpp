#include "calls/CameraPortal.h"
#include "calls/PortalRequest.h"

#include <QLoggingCategory>
#include <QRandomGenerator>

#ifdef HAVE_QT_DBUS
#include <unistd.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QVariantMap>
#endif

namespace {
Q_LOGGING_CATEGORY(lcCameraPortal, "lightning.calls.portal")

constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kCamera = "org.freedesktop.portal.Camera";
constexpr auto kRequest = "org.freedesktop.portal.Request";

QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}

void closeRequest(const QString &requestPath)
{
    if (requestPath.isEmpty())
        return;
    QDBusConnection::sessionBus().send(QDBusMessage::createMethodCall(
        kService, requestPath, kRequest, QStringLiteral("Close")));
}
#endif
}

#ifdef HAVE_QT_DBUS
/// One step of the handshake: subscribe to the Request's `Response`, deliver
/// it once, unsubscribe. A deliberate near-copy of ScreenCastPortal's
/// PortalStep (file-local moc types); named differently to avoid collisions.
class CameraPortalStep : public QObject
{
    Q_OBJECT
public:
    CameraPortalStep(CameraPortal *owner, const QString &requestPath)
        : QObject(owner), m_path(requestPath)
    {
        QDBusConnection::sessionBus().connect(
            QString::fromLatin1(kService), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }
    ~CameraPortalStep() override
    {
        QDBusConnection::sessionBus().disconnect(
            QString::fromLatin1(kService), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }

Q_SIGNALS:
    void answered(uint response, const QVariantMap &results);

private Q_SLOTS:
    void onResponse(uint response, const QVariantMap &results)
    {
        Q_EMIT answered(response, results);
        deleteLater();
    }

private:
    QString m_path;
};
#endif

CameraPortal::CameraPortal(QObject *parent)
    : QObject(parent),
      m_capability(this, "org.freedesktop.portal.Camera",
                   [this] { Q_EMIT capabilityChanged(); })
{
    m_requestTimeout.setSingleShot(true);
    m_requestTimeout.setInterval(kRequestTimeoutMs);
    connect(&m_requestTimeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcCameraPortal) << "camera portal request timed out; "
                                     "releasing";
        retire();
        Q_EMIT failed(QStringLiteral("timeout"));
    });
}

CameraPortal::~CameraPortal()
{
    retire();
}

portal::Capability CameraPortal::capability()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return portal::Capability::Unavailable;
#else
    return m_capability.state();
#endif
}

portal::Capability CameraPortal::cameraPresence()
{
    const portal::Capability state = capability();
    if (state != portal::Capability::Available)
        return state;
    return m_capability.properties()
                   .value(QStringLiteral("IsCameraPresent"))
                   .toBool()
        ? portal::Capability::Available
        : portal::Capability::Unavailable;
}

void CameraPortal::refreshCapability()
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    m_capability.refresh();
#endif
}

QString CameraPortal::failureCategory(const QString &errorName,
                                     bool openingRemote)
{
    const QString category = portal::permissionError(errorName);
    if (!category.isEmpty())
        return category;
    return openingRemote ? QStringLiteral("no_pipewire_remote")
                         : QStringLiteral("portal_failed");
}

void CameraPortal::cancel()
{
    const bool pending = m_busy;
    retire();
    if (pending) {
        m_cancelled = true;
        Q_EMIT cancelled();
    }
}

void CameraPortal::finishError(const QString &category)
{
    if (category == QLatin1String("cancelled")) {
        cancel();
        return;
    }
    retire();
    Q_EMIT failed(category);
}

void CameraPortal::retire()
{
    ++m_generation;
    m_busy = false;
    m_requestTimeout.stop();
#ifdef HAVE_QT_DBUS
    closeRequest(m_requestPath);
#endif
    m_requestPath.clear();
}

#if !defined(HAVE_QT_DBUS) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void CameraPortal::requestAccess()
{
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void CameraPortal::requestAccess()
{
    if (m_busy) {
        Q_EMIT failed(QStringLiteral("busy"));
        return;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        Q_EMIT failed(QStringLiteral("no_portal"));
        return;
    }

    m_cancelled = false;
    m_busy = true;
    m_requestTimeout.start();
    const quint64 generation = ++m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    const QString token = freshToken();
    const auto onAnswer = [this, stale](uint response, const QVariantMap &) {
        if (stale())
            return;
        m_requestPath.clear();
        if (response != 0) {
            finishError(response == 1 ? QStringLiteral("cancelled")
                                      : QStringLiteral("portal_failed"));
            return;
        }
        openRemote();
    };
    auto subscription = portal::subscribeBeforeCall<CameraPortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);
    m_requestPath = subscription.path;

    QDBusMessage access = QDBusMessage::createMethodCall(
        kService, kPath, kCamera, QStringLiteral("AccessCamera"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    access << options;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(access), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale, subscription, onAnswer](
                QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale()) {
                    // cancel() closed the predicted path; an older portal's
                    // own path is still open.
                    if (!reply.isError()
                        && reply.value().path() != subscription.path)
                        closeRequest(reply.value().path());
                    portal::dropSubscription(subscription);
                    return;
                }
                if (m_requestPath != subscription.path)
                    return;
                if (reply.isError()) {
                    // Error text can name the desktop and paths; the error
                    // NAME decides between a refusal and no portal at all.
                    const QString category = failureCategory(
                        reply.error().name(),false);
                    qCWarning(lcCameraPortal)
                        << "camera AccessCamera failed error="
                        << reply.error().name() << "category=" << category;
                    portal::dropSubscription(subscription);
                    m_requestPath.clear();
                    finishError(category);
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
                m_requestPath = subscription.path;
            });
}

void CameraPortal::openRemote()
{
    const quint64 generation = m_generation;
    QDBusMessage open = QDBusMessage::createMethodCall(
        kService, kPath, kCamera, QStringLiteral("OpenPipeWireRemote"));
    open << QVariantMap();

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(open), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
                if (generation != m_generation)
                    return;
                if (reply.isError() || !reply.value().isValid()) {
                    qCWarning(lcCameraPortal)
                        << "camera OpenPipeWireRemote failed error="
                        << (reply.isError() ? reply.error().name()
                                            : QStringLiteral("no_fd"));
                    finishError(
                        reply.isError()
                            ? failureCategory(reply.error().name(), true)
                            : QStringLiteral("no_pipewire_remote"));
                    return;
                }
                // QDBusUnixFileDescriptor closes its fd with the last copy, so
                // hand over a dup; the receiver owns it.
                const int fd = ::dup(reply.value().fileDescriptor());
                if (fd < 0) {
                    retire();
                    Q_EMIT failed(QStringLiteral("no_pipewire_remote"));
                    return;
                }
                m_busy = false;
                m_requestTimeout.stop();
                qCInfo(lcCameraPortal) << "camera portal granted; pipewire "
                                          "remote open";
                Q_EMIT ready(fd);
            });
}
#endif

#ifdef HAVE_QT_DBUS
#include "CameraPortal.moc"
#endif
