#pragma once

// Whether an xdg-desktop-portal interface exists, read without blocking.
// One Properties.GetAll in flight at a time; an Available answer is kept for
// kTtlMs, any other answer is asked again on the next read, and a portal
// restart drops what was known.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#endif

namespace portal {

enum class Capability {
    Unknown,
    Available,
    Unavailable,
};

class CapabilityProbe
{
public:
    static constexpr int kTimeoutMs = 2000;
    static constexpr qint64 kTtlMs = 60000;

    CapabilityProbe(QObject *owner, const char *interfaceName,
                    std::function<void()> changed)
        : m_owner(owner), m_interface(QString::fromLatin1(interfaceName)),
          m_changed(std::move(changed))
    {
    }

    /// The last answer; starts a probe when there is none or it is stale.
    Capability state()
    {
        const bool fresh = m_state == Capability::Available
            && QDateTime::currentMSecsSinceEpoch() - m_settledAtMs < kTtlMs;
        if (!fresh)
            refresh();
        return m_state;
    }

    const QVariantMap &properties() const { return m_properties; }
    bool probing() const { return m_inFlight; }

    void refresh()
    {
#ifdef HAVE_QT_DBUS
        if (m_inFlight)
            return;
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            settle(Capability::Unavailable, {});
            return;
        }
        watchOwner(bus);
        m_inFlight = true;
        QDBusMessage get = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("GetAll"));
        get << m_interface;
        auto *watcher = new QDBusPendingCallWatcher(
            bus.asyncCall(get, kTimeoutMs), m_owner);
        const quint64 generation = m_generation;
        QObject::connect(
            watcher, &QDBusPendingCallWatcher::finished, m_owner,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (generation != m_generation)
                    return;
                m_inFlight = false;
                const QDBusPendingReply<QVariantMap> reply = *w;
                if (reply.isError())
                    settle(Capability::Unavailable, {});
                else
                    settle(Capability::Available, reply.value());
            });
#else
        settle(Capability::Unavailable, {});
#endif
    }

private:
    void settle(Capability state, const QVariantMap &properties)
    {
        const bool moved = state != m_state || properties != m_properties;
        m_state = state;
        m_properties = properties;
        m_settledAtMs = QDateTime::currentMSecsSinceEpoch();
        if (moved && m_changed)
            m_changed();
    }

#ifdef HAVE_QT_DBUS
    void watchOwner(const QDBusConnection &bus)
    {
        if (m_ownerWatch)
            return;
        auto *watch = new QDBusServiceWatcher(
            QStringLiteral("org.freedesktop.portal.Desktop"), bus,
            QDBusServiceWatcher::WatchForOwnerChange, m_owner);
        m_ownerWatch = watch;
        QObject::connect(watch,
                         &QDBusServiceWatcher::serviceOwnerChanged, m_owner,
                         [this] {
                             ++m_generation;
                             m_inFlight = false;
                             m_settledAtMs = 0;
                             const bool moved =
                                 m_state != Capability::Unknown;
                             m_state = Capability::Unknown;
                             m_properties.clear();
                             if (moved && m_changed)
                                 m_changed();
                         });
    }
#endif

    // QObject, not the watcher type: the layout must not depend on HAVE_QT_DBUS.
    QObject *m_ownerWatch = nullptr;
    QObject *m_owner = nullptr;
    QString m_interface;
    std::function<void()> m_changed;
    Capability m_state = Capability::Unknown;
    QVariantMap m_properties;
    qint64 m_settledAtMs = 0;
    quint64 m_generation = 0;
    bool m_inFlight = false;
};

}
