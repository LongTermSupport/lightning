#pragma once

#include <QByteArray>
#include <QtGlobal>

// A valid all-black 8- or 16-bit RGB PNG of any size, built without allocating the image; a
// 9000x9000 one is about 230 KB.
namespace pngfixture {

inline quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : data) {
        crc ^= quint8(c);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void appendBe32(QByteArray &out, quint32 v)
{
    out.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
}

inline void appendChunk(QByteArray &png, const char *type, const QByteArray &data)
{
    appendBe32(png, quint32(data.size()));
    const QByteArray body = QByteArray(type, 4) + data;
    png.append(body);
    appendBe32(png, crc32(body));
}

inline QByteArray blackRgb(int width, int height, int bitDepth = 8)
{
    const qint64 raw = qint64(height) * (1 + 3 * qint64(width) * (bitDepth / 8));
    QByteArray deflate;
    quint32 acc = 0;
    int bits = 0;
    const auto put = [&](quint32 value, int count) {
        acc |= value << bits;
        bits += count;
        while (bits >= 8) {
            deflate.append(char(acc & 0xFF));
            acc >>= 8;
            bits -= 8;
        }
    };
    // Huffman codes go out most significant bit first.
    const auto code = [&](quint32 value, int count) {
        for (int i = count - 1; i >= 0; --i)
            put((value >> i) & 1u, 1);
    };
    // One dynamic block whose only codes are literal 0 (10), end of block (11), length 258 (0)
    // and distance 1 (0): two bits per 258 zero bytes.
    put(1, 1);
    put(2, 2);
    put(29, 5);
    put(0, 5);
    put(14, 4);
    for (const int length : {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2})
        put(quint32(length), 3);
    code(3, 2);
    code(0, 1);
    put(127, 7);
    code(0, 1);
    put(106, 7);
    code(3, 2);
    code(0, 1);
    put(17, 7);
    code(2, 2);
    code(2, 2);
    code(2, 2);
    qint64 left = raw - 1;
    for (; left >= 258; left -= 258) {
        code(0, 1);
        code(0, 1);
    }
    for (; left > 0; --left)
        code(2, 2);
    code(3, 2);
    if (bits > 0)
        put(0, 8 - bits);

    QByteArray idat("\x78\x01", 2);
    idat.append(deflate);
    appendBe32(idat, quint32(raw % 65521) << 16 | 1u);

    QByteArray ihdr;
    appendBe32(ihdr, quint32(width));
    appendBe32(ihdr, quint32(height));
    ihdr.append(char(bitDepth)).append(char(2)).append(char(0)).append(char(0)).append(char(0));

    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", idat);
    appendChunk(png, "IEND", {});
    return png;
}

}
