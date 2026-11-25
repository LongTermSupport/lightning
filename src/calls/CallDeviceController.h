#pragma once

#include "calls/CaptureDeviceSelection.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QMediaDevices;
class SettingsManager;

class CallDeviceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallDeviceController is exposed via app.callDevices")

    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList cameras READ cameras NOTIFY devicesChanged)
    /// True inside a Flatpak, where Qt lists no camera (no /dev/video*) yet
    /// the camera works through the xdg Camera portal, which picks the
    /// device. Lets Settings say so instead of "No camera was found". Not
    /// Snap, whose camera plug exposes the real device.
    Q_PROPERTY(bool camerasChosenByDesktop READ camerasChosenByDesktop
                   CONSTANT)
    Q_PROPERTY(QString activeMicrophoneId READ activeMicrophoneId
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString activeSpeakerId READ activeSpeakerId
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString activeCameraId READ activeCameraId
                   NOTIFY selectionChanged)
    Q_PROPERTY(bool microphonePinned READ microphonePinned WRITE setMicrophonePinned NOTIFY selectionChanged)
    Q_PROPERTY(bool preferredMicrophoneMissing READ preferredMicrophoneMissing
                   NOTIFY selectionChanged)
    /// A capture pipeline could not open the chosen microphone and used the
    /// system default instead. Says nothing about whether it is connected.
    Q_PROPERTY(bool preferredMicrophoneUnusable
                   READ preferredMicrophoneUnusable NOTIFY selectionChanged)
    Q_PROPERTY(bool preferredCameraMissing READ preferredCameraMissing
                   NOTIFY selectionChanged)
    Q_PROPERTY(bool hasMicrophone READ hasMicrophone NOTIFY devicesChanged)
    Q_PROPERTY(bool hasCamera READ hasCamera NOTIFY devicesChanged)

public:
    explicit CallDeviceController(QObject *parent = nullptr);

    void setSettings(SettingsManager *settings);

    QVariantList microphones() const;
    QVariantList speakers() const;
    QVariantList cameras() const;
    bool camerasChosenByDesktop() const;

    QString activeMicrophoneId() const;
    QString activeSpeakerId() const;
    QString activeCameraId() const;
    bool microphonePinned() const;
    void setMicrophonePinned(bool pinned);
    bool preferredMicrophoneMissing() const;
    bool preferredCameraMissing() const;
    bool preferredMicrophoneUnusable() const;
    /// What a capture pipeline did with microphone `chosenId`; any thread.
    void noteMicrophoneBinding(const QString &chosenId, bool fellBack);
    bool hasMicrophone() const;
    bool hasCamera() const;
    bool hasSpeaker() const;

    Q_INVOKABLE void selectMicrophone(const QString &id);
    Q_INVOKABLE void selectSpeaker(const QString &id);
    Q_INVOKABLE void selectCamera(const QString &id);

    using Selection = lightning::calls::DeviceChoice;
    Selection cameraSelection() const;
    Selection microphoneSelection() const;
    Selection speakerSelection() const;

Q_SIGNALS:
    void devicesChanged();
    void selectionChanged();
    void activeDevicesChanged();
    void audioOutputsChanged();

private Q_SLOTS:
    void onDeviceListChanged();

private:
    void ensureBackend() const;
    QString resolveActive(const QString &preferred, int kind) const;

    mutable QMediaDevices *m_devices = nullptr;
    QPointer<SettingsManager> m_settings;
    QString m_pinnedMicrophoneId;
    QString m_lastActiveMic;
    QString m_lastActiveSpeaker;
    QString m_lastActiveCamera;
    bool m_lastCameraMissing = false;
    QString m_microphoneFellBackFrom;
};
