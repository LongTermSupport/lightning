#pragma once

#include <QImageIOHandler>
#include <QImageReader>
#include <QSize>

#include <cmath>

namespace imagemeta {

// The size a viewer draws: QImageReader::size() ignores the EXIF orientation.
inline QSize displayedSize(QImageReader &reader)
{
    QSize size = reader.size();
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        size.transpose();
    return size;
}

// Shrinks a scaled-size target (stored orientation) until the decode fits budgetBytes at
// 32 bpp. Qt's JPEG decoder allocates natural * ceil(8 * target / natural) / 8 before it
// scales, so a target just under the natural size still allocates the full image.
inline QSize decodeSizeWithinBudget(QImageReader &reader, QSize target,
                                    qint64 budgetBytes)
{
    const QSize natural = reader.size();
    if (!natural.isValid() || natural.isEmpty())
        return target;
    if (!target.isValid() || target.isEmpty())
        target = natural;
    const auto bytes = [](QSize s) { return qint64(s.width()) * s.height() * 4; };
    if (bytes(target) > budgetBytes) {
        const double k = std::sqrt(double(budgetBytes) / double(bytes(target)));
        target = QSize(qMax(1, int(target.width() * k)),
                       qMax(1, int(target.height() * k)));
    }
    if (reader.format() != "jpeg")
        return target;
    for (int eighths = 8; eighths >= 1; --eighths) {
        const QSize step((natural.width() * eighths + 7) / 8,
                         (natural.height() * eighths + 7) / 8);
        if (bytes(step) > budgetBytes)
            continue;
        if (eighths < 8) {
            // One pixel under the step so the decoder's float ratio cannot round up into the next one.
            const QSize cap = natural.scaled(qMax(1, natural.width() * eighths / 8 - 1),
                                             qMax(1, natural.height() * eighths / 8 - 1),
                                             Qt::KeepAspectRatio);
            if (target.width() > cap.width() || target.height() > cap.height())
                target = cap;
        }
        break;
    }
    return target;
}

}
