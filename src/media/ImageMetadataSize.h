#pragma once

#include <QImageIOHandler>
#include <QImageReader>
#include <QSize>

namespace imagemeta {

// The size a viewer draws: QImageReader::size() ignores the EXIF orientation.
inline QSize displayedSize(QImageReader &reader)
{
    QSize size = reader.size();
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        size.transpose();
    return size;
}

}
