#include "calls/noise/EchoReference.h"

#include <gst/app/gstappsrc.h>
#include <algorithm>
#include <atomic>

namespace calls::noise {
namespace {

// The reference pipeline's latency: every buffer is stamped this much early
// and webrtcechoprobe adds it back, so the mix reaches the probe before the
// track plays. It must exceed the mixer's wait plus one 10 ms output block.
constexpr GstClockTime kLead = 40 * GST_MSECOND;
constexpr GstClockTime kMixerWait = 20 * GST_MSECOND;
constexpr const char *kTapKey = "lightning-echo-tap";
constexpr const char *kMixFormat =
    "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2";

struct Input {
    GstElement *pipeline;
    GstElement *branch;
    GstElement *source;
    GstPad *mixerPad;
    GstClock *clock;
    GstClockTime origin;
    GstSegment segment;
    std::atomic<GstClockTime> latency{0};

    ~Input()
    {
        if (GstElement *mixer = gst_pad_get_parent_element(mixerPad)) {
            gst_element_release_request_pad(mixer, mixerPad);
            gst_object_unref(mixer);
        }
        gst_object_unref(mixerPad);
        gst_element_set_state(branch, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(pipeline), branch);
        gst_object_unref(branch);
        gst_object_unref(source);
        gst_object_unref(clock);
        gst_object_unref(pipeline);
    }
};

struct Tap {
    GstPad *pad;
    gulong id;
};

GstPadProbeReturn copyPlayback(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    auto *input = static_cast<Input *>(data);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_CAPS: {
            GstCaps *caps = nullptr;
            gst_event_parse_caps(event, &caps);
            gst_app_src_set_caps(GST_APP_SRC(input->source), caps);
            break;
        }
        case GST_EVENT_SEGMENT:
            gst_event_copy_segment(event, &input->segment);
            break;
        case GST_EVENT_LATENCY: {
            GstClockTime latency = 0;
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
    if (!buffer)
        return GST_PAD_PROBE_OK;
    const GstClockTime earliest = gst_clock_get_time(input->clock) + kLead;
    GstClockTime play = earliest;
    GstElement *element = GST_PAD_PARENT(pad);
    if (element && input->segment.format == GST_FORMAT_TIME
        && GST_BUFFER_PTS_IS_VALID(buffer)) {
        const GstClockTime running = gst_segment_to_running_time(
            &input->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
        // Where the sink plays it; one already late plays as soon as it can.
        if (GST_CLOCK_TIME_IS_VALID(running))
            play = std::max(earliest, gst_element_get_base_time(element) + running
                                          + input->latency.load());
    }
    if (play < input->origin + kLead)
        return GST_PAD_PROBE_OK;
    GstBuffer *copy = gst_buffer_copy(buffer);
    GST_BUFFER_PTS(copy) = play - input->origin - kLead;
    GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(input->source), copy);
    return GST_PAD_PROBE_OK;
}

bool factoryExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

}

QString EchoReference::probeStage()
{
    return QStringLiteral("! %1 ! webrtcechoprobe name=%2 ")
        .arg(QLatin1String(kMixFormat), QLatin1String(kProbeName));
}

bool EchoReference::available()
{
    static const bool present = factoryExists("webrtcechoprobe")
        && factoryExists("audiomixer") && factoryExists("appsrc");
    return present;
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

bool EchoReference::start()
{
    if (m_pipeline)
        return true;
    if (!available())
        return false;
    // force-live is construct-only, so not settable from a launch string.
    GstElement *mixer = gst_element_factory_make_full(
        "audiomixer", "name", "echomix", "force-live", TRUE,
        "ignore-inactive-pads", TRUE, "latency", guint64(kMixerWait), nullptr);
    if (!mixer)
        return false;
    gst_util_set_object_arg(G_OBJECT(mixer), "start-time-selection", "first");
    const QString tail = QStringLiteral("capsfilter caps=\"%1\" ! webrtcechoprobe name=%2 "
                                        "! fakesink sync=false async=false "
                                        "enable-last-sample=false")
                             .arg(QLatin1String(kMixFormat), QLatin1String(kProbeName));
    GError *error = nullptr;
    GstElement *sink = gst_parse_bin_from_description(tail.toUtf8().constData(),
                                                      TRUE, &error);
    if (error || !sink) {
        g_clear_error(&error);
        if (sink)
            gst_object_unref(gst_object_ref_sink(sink));
        gst_object_unref(gst_object_ref_sink(mixer));
        return false;
    }
    GstElement *pipeline = gst_pipeline_new("echoreference");
    gst_bin_add_many(GST_BIN(pipeline), mixer, sink, nullptr);
    alignClock(pipeline);
    gst_pipeline_set_latency(GST_PIPELINE(pipeline), kLead);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_flushing(bus, TRUE);
    gst_object_unref(bus);
    if (!gst_element_link(mixer, sink)
        || gst_element_set_state(pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    m_pipeline = pipeline;
    m_mixer = GST_ELEMENT(gst_object_ref(mixer));
    return true;
}

bool EchoReference::attach(GstElement *receiveBin, GstElement *volume)
{
    std::lock_guard lock(m_mutex);
    if (!start())
        return false;
    // Bounded by time; a full second is held only if playback is that far
    // ahead, and the oldest goes first.
    GError *error = nullptr;
    GstElement *branch = gst_parse_bin_from_description(
        "appsrc name=src format=time is-live=true max-buffers=0 max-bytes=0 "
        "max-time=1000000000 leaky-type=downstream "
        "! audioconvert ! audioresample ! capsfilter name=format",
        TRUE, &error);
    if (error || !branch) {
        g_clear_error(&error);
        if (branch)
            gst_object_unref(gst_object_ref_sink(branch));
        return false;
    }
    gst_object_ref_sink(branch);
    if (GstElement *format = gst_bin_get_by_name(GST_BIN(branch), "format")) {
        GstCaps *caps = gst_caps_from_string(kMixFormat);
        g_object_set(format, "caps", caps, nullptr);
        gst_caps_unref(caps);
        gst_object_unref(format);
    }
    GstElement *source = gst_bin_get_by_name(GST_BIN(branch), "src");
    GstPad *mixerPad = nullptr;
    bool linked = false;
    if (source && gst_bin_add(GST_BIN(m_pipeline), branch)) {
        mixerPad = gst_element_request_pad_simple(m_mixer, "sink_%u");
        GstPad *out = gst_element_get_static_pad(branch, "src");
        linked = mixerPad && out && gst_pad_link(out, mixerPad) == GST_PAD_LINK_OK
            && gst_element_sync_state_with_parent(branch);
        if (out)
            gst_object_unref(out);
        if (!linked) {
            if (mixerPad) {
                gst_element_release_request_pad(m_mixer, mixerPad);
                gst_object_unref(mixerPad);
            }
            gst_element_set_state(branch, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(m_pipeline), branch);
        }
    }
    if (!linked) {
        if (source)
            gst_object_unref(source);
        gst_object_unref(branch);
        return false;
    }

    auto *input = new Input{GST_ELEMENT(gst_object_ref(m_pipeline)), branch, source,
                            mixerPad, gst_system_clock_obtain(), m_origin, {}};
    gst_segment_init(&input->segment, GST_FORMAT_UNDEFINED);
    GstPad *pad = gst_element_get_static_pad(volume, "src");
    if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        gst_caps_unref(caps);
    }
    if (GstEvent *segment = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0)) {
        gst_event_copy_segment(segment, &input->segment);
        gst_event_unref(segment);
    }
    const gulong id = gst_pad_add_probe(
        pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_BOTH),
        copyPlayback, input, [](gpointer p) { delete static_cast<Input *>(p); });
    g_object_set_data_full(G_OBJECT(receiveBin), kTapKey, new Tap{pad, id},
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
    g_object_set_data(G_OBJECT(receiveBin), kTapKey, nullptr);
}

GstElement *EchoReference::probe() const
{
    std::lock_guard lock(m_mutex);
    return m_pipeline ? gst_bin_get_by_name(GST_BIN(m_pipeline), kProbeName)
                      : nullptr;
}

int EchoReference::inputCount() const
{
    std::lock_guard lock(m_mutex);
    if (!m_mixer)
        return 0;
    GST_OBJECT_LOCK(m_mixer);
    const int count = m_mixer->numsinkpads;
    GST_OBJECT_UNLOCK(m_mixer);
    return count;
}

}
