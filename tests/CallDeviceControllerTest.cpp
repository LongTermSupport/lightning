#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "app/SettingsManager.h"
#include "calls/CallDeviceController.h"

class CallDeviceControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("call-device-controller-test"));
    }

    void aChosenCameraThatIsNotConnectedIsReportedMissing()
    {
        SettingsManager settings;
        CallDeviceController devices;
        devices.setSettings(&settings);

        settings.setPreferredCameraId(QString());
        auto selection = devices.cameraSelection();
        QVERIFY(selection.id.isEmpty());
        QVERIFY(!selection.preferredMissing);

        settings.setPreferredCameraId(
            QStringLiteral("/dev/video-lightning-test-missing"));
        selection = devices.cameraSelection();
        QVERIFY(selection.id.isEmpty());
        QVERIFY2(selection.preferredMissing,
                 "an unplugged chosen camera left as 'system default'; the "
                 "engine would open the default camera");
        QVERIFY(devices.activeCameraId().isEmpty());

        devices.selectCamera(QString());
        QVERIFY(!devices.cameraSelection().preferredMissing);
        QVERIFY(settings.preferredCameraDescription().isEmpty());
    }

    void aRePickAndTheEndOfAMissingCameraAreAnnounced()
    {
        SettingsManager settings;
        CallDeviceController devices;
        devices.setSettings(&settings);
        const QString gone = QStringLiteral("/dev/video-lightning-test-gone");
        devices.selectCamera(gone);
        QVERIFY(devices.preferredCameraMissing());

        QSignalSpy selection(&devices, &CallDeviceController::selectionChanged);
        QSignalSpy active(&devices,
                          &CallDeviceController::activeDevicesChanged);
        devices.selectCamera(gone);
        QVERIFY2(!selection.isEmpty(),
                 "re-picking the same camera announced nothing");

        selection.clear();
        active.clear();
        devices.selectCamera(QString());
        QVERIFY(!devices.preferredCameraMissing());
        QVERIFY(!selection.isEmpty());
        QVERIFY2(!active.isEmpty(),
                 "missing -> system default keeps the empty id, and the "
                 "engine was never told the camera may open again");
    }

    void aMissingCameraSelectsNoRowAndSaysSo()
    {
        const auto read = [](const char *name) {
            QFile file(QStringLiteral(QML_DIR "/") + QLatin1String(name));
            return file.open(QIODevice::ReadOnly)
                ? QString::fromUtf8(file.readAll()).simplified()
                : QString();
        };
        const QString menu = read("CallDeviceMenu.qml");
        QVERIFY(!menu.isEmpty());
        QVERIFY2(QRegularExpression(QStringLiteral(
                     "root\\.kind === \"camera\"\\) return "
                     "app\\.callDevices\\.activeCameraId === \"\" && "
                     "!app\\.callDevices\\.preferredCameraMissing"))
                     .match(menu)
                     .hasMatch(),
                 "the menu's 'System default' radio is selected while the "
                 "chosen camera is missing");
        QVERIFY(menu.contains(QStringLiteral("\"callMenuCameraMissing\"")));

        const QString settingsUi = read("CallDeviceSettings.qml");
        QVERIFY(!settingsUi.isEmpty());
        QVERIFY2(settingsUi.contains(QStringLiteral(
                     "if (picker.missing) return -1;")),
                 "Settings selects 'System default' while the chosen camera "
                 "is missing");
        QVERIFY(settingsUi.contains(QStringLiteral(
            "return root.activated && app.callDevices.preferredCameraMissing;")));
        QVERIFY(settingsUi.contains(
            QStringLiteral("\"callDeviceCameraMissing\"")));
    }

    void aChosenMicrophoneTheCallCouldNotOpenIsReported()
    {
        SettingsManager settings;
        CallDeviceController devices;
        devices.setSettings(&settings);
        const QString chosen = QStringLiteral("lightning-test-mic-a");
        devices.selectMicrophone(chosen);
        QVERIFY(!devices.preferredMicrophoneUnusable());

        QSignalSpy selection(&devices, &CallDeviceController::selectionChanged);
        devices.noteMicrophoneBinding(chosen, true);
        QVERIFY(devices.preferredMicrophoneUnusable());
        QCOMPARE(selection.count(), 1);

        devices.noteMicrophoneBinding(QString(), false);
        devices.noteMicrophoneBinding(QStringLiteral("lightning-test-mic-b"),
                                      false);
        QVERIFY2(devices.preferredMicrophoneUnusable(),
                 "a report about another microphone cleared the fallback");

        devices.noteMicrophoneBinding(chosen, false);
        QVERIFY(!devices.preferredMicrophoneUnusable());
        QCOMPARE(selection.count(), 2);

        devices.noteMicrophoneBinding(chosen, true);
        devices.selectMicrophone(QStringLiteral("lightning-test-mic-b"));
        QVERIFY2(!devices.preferredMicrophoneUnusable(),
                 "choosing another microphone kept the old fallback notice");
        devices.selectMicrophone(QString());
        QVERIFY(!devices.preferredMicrophoneUnusable());

        const auto read = [](const char *name) {
            QFile file(QStringLiteral(QML_DIR "/") + QLatin1String(name));
            return file.open(QIODevice::ReadOnly)
                ? QString::fromUtf8(file.readAll()).simplified()
                : QString();
        };
        QVERIFY(read("CallDeviceMenu.qml")
                    .contains(QStringLiteral("\"callMenuMicrophoneUnusable\"")));
        QVERIFY(read("CallDeviceSettings.qml")
                    .contains(QStringLiteral(
                        "\"callDeviceMicrophoneUnusable\"")));
        QVERIFY(read("CallHeaderBar.qml")
                    .contains(QStringLiteral(
                        "|| app.callDevices.preferredMicrophoneUnusable")));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(CallDeviceControllerTest)
#include "CallDeviceControllerTest.moc"
