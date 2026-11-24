
#include "media/VoiceRecorder.h"

#ifdef LIGHTNING_VOICE_BUFFER_PROBE
#include <QAudioBufferInput>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QSysInfo>
#include <gst/gst.h>
#endif

#include <QFile>
#include <QMediaDevices>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest/QtTest>

class VoiceFixC43Test : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aVanishedMicrophoneFailsOnceAndDiscardsTheClip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString clip = dir.filePath(QStringLiteral("clip.ogg"));
        QFile f(clip);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("OggS-captured");
        f.close();

        VoiceRecorder rec;
        rec.setRecordingForTest("mic-1", clip, 4200);
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        QSignalSpy ready(&rec, &VoiceRecorder::ready);
        rec.inputDevicesChanged({});
        QVERIFY(rec.endedByDisconnect());
        QVERIFY(!rec.recording());
        QVERIFY(rec.processing());
        rec.finishForTest();
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!rec.processing());
        QVERIFY(!QFile::exists(clip));
    }

    void anOrdinaryStopStillEmitsReady()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("mic-1", QString(), 1000);
        QSignalSpy ready(&rec, &VoiceRecorder::ready);
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        rec.stop();
        rec.finishForTest();
        QCOMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!rec.endedByDisconnect());
    }

    void aListedDeviceOrAnUnrelatedChangeLeavesTheRecordingAlone()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("mic-1");
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        rec.inputDevicesChanged({{"mic-2", false}, {"mic-1", false}});
        QCOMPARE(failed.count(), 0);
        QVERIFY(rec.recording());
        QVERIFY(!rec.endedByDisconnect());
        VoiceRecorder anon;
        anon.setRecordingForTest(QByteArray());
        anon.inputDevicesChanged({});
        QVERIFY(anon.recording());
    }

    void aReplacementDefaultInterruptsTheCurrentTake()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("old-default");
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        QSignalSpy ready(&rec, &VoiceRecorder::ready);
        rec.inputDevicesChanged({{"usb", false}, {"new-default", true}});
        QVERIFY(rec.endedByDisconnect());
        rec.finishForTest();
        QCOMPARE(failed.size(), 1);
        QVERIFY(ready.isEmpty());
    }

    void syntheticSamplesAcrossInputChanges_data()
    {
        QTest::addColumn<bool>("lost");
        QTest::newRow("default-changes-original-remains") << false;
        QTest::newRow("original-input-removed") << true;
    }

    void syntheticSamplesAcrossInputChanges()
    {
#ifdef LIGHTNING_VOICE_BUFFER_PROBE
        QFETCH(bool, lost);
        qInfo() << "voice continuity platform" << QSysInfo::kernelType()
                << "Qt" << qVersion() << "backend" << qgetenv("QT_MEDIA_BACKEND");
        if (!qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND")
            && qgetenv("QT_MEDIA_BACKEND") != "ffmpeg")
            QSKIP("Buffer-input recording needs the ffmpeg media backend");
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("GStreamer unavailable");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "audiotestsrc wave=sine freq=440 samplesperbuffer=1600 num-buffers=1 "
            "! audio/x-raw,format=S16LE,rate=16000,channels=1 ! fakesink name=probe", &error);
        if (error || !pipeline) {
            g_clear_error(&error);
            if (pipeline)
                gst_object_unref(pipeline);
            QSKIP("Synthetic tone source unavailable");
        }
        GstBus *bus = gst_element_get_bus(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstMessage *end = gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND,
            GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "probe");
        GstSample *sample = nullptr;
        g_object_get(sink, "last-sample", &sample, nullptr);
        QByteArray pcm(3200, '\0');
        const bool tone = end && GST_MESSAGE_TYPE(end) == GST_MESSAGE_EOS && sample
            && gst_buffer_extract(gst_sample_get_buffer(sample), 0, pcm.data(), pcm.size()) == 3200;
        if (end)
            gst_message_unref(end);
        if (sample)
            gst_sample_unref(sample);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        QVERIFY(tone);

        QMediaFormat codec(QMediaFormat::Ogg);
        codec.setAudioCodec(QMediaFormat::AudioCodec::Opus);
        if (!codec.isSupported(QMediaFormat::Encode))
            QSKIP("Ogg/Opus recording unavailable on this backend");
        QAudioFormat format;
        format.setSampleRate(16000);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        QAudioBufferInput input(format);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("probe.ogg"));
        VoiceRecorder rec;
        QVERIFY(rec.ensureRecorder());
        rec.m_session->setAudioBufferInput(&input);
        rec.m_recorder->setMediaFormat(codec);
        rec.m_recorder->setOutputLocation(QUrl::fromLocalFile(file));
        rec.m_activeFile = file;
        QSignalSpy ready(&rec, &VoiceRecorder::ready);
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        QVERIFY(rec.beginRecording("synthetic-a"));
        const auto send = [&](const QByteArray &data, int index) {
            const QAudioBuffer buffer(data, format, qint64(index) * 100000);
            return QTest::qWaitFor([&] { return input.sendAudioBuffer(buffer); }, 2000);
        };
        for (int i = 0; i < 10; ++i)
            QVERIFY(send(pcm, i));
        if (lost) {
            rec.inputDevicesChanged({{"synthetic-b", true}});
            QTRY_COMPARE(failed.size(), 1);
            QVERIFY(rec.endedByDisconnect());
            QVERIFY(ready.isEmpty());
            QVERIFY(!QFile::exists(file));
        } else {
            rec.inputDevicesChanged({{"synthetic-b", true}, {"synthetic-a", false}});
            QVERIFY(rec.recording());
            const QByteArray silence(pcm.size(), '\0');
            for (int i = 10; i < 30; ++i)
                QVERIFY(send(i < 20 ? silence : pcm, i));
            rec.stop();
            QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 15000);
            QVERIFY(failed.isEmpty());
            const QList<int> peaks = ready.front().at(3).value<QList<int>>();
            QVERIFY(peaks.size() >= 28);
            QVERIFY(peaks.at(5) > 50);
            QVERIFY(peaks.at(15) < 10);
            QVERIFY(peaks.at(25) > 50);
        }
#else
        QSKIP("Requires Qt 6.8 buffer input and the synthetic GStreamer source");
#endif
    }

    void theDeviceWatchIsConnected()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("mic-that-is-not-real");
        QMediaDevices *devices = rec.watchInputDevicesForTest();
        QVERIFY(devices != nullptr);
        QVERIFY(rec.recording());
        Q_EMIT devices->audioInputsChanged();
        QVERIFY(!rec.recording());
        QVERIFY(rec.endedByDisconnect());
    }
};

QTEST_GUILESS_MAIN(VoiceFixC43Test)
#include "VoiceFixC43Test.moc"
