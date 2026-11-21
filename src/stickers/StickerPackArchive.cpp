#include "stickers/StickerPackArchive.h"

#include "updater/SafeArchive.h"

#include <array>
#include <utility>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace stickers::archive {
namespace {

constexpr auto kFormat = "lightning.sticker-pack";
constexpr int kVersion = 1;
constexpr int kMaxBodyChars = 160;
constexpr int kMaxNameChars = 80;
constexpr int kMaxAttributionChars = 160;

quint32 crc32(const QByteArray &data)
{
    static constexpr auto table = [] {
        std::array<quint32, 256> entries{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            entries[i] = c;
        }
        return entries;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data)
        c = table[(c ^ quint8(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void le16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}
void le32(QByteArray &b, quint32 v)
{
    le16(b, quint16(v & 0xFFFF));
    le16(b, quint16(v >> 16));
}

QString oneLine(const QString &text, int maxChars)
{
    QString collapsed;
    collapsed.reserve(text.size());
    for (const QChar c : text)
        collapsed += (c.isNull() || c.category() == QChar::Other_Control)
                         ? QLatin1Char(' ') : c;
    return collapsed.simplified().left(maxChars);
}

QString normalizeUsage(const QString &usage)
{
    if (usage == QLatin1String("emoticon") || usage == QLatin1String("both"))
        return usage;
    return QStringLiteral("sticker");
}

}

bool isValidShortcode(const QString &key)
{
    if (key.isEmpty() || key.size() > kMaxShortcodeChars)
        return false;
    for (const QChar c : key) {
        const ushort u = c.unicode();
        const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                        || (u >= 'A' && u <= 'Z') || u == '-' || u == '_';
        if (!ok)
            return false;
    }
    return true;
}

QString proposeShortcode(const QString &requested)
{
    const QString trimmed = requested.trimmed();
    return isValidShortcode(trimmed) ? trimmed : sanitizeShortcode(trimmed);
}

QString sanitizeShortcode(const QString &raw)
{
    QString out;
    for (const QChar c : raw.trimmed()) {
        const ushort u = c.unicode();
        const bool alnum = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                           || (u >= 'A' && u <= 'Z');
        if (alnum || u == '-') {
            out += c;
        } else if (!out.endsWith(QLatin1Char('_'))) {
            out += QLatin1Char('_');
        }
        if (out.size() >= kMaxShortcodeChars)
            break;
    }
    int begin = 0;
    int end = out.size();
    while (begin < end && (out[begin] == QLatin1Char('-')
                           || out[begin] == QLatin1Char('_')))
        ++begin;
    while (end > begin && (out[end - 1] == QLatin1Char('-')
                           || out[end - 1] == QLatin1Char('_')))
        --end;
    return out.mid(begin, end - begin);
}

QString extensionForBytes(const QByteArray &b)
{
    if (b.size() >= 8 && b.startsWith("\x89PNG\r\n\x1A\n"))
        return QStringLiteral("png");
    if (b.size() >= 3 && quint8(b[0]) == 0xFF && quint8(b[1]) == 0xD8
        && quint8(b[2]) == 0xFF)
        return QStringLiteral("jpg");
    if (b.size() >= 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP")
        return QStringLiteral("webp");
    if (b.size() >= 6 && (b.startsWith("GIF87a") || b.startsWith("GIF89a")))
        return QStringLiteral("gif");
    return {};
}

QString mimeForExtension(const QString &ext)
{
    if (ext == QLatin1String("png")) return QStringLiteral("image/png");
    if (ext == QLatin1String("jpg")) return QStringLiteral("image/jpeg");
    if (ext == QLatin1String("webp")) return QStringLiteral("image/webp");
    if (ext == QLatin1String("gif")) return QStringLiteral("image/gif");
    return {};
}

WriteResult Writer::writeEntry(const QByteArray &name, const QByteArray &data)
{
    // Stored ZIP: local header, central record, two names, and the final directory trailer.
    const qint64 finalSize = m_offset + m_central.size() + data.size()
                             + 30 + 46 + 2 * name.size() + 22;
    if (finalSize > kMaxArchiveBytes)
        return WriteResult::TooLarge;
    const quint32 crc = crc32(data);
    QByteArray header;
    le32(header, 0x04034b50);
    le16(header, 20);
    le16(header, 0x0800);
    le16(header, 0);
    le16(header, 0);
    le16(header, 0x0021);
    le32(header, crc);
    le32(header, quint32(data.size()));
    le32(header, quint32(data.size()));
    le16(header, quint16(name.size()));
    le16(header, 0);
    header.append(name);
    if (m_output.write(header) != header.size() || m_output.write(data) != data.size())
        return WriteResult::WriteFailed;
    le32(m_central, 0x02014b50);
    le16(m_central, 20);
    le16(m_central, 20);
    le16(m_central, 0x0800);
    le16(m_central, 0);
    le16(m_central, 0);
    le16(m_central, 0x0021);
    le32(m_central, crc);
    le32(m_central, quint32(data.size()));
    le32(m_central, quint32(data.size()));
    le16(m_central, quint16(name.size()));
    le16(m_central, 0);
    le16(m_central, 0);
    le16(m_central, 0);
    le16(m_central, 0);
    le32(m_central, 0);
    le32(m_central, quint32(m_offset));
    m_central.append(name);
    m_offset += header.size() + data.size();
    return WriteResult::Ok;
}

WriteResult Writer::append(int index, const Image &image)
{
    const QString ext = extensionForBytes(image.data);
    const QString code = proposeShortcode(image.shortcode);
    if (ext.isEmpty() || image.data.size() > kMaxImageBytes || code.isEmpty()
        || m_images.size() >= kMaxImages || m_codes.contains(code) || m_images.contains(index))
        return WriteResult::Skipped;
    const QString file = QStringLiteral("images/%1-%2.%3")
                             .arg(index + 1, 4, 10, QLatin1Char('0'))
                             .arg(code.left(64), ext);
    const auto result = writeEntry(file.toUtf8(), image.data);
    if (result != WriteResult::Ok)
        return result;
    QJsonObject obj;
    obj.insert(QStringLiteral("shortcode"), code);
    obj.insert(QStringLiteral("file"), file);
    obj.insert(QStringLiteral("mimetype"), mimeForExtension(ext));
    obj.insert(QStringLiteral("usage"), normalizeUsage(image.usage));
    const QString body = oneLine(image.body, kMaxBodyChars);
    if (!body.isEmpty() && body != code)
        obj.insert(QStringLiteral("body"), body);
    m_codes.insert(code);
    m_images.insert(index, obj);
    return WriteResult::Ok;
}

WriteResult Writer::finish(const QString &name, const QString &attribution)
{
    if (m_images.isEmpty())
        return WriteResult::Skipped;
    QJsonArray images;
    for (const auto &image : std::as_const(m_images))
        images.append(image);
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QLatin1String(kFormat));
    manifest.insert(QStringLiteral("version"), kVersion);
    manifest.insert(QStringLiteral("name"), oneLine(name, kMaxNameChars));
    if (!attribution.isEmpty())
        manifest.insert(QStringLiteral("attribution"), oneLine(attribution, kMaxAttributionChars));
    manifest.insert(QStringLiteral("images"), images);
    const auto json = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (json.size() > kMaxManifestBytes)
        return WriteResult::TooLarge;
    const auto result = writeEntry(QByteArrayLiteral("pack.json"), json);
    if (result != WriteResult::Ok)
        return result;
    QByteArray out = m_central;
    le32(out, 0x06054b50);
    le16(out, 0);
    le16(out, 0);
    le16(out, quint16(m_images.size() + 1));
    le16(out, quint16(m_images.size() + 1));
    le32(out, quint32(m_central.size()));
    le32(out, quint32(m_offset));
    le16(out, 0);
    return m_output.write(out) == out.size() ? WriteResult::Ok : WriteResult::WriteFailed;
}

QByteArray buildArchive(const Pack &pack, int *skipped)
{
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    Writer writer(buffer);
    int skippedCount = 0;
    for (int i = 0; i < pack.images.size(); ++i) {
        const auto result = writer.append(i, pack.images.at(i));
        if (result == WriteResult::Skipped)
            ++skippedCount;
        else if (result != WriteResult::Ok)
            return {};
    }
    if (skipped)
        *skipped = skippedCount;
    if (writer.finish(pack.name, pack.attribution) != WriteResult::Ok)
        return {};
    return buffer.data();
}

ReadResult readArchive(const QString &zipPath, const QString &workDir)
{
    ReadResult result;
    updater::ArchiveLimits limits;
    limits.maxEntries = kMaxImages + 8;
    limits.maxEntryUncompressedBytes = kMaxImageBytes;
    limits.maxTotalUncompressedBytes = kMaxArchiveBytes;
    limits.maxArchiveBytes = kMaxArchiveBytes;
    limits.maxPathDepth = 4;
    const updater::ArchiveResult extracted =
        updater::extractZipSafely(zipPath, workDir, limits);
    if (!extracted.ok()) {
        result.error = ReadError::Unreadable;
        return result;
    }

    QFile manifestFile(QDir(workDir).filePath(QStringLiteral("pack.json")));
    if (!manifestFile.exists()) {
        result.error = ReadError::NoManifest;
        return result;
    }
    if (!manifestFile.open(QIODevice::ReadOnly)
        || manifestFile.size() > kMaxManifestBytes) {
        result.error = ReadError::BadManifest;
        return result;
    }
    QJsonParseError parse;
    const QJsonDocument doc =
        QJsonDocument::fromJson(manifestFile.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = ReadError::BadManifest;
        return result;
    }
    const QJsonObject manifest = doc.object();
    if (manifest.value(QStringLiteral("format")).toString()
            != QLatin1String(kFormat)
        || manifest.value(QStringLiteral("version")).toInt() != kVersion) {
        result.error = ReadError::BadManifest;
        return result;
    }
    result.pack.name = oneLine(
        manifest.value(QStringLiteral("name")).toString(), kMaxNameChars);
    result.pack.attribution = oneLine(
        manifest.value(QStringLiteral("attribution")).toString(),
        kMaxAttributionChars);

    static const QRegularExpression fileName(
        QStringLiteral("^images/[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    const QJsonArray entries = manifest.value(QStringLiteral("images")).toArray();
    QSet<QString> seen;
    for (const QJsonValue &value : entries) {
        const QJsonObject obj = value.toObject();
        const QString file = obj.value(QStringLiteral("file")).toString();
        if (result.pack.images.size() >= kMaxImages
            || !fileName.match(file).hasMatch()
            || file.contains(QLatin1String(".."))) {
            ++result.refused;
            continue;
        }
        const QString path = QDir(workDir).filePath(file);
        const QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() <= 0
            || info.size() > kMaxImageBytes) {
            ++result.refused;
            continue;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            ++result.refused;
            continue;
        }
        // The type comes from the bytes, never from the name or the manifest.
        // SVG (and everything not a plain raster) is refused here.
        const QString ext = extensionForBytes(f.read(16));
        if (ext.isEmpty()) {
            ++result.refused;
            continue;
        }
        QString code = proposeShortcode(
            obj.value(QStringLiteral("shortcode")).toString());
        if (code.isEmpty())
            code = sanitizeShortcode(QFileInfo(file).completeBaseName());
        if (code.isEmpty())
            code = QStringLiteral("sticker");
        if (seen.contains(code)) {
            ++result.refused;
            continue;
        }
        seen.insert(code);
        Image image;
        image.shortcode = code;
        image.body = oneLine(obj.value(QStringLiteral("body")).toString(),
                             kMaxBodyChars);
        image.mimetype = mimeForExtension(ext);
        image.usage = normalizeUsage(
            obj.value(QStringLiteral("usage")).toString());
        image.path = path;
        result.pack.images.append(image);
    }
    if (result.pack.images.isEmpty())
        result.error = ReadError::NoImages;
    return result;
}

}
