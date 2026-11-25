#include "calls/ScreenCastPortal.h"
#include "calls/PortalRequest.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QScreen>
#include <QWindow>

#ifdef HAVE_QT_DBUS
#include <unistd.h>
#endif

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#endif

namespace {
Q_LOGGING_CATEGORY(lcPortal, "lightning.calls.portal")

constexpr int kRequestTimeoutMs = 120000;

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr auto kRequest = "org.freedesktop.portal.Request";

QString errorCategory(const QDBusError &error, const QString &fallback)
{
    const QString category = portal::permissionError(error.name());
    if (!category.isEmpty())
        return category;
    const QString message = error.message();
    if (message.contains(QLatin1String("X11"), Qt::CaseSensitive)
        && (message.contains(QLatin1String("not available"),
                             Qt::CaseInsensitive)
            || message.contains(QLatin1String("not supported"),
                                Qt::CaseInsensitive)
            || message.contains(QLatin1String("unsupported"),
                                Qt::CaseInsensitive)))
        return QStringLiteral("x11_unsupported");
    return fallback;
}

/// A caller-unique token per request, from which the portal derives the
/// Request path. Random, so the path is not guessable by other apps on the
/// bus.
QString freshToken()
{
    return QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
}

void closePortalObject(const QString &path, const char *interface)
{
    if (path.isEmpty())
        return;
    QDBusConnection::sessionBus().send(QDBusMessage::createMethodCall(
        kService, path, QString::fromLatin1(interface),
        QStringLiteral("Close")));
}

constexpr uint kCursorHidden = 1;
constexpr uint kCursorEmbedded = 2;
constexpr uint kCursorMetadata = 4;
#endif
}

#ifdef HAVE_QT_DBUS
class PortalStep : public QObject
{
    Q_OBJECT
public:
    PortalStep(ScreenCastPortal *owner, const QString &requestPath)
        : QObject(owner)
    {
        QDBusConnection::sessionBus().connect(
            QString::fromLatin1(kService), requestPath, kRequest, QStringLiteral("Response"),
            this, SLOT(onResponse(uint, QVariantMap)));
        m_path = requestPath;
    }
    ~PortalStep() override
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

ScreenCastPortal::ScreenCastPortal(QObject *parent)
    : QObject(parent),
      m_capability(this, "org.freedesktop.portal.ScreenCast",
                   [this] { Q_EMIT capabilityChanged(); })
{
    m_requestTimeout.setSingleShot(true);
    m_requestTimeout.setInterval(kRequestTimeoutMs);
    connect(&m_requestTimeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcPortal) << "screen share request timed out; releasing";
        retire();
        Q_EMIT failed(QStringLiteral("timeout"));
    });
    m_answerWatchdog.setSingleShot(true);
    connect(&m_answerWatchdog, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        qCWarning(lcPortal)
            << "screen share portal gave no answer within the caller's bound; "
               "abandoning it";
        retire();
        Q_EMIT failed(QStringLiteral("no_answer"));
    });
}

void ScreenCastPortal::armAnswerWatchdog(int ms)
{
    if (ms > 0)
        m_answerWatchdog.start(ms);
    else
        m_answerWatchdog.stop();
}

ScreenCastPortal::~ScreenCastPortal()
{
    retire();
}

portal::Capability ScreenCastPortal::capability()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return QGuiApplication::screens().size() > 0
        ? portal::Capability::Available
        : portal::Capability::Unavailable;
#else
    return m_capability.state();
#endif
}

void ScreenCastPortal::refreshCapability()
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    m_capability.refresh();
#endif
}

void ScreenCastPortal::cancel()
{
    const bool pending = m_busy;
    retire();
    if (pending) {
        m_cancelled = true;
        Q_EMIT cancelled();
    }
}

void ScreenCastPortal::finishError(const QString &category)
{
    if (category == QLatin1String("cancelled")) {
        cancel();
        return;
    }
    retire();
    Q_EMIT failed(category);
}

