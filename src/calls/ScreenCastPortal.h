#pragma once

#include "calls/PortalCapability.h"

#include <optional>
#ifdef HAVE_QT_DBUS
#include <QDBusMessage>
#endif
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QString>

class ScreenCastPortal : public QObject
{
    Q_OBJECT

public:
    enum SourceType {
        Monitor = 1,
        Window = 2,
        Virtual = 4,
    };

    explicit ScreenCastPortal(QObject *parent = nullptr);
    ~ScreenCastPortal() override;

    /// Never blocks: Unknown until the portal has answered. On Windows and
    /// macOS, whether there is a screen.
    portal::Capability capability();
    void refreshCapability();

    void requestShare(int types = Monitor | Window, int preparationBoundMs = 0,
                      int pickerBoundMs = 0);
    void cancel();
    bool wasCancelled() const { return m_cancelled; }
    bool busy() const { return m_busy; }

Q_SIGNALS:
    /// The user chose a source and its PipeWire remote is open. Both values
    /// are needed: the node lives in the remote the portal opened for us, and
    /// `pipewiresrc path=<node>` alone produces no frames. `pipewireFd` is a
    /// duplicated descriptor owned by the receiver.
    void ready(unsigned pipewireNodeId, int pipewireFd, std::optional<quint64> serial);
    void sessionClosed();
    void cancelled();
    void failed(const QString &category);
    void capabilityChanged();

#ifdef HAVE_QT_DBUS
private Q_SLOTS:
    void onSessionClosed(const QVariantMap &, const QDBusMessage &message);
#endif

private:
    void retire();
    void finishError(const QString &category);
    void armAnswerWatchdog(int ms);
#ifdef HAVE_QT_DBUS
    void negotiateSources(int types);
    void selectSources(int types, uint cursorMode);
    void startSession();
    void handleStreams(const QVariantMap &results);
    void openRemote(unsigned nodeId, std::optional<quint64> serial);
#endif

    bool m_busy = false;
    bool m_cancelled = false;
    QTimer m_requestTimeout;
    QTimer m_answerWatchdog;
    int m_pickerBoundMs = 0;
    QString m_sessionHandle;
    // The portal Request still awaiting its Response; closed on cancel.
    QString m_requestPath;
    quint64 m_generation = 0;
    portal::CapabilityProbe m_capability;
};
