#include "calls/noise/EchoReference.h"

#include <gst/app/gstappsrc.h>
#include <atomic>

namespace calls::noise {
namespace {

struct Input {
    GstElement *pipeline;
    GstElement *source;
    GstPad *sinkPad;
    GstClockTime origin;
    GstSegment segment;
    std::atomic<GstClockTime> latency{0};

    ~Input()
    {
        GstElement *mixer = gst_pad_get_parent_element(sinkPad);
        gst_element_set_state(source, GST_STATE_NULL);
        gst_element_release_request_pad(mixer, sinkPad);
        gst_object_unref(mixer);
        gst_object_unref(sinkPad);
        gst_bin_remove(GST_BIN(pipeline), source);
        gst_object_unref(pipeline);
    }
};

GstPadProbeReturn copyPlayback(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    auto *input = static_cast<Input *>(data);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_CAPS: {
            GstCaps *caps;
            gst_event_parse_caps(event, &caps);
            gst_app_src_set_caps(GST_APP_SRC(input->source), caps);
            break;
        }
        case GST_EVENT_SEGMENT:
            gst_event_copy_segment(event, &input->segment);
            break;
        case GST_EVENT_LATENCY: {
            GstClockTime latency;
            gst_event_parse_latency(event, &latency);
            input->latency.store(latency);
            break;
        }
        default:
            break;
        }
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (input->segment.format != GST_FORMAT_TIME || !GST_BUFFER_PTS_IS_VALID(buffer))
        return GST_PAD_PROBE_OK;
    const GstClockTime running = gst_segment_to_running_time(
        &input->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    if (!GST_CLOCK_TIME_IS_VALID(running))
        return GST_PAD_PROBE_OK;
    // Subscriber pipelines share the system clock but start at different times.
    const GstClockTime playTime = gst_element_get_base_time(GST_PAD_PARENT(pad))
        + running + input->latency.load();
    if (playTime >= input->origin) {
        GstBuffer *copy = gst_buffer_copy(buffer);
        GST_BUFFER_PTS(copy) = playTime - input->origin;
        GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
        gst_app_src_push_buffer(GST_APP_SRC(input->source), copy);
    }
    return GST_PAD_PROBE_OK;
}

}

EchoReference::~EchoReference()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_mixer);
        gst_object_unref(m_pipeline);
    }
}

void EchoReference::alignClock(GstElement *pipeline)
{
    GstClock *clock = gst_system_clock_obtain();
    if (!GST_CLOCK_TIME_IS_VALID(m_origin))
        m_origin = gst_clock_get_time(clock);
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    gst_element_set_start_time(pipeline, GST_CLOCK_TIME_NONE);
    gst_element_set_base_time(pipeline, m_origin);
    gst_object_unref(clock);
}

void EchoReference::alignPublisherClock(GstElement *pipeline)
{
    std::lock_guard lock(m_mutex);
    alignClock(pipeline);
}

QString EchoReference::probeStage()
{
    return QStringLiteral("! audio/x-raw,format=S16LE,layout=interleaved,"
                          "rate=48000,channels=2 ! webrtcechoprobe name=%1 ")
        .arg(QLatin1String(kProbeName));
}

bool EchoReference::start()
{
    if (m_pipeline)
        return true;
    const QString chain = QStringLiteral(
        "audiomixer name=mix force-live=true ignore-inactive-pads=true "
        "start-time-selection=first %1! fakesink sync=true async=false processing-deadline=0 "
        "enable-last-sample=false").arg(probeStage());
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(chain.toUtf8().constData(), &error);
    if (error || !pipeline) {
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }
    alignClock(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_flushing(bus, TRUE);
    gst_object_unref(bus);
    m_pipeline = pipeline;
    m_mixer = gst_bin_get_by_name(GST_BIN(pipeline), "mix");
    return true;
}

bool EchoReference::attach(GstElement *receiveBin, GstElement *volume)
{
    std::lock_guard lock(m_mutex);
    if (!start())
        return false;
    GstElement *source = gst_element_factory_make("appsrc", nullptr);
    if (!source)
        return false;
    g_object_set(source, "format", GST_FORMAT_TIME, "is-live", TRUE,
                 "max-buffers", guint64(5), "max-time", guint64(100 * GST_MSECOND),
                 "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM, nullptr);
    if (!gst_bin_add(GST_BIN(m_pipeline), source)) {
        gst_object_unref(source);
        return false;
    }
    GstPad *sinkPad = gst_element_request_pad_simple(m_mixer, "sink_%u");
    GstPad *out = gst_element_get_static_pad(source, "src");
    const bool linked = sinkPad && gst_pad_link(out, sinkPad) == GST_PAD_LINK_OK;
    gst_object_unref(out);
    if (!linked || !gst_element_sync_state_with_parent(source)) {
        gst_element_set_state(source, GST_STATE_NULL);
        if (sinkPad) {
            gst_element_release_request_pad(m_mixer, sinkPad);
            gst_object_unref(sinkPad);
        }
        gst_bin_remove(GST_BIN(m_pipeline), source);
        return false;
    }
    auto *input = new Input{GST_ELEMENT(gst_object_ref(m_pipeline)), source,
                            sinkPad, m_origin, {}};
    gst_segment_init(&input->segment, GST_FORMAT_UNDEFINED);
    GstPad *pad = gst_element_get_static_pad(volume, "src");
    const gulong id = gst_pad_add_probe(
        pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_BOTH),
        copyPlayback, input, [](gpointer p) { delete static_cast<Input *>(p); });
    struct Tap {
        GstPad *pad;
        gulong id;
    };
    g_object_set_data_full(G_OBJECT(receiveBin), "echo-input", new Tap{pad, id},
        [](gpointer data) {
            auto *tap = static_cast<Tap *>(data);
            gst_pad_remove_probe(tap->pad, tap->id);
            gst_object_unref(tap->pad);
            delete tap;
        });
    return true;
}

void EchoReference::detach(GstElement *receiveBin)
{
    g_object_set_data(G_OBJECT(receiveBin), "echo-input", nullptr);
}

GstElement *EchoReference::probe() const
{
    std::lock_guard lock(m_mutex);
    return m_pipeline ? gst_bin_get_by_name(GST_BIN(m_pipeline), kProbeName)
                      : nullptr;
}

}