void ScreenCastPortal::retire()
{
    ++m_generation;
    m_busy = false;
    m_requestTimeout.stop();
    m_answerWatchdog.stop();
#ifdef HAVE_QT_DBUS
    if (!m_sessionHandle.isEmpty())
        QDBusConnection::sessionBus().disconnect(kService, m_sessionHandle,
            QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"),
            this, SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
    closePortalObject(m_requestPath, kRequest);
    closePortalObject(m_sessionHandle, "org.freedesktop.portal.Session");
#endif
    m_requestPath.clear();
    m_sessionHandle.clear();
}

#ifdef HAVE_QT_DBUS
void ScreenCastPortal::onSessionClosed(const QVariantMap &, const QDBusMessage &message)
{
    if (message.path() != m_sessionHandle)
        return;
    const bool requesting = m_busy;
    QDBusConnection::sessionBus().disconnect(kService, m_sessionHandle,
        QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"),
        this, SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
    m_sessionHandle.clear();
    retire();
    if (requesting)
        Q_EMIT failed(QStringLiteral("session_closed"));
    else
        Q_EMIT sessionClosed();
}
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void ScreenCastPortal::requestShare(int types, int preparationBoundMs,
                                    int pickerBoundMs)
{
    Q_UNUSED(types);
    Q_UNUSED(preparationBoundMs);
    Q_UNUSED(pickerBoundMs);
    if (m_busy) {
        Q_EMIT failed(QStringLiteral("busy"));
        return;
    }
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        Q_EMIT failed(QStringLiteral("no_screen"));
        return;
    }
    const QWindow *window = QGuiApplication::focusWindow();
    const QScreen *screen = window ? window->screen() : nullptr;
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const int index = screen ? int(screens.indexOf(screen)) : 0;
    Q_EMIT ready(static_cast<unsigned>(index < 0 ? 0 : index), -1, std::nullopt);
}
#elif !defined(HAVE_QT_DBUS)
void ScreenCastPortal::requestShare(int types, int preparationBoundMs,
                                    int pickerBoundMs)
{
    Q_UNUSED(types);
    Q_UNUSED(preparationBoundMs);
    Q_UNUSED(pickerBoundMs);
    Q_EMIT failed(QStringLiteral("no_portal"));
}
#else
void ScreenCastPortal::requestShare(int types, int preparationBoundMs,
                                    int pickerBoundMs)
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
    m_pickerBoundMs = pickerBoundMs;
    armAnswerWatchdog(preparationBoundMs);
    const quint64 generation = ++m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    QDBusMessage create = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("CreateSession"));
    const QString token = freshToken();
    QVariantMap createOptions;
    createOptions.insert(QStringLiteral("handle_token"), token);
    createOptions.insert(QStringLiteral("session_handle_token"),
                         freshToken());
    create << createOptions;

    const auto onAnswer = [this, types, stale](uint response,
                                               const QVariantMap &results) {
        const QString session =
            results.value(QStringLiteral("session_handle")).toString();
        if (stale()) {
            // A session created after cancel() is ours to close.
            if (response == 0)
                closePortalObject(session, "org.freedesktop.portal.Session");
            return;
        }
        m_requestPath.clear();
        if (response == 1) {
            cancel();
            return;
        }
        if (response != 0) {
            qCWarning(lcPortal) << "screen share session refused response="
                                << response;
            retire();
            Q_EMIT failed(QStringLiteral("session_refused"));
            return;
        }
        m_sessionHandle = session;
        if (m_sessionHandle.isEmpty()) {
            retire();
            Q_EMIT failed(QStringLiteral("no_session"));
            return;
        }
        QDBusConnection::sessionBus().connect(kService, session,
            QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"),
            this, SLOT(onSessionClosed(QVariantMap,QDBusMessage)));
        negotiateSources(types);
    };
    auto subscription = portal::subscribeBeforeCall<PortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);
    m_requestPath = subscription.path;

    auto *createWatcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(create), this);
    connect(createWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale, subscription, onAnswer](
                QDBusPendingCallWatcher *watcher) mutable {
                watcher->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                if (stale()) {
                    if (reply.isError()) {
                        portal::dropSubscription(subscription);
                        return;
                    }
                    // Keep listening: the session may still be created.
                    if (reply.value().path() != subscription.path)
                        closePortalObject(reply.value().path(), kRequest);
                    portal::reconcileRequestPath(this, subscription,
                                                 reply.value().path(),
                                                 onAnswer);
                    if (subscription.step)
                        QTimer::singleShot(kRequestTimeoutMs,
                                           subscription.step.data(),
                                           &QObject::deleteLater);
                    return;
                }
                if (m_requestPath != subscription.path)
                    return;
                if (reply.isError()) {
                    portal::dropSubscription(subscription);
                    finishError(errorCategory(reply.error(),
                                                QStringLiteral("session_failed")));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
                m_requestPath = subscription.path;
            });
}

