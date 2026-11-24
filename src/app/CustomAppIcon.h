#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

// Validation and normalization for the user-selected custom app icon.
//
// - The format comes from magic-byte sniffing, never the file name or MIME.
// - Only static raster formats are accepted; SVG is rejected outright.
// - Decoding is bounded in bytes, dimensions and allocation.
// - Output matches the default logo: center-cropped, 512x512, circular alpha.
namespace appicon {

inline constexpr qsizetype kMaxInputBytes = 32LL * 1024 * 1024;
inline constexpr int kMaxInputEdge = 8192;
inline constexpr qint64 kMaxDecodeBytes = qint64(128) << 20;
// The largest square both limits admit at the 32 bpp the decode check assumes; what the user is told.
inline constexpr int kMaxSquareEdge = [] {
    int edge = kMaxInputEdge;
    while (qint64(edge) * edge * 4 > kMaxDecodeBytes)
        --edge;
    return edge;
}();
inline constexpr int kMinInputEdge = 16;
inline constexpr int kNormalizedEdge = 512;

struct NormalizeResult {
    bool ok = false;
    QString category;
    QImage image;
};

QString sniffedRasterFormat(const QByteArray &bytes);

NormalizeResult normalizeIconBytes(const QByteArray &bytes);

}
