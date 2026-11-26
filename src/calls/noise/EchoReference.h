#pragma once

#include <QString>
#include <gst/gst.h>
#include <mutex>

namespace calls::noise {

/// The far end webrtcdsp cancels: every received audio track of a call, after
/// its volume and mute, mixed into the one webrtcechoprobe a DSP can hold.
/// Shared by all engines of one call; its pipeline and the publisher's share a
/// base time so the probe's and the DSP's timestamps compare.
class EchoReference
{
public:
    static constexpr const char *kProbeName = "webrtcechoprobe0";
    static QString probeStage();
    static bool available();
    ~EchoReference();
    /// Before the publisher pipeline goes to PLAYING.
    void alignPublisherClock(GstElement *pipeline);
    bool attach(GstElement *receiveBin, GstElement *volume);
    static void detach(GstElement *receiveBin);
    GstElement *probe() const;
    int inputCount() const;

private:
    void alignClock(GstElement *pipeline);
    bool start();
    mutable std::mutex m_mutex;
    GstClockTime m_origin = GST_CLOCK_TIME_NONE;
    GstElement *m_pipeline = nullptr;
    GstElement *m_mixer = nullptr;
};

}