void ScreenCastPortal::negotiateSources(int types)
{
    const quint64 generation = m_generation;
    QDBusMessage get = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("GetAll"));
    get << QString::fromLatin1(kScreenCast);
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(get), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, types](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation)
                    return;
                QDBusPendingReply<QVariantMap> reply = *w;
                const QVariantMap properties =
                    reply.isError() ? QVariantMap() : reply.value();
                // Version 1 has neither property: send no cursor_mode, which
                // the portal would close the session over if unadvertised.
                uint cursorMode = 0;
                const QVariant cursorModes =
                    properties.value(QStringLiteral("AvailableCursorModes"));
                if (cursorModes.isValid()) {
                    const uint modes = cursorModes.toUInt();
                    cursorMode = (modes & kCursorEmbedded) ? kCursorEmbedded
                        : (modes & kCursorHidden)          ? kCursorHidden
                        : (modes & kCursorMetadata)        ? kCursorMetadata
                                                           : 0;
                }
                if (cursorModes.isValid() && cursorMode != kCursorEmbedded)
                    qCInfo(lcPortal) << "screen share portal cannot draw the "
                                        "pointer into the picture; it will "
                                        "not be visible cursor_mode="
                                     << cursorMode;
                int offered = types;
                const QVariant sourceTypes =
                    properties.value(QStringLiteral("AvailableSourceTypes"));
                if (sourceTypes.isValid())
                    offered &= int(sourceTypes.toUInt());
                if (offered == 0) {
                    qCWarning(lcPortal)
                        << "screen share portal offers none of the requested "
                           "source types requested=" << types << "available="
                        << sourceTypes.toUInt();
                    retire();
                    Q_EMIT failed(QStringLiteral("no_source_type"));
                    return;
                }
                selectSources(offered, cursorMode);
            });
}

void ScreenCastPortal::selectSources(int types, uint cursorMode)
{
    const quint64 generation = m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    QDBusMessage select = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("SelectSources"));
    const QString token = freshToken();
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("types"), static_cast<uint>(types));
    options.insert(QStringLiteral("multiple"), false);
    if (cursorMode != 0)
        options.insert(QStringLiteral("cursor_mode"), cursorMode);
    select << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << options;

    const auto onAnswer = [this, stale](uint response, const QVariantMap &) {
        if (stale())
            return;
        m_requestPath.clear();
        if (response == 1) {
            cancel();
            return;
        }
        if (response != 0) {
            qCWarning(lcPortal) << "screen share source selection refused "
                                   "response=" << response;
            retire();
            Q_EMIT failed(QStringLiteral("session_refused"));
            return;
        }
        startSession();
    };
    auto subscription = portal::subscribeBeforeCall<PortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);
    m_requestPath = subscription.path;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(select), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale, subscription, onAnswer](
                QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale()) {
                    if (!reply.isError()
                        && reply.value().path() != subscription.path)
                        closePortalObject(reply.value().path(), kRequest);
                    portal::dropSubscription(subscription);
                    return;
                }
                if (m_requestPath != subscription.path)
                    return;
                if (reply.isError()) {
                    portal::dropSubscription(subscription);
                    m_requestPath.clear();
                    finishError(errorCategory(
                        reply.error(), QStringLiteral("select_failed")));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
                m_requestPath = subscription.path;
            });
}

