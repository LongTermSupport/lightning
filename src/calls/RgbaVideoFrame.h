#pragma once

#include <QSize>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <gst/gst.h>
#include <gst/video/video.h>

#include <cstring>

namespace lightning::calls {

/// The RGBA sample as a frame, or an invalid frame when the sample is not a
/// complete RGBA picture: never a partly filled one.
inline QVideoFrame rgbaFrameFromSample(GstSample *sample)
{
    GstCaps *caps = sample ? gst_sample_get_caps(sample) : nullptr;
    GstBuffer *buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    GstVideoInfo info;
    // Dimensions come from the peer and size an allocation: bound them.
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)
        || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA
        || info.width <= 0 || info.height <= 0 || info.width > 8192
        || info.height > 8192)
        return {};
    // Follows a GstVideoMeta's stride, and refuses a buffer shorter than the
    // layout the caps describe.
    GstVideoFrame source;
    if (!gst_video_frame_map(&source, &info, buffer, GST_MAP_READ))
        return {};
    const int width = info.width;
    const int height = info.height;
    const qsizetype rowBytes = qsizetype(width) * 4;
    const qsizetype sourceStride = GST_VIDEO_FRAME_PLANE_STRIDE(&source, 0);
    const auto *from =
        static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&source, 0));
    QVideoFrame frame(QVideoFrameFormat(QSize(width, height),
                                        QVideoFrameFormat::Format_RGBA8888));
    bool filled = false;
    if (from && sourceStride >= rowBytes && frame.map(QVideoFrame::WriteOnly)) {
        const qsizetype targetStride = frame.bytesPerLine(0);
        uchar *to = frame.bits(0);
        if (to && targetStride >= rowBytes) {
            for (int row = 0; row < height; ++row)
                std::memcpy(to + row * targetStride, from + row * sourceStride,
                            size_t(rowBytes));
            filled = true;
        }
        frame.unmap();
    }
    gst_video_frame_unmap(&source);
    return filled ? frame : QVideoFrame();
}

}
