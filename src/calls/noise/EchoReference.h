#pragma once

#include <QString>
#include <gst/gst.h>
#include <mutex>

namespace calls::noise {

class EchoReference
{
public:
    static constexpr const char *kProbeName = "webrtcechoprobe0";
    static QString probeStage();
    ~EchoReference();
    void alignPublisherClock(GstElement *pipeline);
    bool attach(GstElement *receiveBin, GstElement *volume);
    static void detach(GstElement *receiveBin);
    GstElement *probe() const;

private:
    void alignClock(GstElement *pipeline);
    bool start();
    mutable std::mutex m_mutex;
    GstClockTime m_origin = GST_CLOCK_TIME_NONE;
    GstElement *m_pipeline = nullptr;
    GstElement *m_mixer = nullptr;
};

}