void ScreenCastPortal::startSession()
{
    const quint64 generation = m_generation;
    const auto stale = [this, generation] {
        return generation != m_generation;
    };

    armAnswerWatchdog(m_pickerBoundMs);
    QDBusMessage start = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("Start"));
    const QString token = freshToken();
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    start << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
          << QString() << options;

    const auto onAnswer = [this, stale](uint response,
                                        const QVariantMap &results) {
        if (stale())
            return;
        m_requestPath.clear();
        if (response != 0) {
            finishError(response == 1 ? QStringLiteral("cancelled")
                                      : QStringLiteral("start_failed"));
            return;
        }
        handleStreams(results);
    };
    auto subscription = portal::subscribeBeforeCall<PortalStep>(
        this, QDBusConnection::sessionBus().baseService(), token, onAnswer);
    m_requestPath = subscription.path;

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(start), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, stale, subscription, onAnswer](
                QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (stale()) {
                    if (!reply.isError()
                        && reply.value().path() != subscription.path)
                        closePortalObject(reply.value().path(), kRequest);
                    portal::dropSubscription(subscription);
                    return;
                }
                if (m_requestPath != subscription.path)
                    return;
                if (reply.isError()) {
                    portal::dropSubscription(subscription);
                    m_requestPath.clear();
                    finishError(errorCategory(
                        reply.error(), QStringLiteral("start_failed")));
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
                m_requestPath = subscription.path;
            });
}

void ScreenCastPortal::handleStreams(const QVariantMap &results)
{
    const QVariant streams = results.value(QStringLiteral("streams"));
    const QDBusArgument argument = streams.value<QDBusArgument>();
    if (argument.currentType() != QDBusArgument::ArrayType) {
        retire();
        Q_EMIT failed(QStringLiteral("no_stream"));
        return;
    }
    uint nodeId = 0;
    std::optional<quint64> serial;
    bool invalidSerial = false;
    bool found = false;
    argument.beginArray();
    while (!argument.atEnd()) {
        argument.beginStructure();
        uint id = 0;
        argument >> id;
        QVariantMap properties;
        argument >> properties;
        argument.endStructure();
        if (!found) {
            nodeId = id;
            const auto it = properties.constFind(QStringLiteral("pipewire-serial"));
            if (it != properties.cend()) {
                if (it->metaType().id() == QMetaType::ULongLong)
                    serial = it->toULongLong();
                else
                    invalidSerial = true;
            }
            found = true;
        }
    }
    argument.endArray();

    if (!found || invalidSerial) {
        retire();
        Q_EMIT failed(QStringLiteral("no_stream"));
        return;
    }
    qCInfo(lcPortal) << "screen share source selected";
    openRemote(nodeId, serial);
}

void ScreenCastPortal::openRemote(unsigned nodeId, std::optional<quint64> serial)
{
    // OpenPipeWireRemote grants actual access: the node id alone names a node
    // in a remote we were never given, and the pipeline then runs without
    // producing frames. A plain method call; the reply carries the fd.
    const quint64 generation = m_generation;
    QDBusMessage open = QDBusMessage::createMethodCall(
        kService, kPath, kScreenCast, QStringLiteral("OpenPipeWireRemote"));
    open << QVariant::fromValue(QDBusObjectPath(m_sessionHandle))
         << QVariantMap();

    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(open), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation, nodeId, serial](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
                if (generation != m_generation)
                    return;
                if (reply.isError() || !reply.value().isValid()) {
                    qCWarning(lcPortal)
                        << "OpenPipeWireRemote failed error="
                        << (reply.isError() ? reply.error().name()
                                            : QStringLiteral("no_fd"));
                    finishError(reply.isError() ? errorCategory(reply.error(), QStringLiteral("no_pipewire_remote"))
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
                m_answerWatchdog.stop();
                Q_EMIT ready(nodeId, fd, serial);
            });
}
#endif

#ifdef HAVE_QT_DBUS
#include "ScreenCastPortal.moc"
#endif
