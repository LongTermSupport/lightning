
#include "app/GuiStallTracer.h"
#include "app/LogExport.h"
#include "matrix/MatrixClient.h"
#include "media/ImageFormatSupport.h"
#include "media/ImageMetadata.h"
#include "media/ImageSendPrep.h"
#include "media/StagedImageStore.h"
#include "media/SvgRasterJob.h"
#include "models/MessageComposer.h"
#include "storage/PortableMode.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest/QtTest>

#include <memory>
#include <tuple>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastOpId = 0;
    int fileSends = 0;
    int byteSends = 0;
    QString lastMime;
    QString lastFilename;
    qint64 serverLimit = 0;
    bool rejectSends = false;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString userId = QStringLiteral("@me:example.org");
    QString currentUserId() const override { return userId; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsAttachmentSend() const override { return true; }
    qint64 maxUploadSize() const override { return serverLimit; }
    QString lastFilePath;
    QStringList sent;
    int lastFileWidth = 0;
    int lastFileHeight = 0;
    quint64 sendAttachment(const QString &room, const QString &path,
                           const QString &mime, const QString &caption,
                           int width, int height, bool, qint64 durationMs,
                           const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++fileSends;
        sent << room + QLatin1Char('|') + path;
        lastFilePath = path;
        lastFileWidth = width;
        lastFileHeight = height;
        lastReplyTo = replyTo;
        lastCaption = caption;
        lastMime = mime;
        lastDurationMs = durationMs;
        lastOpId = nextOp++;
        return lastOpId;
    }
    QString lastReplyTo;
    QString lastCaption;
    QStringList scheduledCaptions;
    quint64 prepareScheduledMedia(const QString &, const QString &,
                                  const QString &, const QByteArray &,
                                  const QString &, const QString &,
                                  const QString &caption, int, int, qint64, bool,
                                  bool, const QByteArray &, int, int,
                                  const QString &) override
    {
        scheduledCaptions << caption;
        return nextOp++;
    }
    int threadFileSends = 0;
    QString lastThreadRoot;
    quint64 sendThreadAttachment(const QString &, const QString &root,
                                 const QString &path, const QString &mime,
                                 const QString &caption, int width, int height,
                                 bool, qint64, const QString &replyTo) override
    {
        ++threadFileSends;
        lastThreadRoot = root;
        lastFilePath = path;
        lastMime = mime;
        lastCaption = caption;
        lastFileWidth = width;
        lastFileHeight = height;
        lastReplyTo = replyTo;
        lastOpId = nextOp++;
        return lastOpId;
    }
    int videoSends = 0;
    QByteArray lastThumbnail;
    int lastThumbWidth = 0;
    int lastThumbHeight = 0;
    int lastVideoWidth = 0;
    int lastVideoHeight = 0;
    qint64 lastDurationMs = 0;
    quint64 sendVideo(const QString &, const QString &, const QString &mime,
                      const QString &, int width, int height,
                      qint64 durationMs, const QByteArray &thumbnail,
                      int thumbnailWidth, int thumbnailHeight,
                      const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++videoSends;
        lastReplyTo = replyTo;
        lastMime = mime;
        lastVideoWidth = width;
        lastVideoHeight = height;
        lastDurationMs = durationMs;
        lastThumbnail = thumbnail;
        lastThumbWidth = thumbnailWidth;
        lastThumbHeight = thumbnailHeight;
        lastOpId = nextOp++;
        return lastOpId;
    }
    int imageThumbSends = 0;
    int lastImageWidth = 0;
    int lastImageHeight = 0;
    quint64 sendImageWithThumbnail(const QString &, const QString &,
                                   const QString &mime, const QString &,
                                   int width, int height,
                                   const QByteArray &thumbnail,
                                   int thumbnailWidth,
                                   int thumbnailHeight,
                                   const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++imageThumbSends;
        lastReplyTo = replyTo;
        lastMime = mime;
        lastImageWidth = width;
        lastImageHeight = height;
        lastThumbnail = thumbnail;
        lastThumbWidth = thumbnailWidth;
        lastThumbHeight = thumbnailHeight;
        lastOpId = nextOp++;
        return lastOpId;
    }
    bool supportsRoomScopedAttachmentSend() const override { return true; }
    int roomBytesSends = 0;
    quint64 sendAttachmentBytesToRoom(const QString &, const QByteArray &bytes,
                                      const QString &, const QString &mime,
                                      const QString &caption, int,
                                      int) override
    {
        ++roomBytesSends;
        lastBytes = bytes;
        lastMime = mime;
        lastCaption = caption;
        lastOpId = nextOp++;
        return lastOpId;
    }
    bool captionedBytes = false;
    int captionedByteSends = 0;
    QString lastCaptionedRoot;
    bool supportsCaptionedAttachmentBytes() const { return captionedBytes; }
    quint64 sendCaptionedAttachmentBytes(const QString &, const QString &root,
                                         const QByteArray &bytes, const QString &,
                                         const QString &, const QString &caption,
                                         int, int, const QString &replyTo)
    {
        ++captionedByteSends;
        lastCaptionedRoot = root;
        lastBytes = bytes;
        lastCaption = caption;
        lastReplyTo = replyTo;
        lastOpId = nextOp++;
        return lastOpId;
    }
    QByteArray lastBytes;
    quint64 sendAttachmentBytes(const QString &, const QByteArray &bytes,
                                const QString &filename, const QString &mime,
                                int, int, const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++byteSends;
        lastBytes = bytes;
        lastReplyTo = replyTo;
        lastFilename = filename;
        lastMime = mime;
        lastOpId = nextOp++;
        return lastOpId;
    }
    int voiceSends = 0;
    QString lastVoiceRoom;
    quint64 sendVoiceMessage(const QString &roomId, const QString &,
                             const QString &, qint64,
                             const QList<int> &,
                             const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++voiceSends;
        lastReplyTo = replyTo;
        lastVoiceRoom = roomId;
        lastOpId = nextOp++;
        return lastOpId;
    }
};

QString writeFile(const QTemporaryDir &dir, const QString &name,
                  const QByteArray &content)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    file.close();
    return path;
}

QByteArray tinyPng()
{
    QByteArray png = QByteArray::fromHex(
        "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
        "0000000d4944415478da63fcff9fa10e0003030101c9fe92ef0000000049454e44"
        "ae426082");
    return png;
}

QByteArray decodablePng()
{
    QImage image(4, 3, QImage::Format_ARGB32);
    image.fill(QColor(20, 120, 220, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray tinyMatroskaHeader()
{
    return QByteArray::fromHex(
               "1a45dfa39f428681014282886d6174726f736b61")
        + QByteArray(64, '\0');
}

QByteArray tinyAsfHeader()
{
    return QByteArray::fromHex("3026b2758e66cf11a6d900aa0062ce6c")
        + QByteArray(64, '\0');
}

QByteArray unknownBrandMp4Header()
{
    return QByteArray::fromHex(
               "000000186674797061766331000000006176633169736f6d")
        + QByteArray(64, '\0');
}

QByteArray tinyMp4Header()
{
    return QByteArray::fromHex(
        "000000206674797069736f6d0000020069736f6d69736f32617663316d703431");
}

QByteArray tinyJpeg()
{
    QImage image(2, 1, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 80);
    return bytes;
}

const QByteArray kGpsMarker(24, '\xAB');

QByteArray jpegWithGps()
{
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 85);

    QByteArray t("MM\0*\0\0\0\x08", 8);
    t.append("\0\2", 2);
    t.append("\x01\x12\0\3\0\0\0\1\0\1\0\0", 12);
    t.append("\x88\x25\0\4\0\0\0\1\0\0\0\x26", 12);
    t.append("\0\0\0\0", 4);
    t.append("\0\1", 2);
    t.append("\0\2\0\5\0\0\0\3\0\0\0\x38", 12);
    t.append("\0\0\0\0", 4);
    t.append(kGpsMarker);
    QByteArray payload = QByteArray("Exif\0\0", 6) + t;
    QByteArray segment("\xFF\xE1", 2);
    const quint16 len = quint16(payload.size() + 2);
    segment.append(char(len >> 8));
    segment.append(char(len & 0xFF));
    segment.append(payload);
    jpeg.insert(2, segment);
    return jpeg;
}

// A 64x32 stored JPEG, red 16x16 top-left on blue, carrying EXIF `orientation`.
QByteArray rotatedJpeg(int orientation)
{
    QImage image(64, 32, QImage::Format_RGB32);
    image.fill(Qt::blue);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            image.setPixel(x, y, qRgb(255, 0, 0));
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 95);
    QByteArray tiff("MM\0*\0\0\0\x08\0\1\x01\x12\0\3\0\0\0\1", 18);
    tiff.append(char(0));
    tiff.append(char(orientation));
    tiff.append(QByteArray(6, '\0'));
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    QByteArray segment("\xFF\xE1", 2);
    segment.append(char((payload.size() + 2) >> 8));
    segment.append(char((payload.size() + 2) & 0xFF));
    jpeg.insert(2, segment + payload);
    return jpeg;
}

QImage renderedUpright(QByteArray bytes)
{
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    return reader.read();
}

bool isRed(QRgb c) { return qRed(c) > 200 && qBlue(c) < 60; }

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A camera-sized photo: 24 MP, ~8 MiB (comment padding stands in for the
// entropy a real photo carries), with the GPS block of jpegWithGps().
QByteArray largeJpegWithGps()
{
    static const QByteArray bytes = [] {
        QImage image(6000, 4000, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPG", 85);
        const QByteArray small = jpegWithGps();
        const int gpsLength = 2 + ((uchar(small.at(4)) << 8) | uchar(small.at(5)));
        QByteArray inserted = small.mid(2, gpsLength);
        const QByteArray comment(65533, 'x');
        for (int i = 0; i < 128; ++i)
            inserted += QByteArray("\xFF\xFE\xFF\xFF", 4) + comment;
        jpeg.insert(2, inserted);
        return jpeg;
    }();
    return bytes;
}

quint32 pngCrc(const QByteArray &bytes)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : bytes) {
        crc ^= uchar(c);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

const QByteArray kPngTextMarker("Comment\0large-png-text", 22);

// A PNG made large by a 2 MiB text chunk, which "all" removes.
QByteArray largePngWithText()
{
    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(QColor(20, 120, 220, 200));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    const QByteArray body = QByteArray("tEXt") + kPngTextMarker
        + QByteArray(2 * 1024 * 1024, 'p');
    QByteArray chunk;
    const quint32 length = quint32(body.size() - 4);
    for (int shift = 24; shift >= 0; shift -= 8)
        chunk.append(char((length >> shift) & 0xFF));
    chunk += body;
    const quint32 crc = pngCrc(body);
    for (int shift = 24; shift >= 0; shift -= 8)
        chunk.append(char((crc >> shift) & 0xFF));
    png.insert(8 + 25, chunk);
    return png;
}

// A two-frame 1100x1100 GIF (~1.4 MiB).
QByteArray largeAnimatedGif()
{
    const int edge = 1100;
    QByteArray g("GIF89a");
    const auto le16 = [&g](int v) {
        g.append(char(v & 0xff));
        g.append(char((v >> 8) & 0xff));
    };
    le16(edge);
    le16(edge);
    g.append(char(0x81));
    g.append('\0');
    g.append('\0');
    for (const QColor c : {QColor(Qt::red), QColor(Qt::blue), QColor(Qt::black),
                           QColor(Qt::black)}) {
        g.append(char(c.red()));
        g.append(char(c.green()));
        g.append(char(c.blue()));
    }
    g.append(QByteArray("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19));
    for (int f = 0; f < 2; ++f) {
        g.append(QByteArray("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8));
        g.append(char(0x2c));
        le16(0);
        le16(0);
        le16(edge);
        le16(edge);
        g.append('\0');
        g.append(char(0x02));
        QByteArray data;
        quint32 acc = 0;
        int bits = 0;
        const auto put = [&](int code) {
            acc |= quint32(code) << bits;
            bits += 3;
            while (bits >= 8) {
                data.append(char(acc & 0xff));
                acc >>= 8;
                bits -= 8;
            }
        };
        for (int p = 0; p < edge * edge; ++p) {
            if (p % 2 == 0)
                put(4);
            put(f);
        }
        put(5);
        if (bits > 0)
            data.append(char(acc & 0xff));
        for (qsizetype off = 0; off < data.size(); off += 255) {
            const QByteArray block = data.mid(off, 255);
            g.append(char(block.size()));
            g.append(block);
        }
        g.append('\0');
    }
    g.append(char(0x3b));
    return g;
}

// Holds every global pool thread so a background job cannot start until
// release().
class PoolHold
{
public:
    PoolHold()
        : m_threads(QThreadPool::globalInstance()->maxThreadCount())
    {
        for (int i = 0; i < m_threads; ++i)
            QThreadPool::globalInstance()->start([this] {
                m_started.release();
                m_gate.acquire();
            });
        m_held = m_started.tryAcquire(m_threads, 10000);
    }
    ~PoolHold() { release(); }
    bool held() const { return m_held; }
    // Lets the queued jobs run, waits for them, then delivers their results.
    void release()
    {
        if (m_released)
            return;
        m_released = true;
        m_gate.release(m_threads);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
    }

private:
    int m_threads;
    QSemaphore m_started;
    QSemaphore m_gate;
    bool m_held = false;
    bool m_released = false;
};

void settlePool()
{
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
}

// Stripped copies left under a media scratch root (one subdirectory each).
int strippedCopies(const QString &root)
{
    int count = 0;
    const QStringList owners = QDir(root).entryList(
        {QStringLiteral("lightning-stripped-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &owner : owners)
        count += QDir(QDir(root).filePath(owner))
                     .entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)
                     .filter(QRegularExpression(QStringLiteral("^[0-9]")))
                     .size();
    return count;
}

void logGuiWork(const char *category)
{
    qInfo("gui-thread work %s: calls=%d bytes=%lld", category,
          stalltrace::guiWorkCalls(category),
          static_cast<long long>(stalltrace::guiWorkBytes(category)));
}

#ifdef Q_OS_UNIX
// Writes past `bytes` fail with EFBIG, as on a full disk.
class FileSizeLimit
{
public:
    explicit FileSizeLimit(rlim_t bytes)
    {
        std::signal(SIGXFSZ, SIG_IGN);
        m_ok = ::getrlimit(RLIMIT_FSIZE, &m_old) == 0;
        rlimit limited = m_old;
        limited.rlim_cur = bytes;
        m_ok = m_ok && ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
    }
    ~FileSizeLimit()
    {
        if (m_ok)
            ::setrlimit(RLIMIT_FSIZE, &m_old);
    }
    bool ok() const { return m_ok; }

private:
    rlimit m_old{};
    bool m_ok = false;
};
#endif

QByteArray redSquareSvg()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1600\" "
        "height=\"1200\" viewBox=\"0 0 1600 1200\">"
        "<rect x=\"0\" y=\"0\" width=\"1600\" height=\"1200\" fill=\"#ff0000\"/>"
        "</svg>");
}

QByteArray svgReadingALocalFile(const QString &path)
{
    return QByteArrayLiteral(
               "<svg xmlns=\"http://www.w3.org/2000/svg\" "
               "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64\" "
               "height=\"64\"><image width=\"64\" height=\"64\" xlink:href=\"")
        + QUrl::fromLocalFile(path).toEncoded()
        + QByteArrayLiteral("\"/></svg>");
}

}

class AttachmentQueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void archiveContainsOnlyReviewedSnapshots()
    {
        QTemporaryDir dir;
        QTemporaryDir outside;
        const QString keep = writeFile(dir, QStringLiteral("keep.txt"), "reviewed bytes");
        writeFile(dir, QStringLiteral("omit.txt"), "not selected");
        writeFile(dir, QStringLiteral(".hidden"), "hidden");
        writeFile(dir, QStringLiteral("private.pem"), "excluded");
#ifdef Q_OS_UNIX
        const auto link = QFile::encodeName(dir.filePath(QStringLiteral("linked")));
        QVERIFY(::symlink(QFile::encodeName(outside.path()).constData(), link.constData()) == 0);
        QVERIFY(::mkfifo(QFile::encodeName(dir.filePath(QStringLiteral("pipe"))).constData(), 0600) == 0);
#endif
        FakeClient client;
        AttachmentQueueModel model;
        model.setClient(&client);
        QSignalSpy reviewed(&model, SIGNAL(folderReviewed(QVariantList,QStringList,QString)));
        QVERIFY(reviewed.isValid());
        QSignalSpy created(&model, SIGNAL(archiveCreated(QString)));
        QVERIFY(QMetaObject::invokeMethod(&model, "reviewFolder",
                    Q_ARG(QUrl, QUrl::fromLocalFile(dir.path()))));
        QTRY_COMPARE(reviewed.size(), 1);
        QVERIFY(reviewed.first()[2].toString().isEmpty());
        QCOMPARE(reviewed.first()[0].toList().size(), 2);
        const auto excluded = reviewed.first()[1].toStringList();
        QVERIFY(excluded.contains(QStringLiteral(".hidden")));
        QVERIFY(excluded.contains(QStringLiteral("private.pem")));
#ifdef Q_OS_UNIX
        QVERIFY(excluded.contains(QStringLiteral("linked")));
        QVERIFY(excluded.contains(QStringLiteral("pipe")));
#endif
        writeFile(dir, QStringLiteral("keep.txt"), "changed bytes");
        QVERIFY(QMetaObject::invokeMethod(&model, "createFolderArchive",
                    Q_ARG(QStringList, QStringList{QStringLiteral("keep.txt")})));
        QTRY_COMPARE(created.size(), 1);
        QCOMPARE(model.rowCount(), 1);
        bool ok = false;
        const auto files = lightning::logexport::readZip(model.entries().first().data, &ok);
        QVERIFY(ok);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().name, QStringLiteral("keep.txt"));
        QCOMPARE(files.first().data, QByteArray("reviewed bytes"));
        model.clearAll();
        QCOMPARE(readAll(keep), QByteArray("changed bytes"));
    }

    void archiveLimitIncludesZipHeadersAndCancelInvalidatesReview()
    {
        QTemporaryDir dir;
        writeFile(dir, QStringLiteral("one"), "1");
        AttachmentQueueModel model;
        FakeClient client;
        client.serverLimit = 1;
        model.setClient(&client);
        QSignalSpy reviewed(&model, SIGNAL(folderReviewed(QVariantList,QStringList,QString)));
        QVERIFY(reviewed.isValid());
        QSignalSpy created(&model, SIGNAL(archiveCreated(QString)));
        QVERIFY(QMetaObject::invokeMethod(&model, "reviewFolder",
                    Q_ARG(QUrl, QUrl::fromLocalFile(dir.path()))));
        QTRY_COMPARE(reviewed.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&model, "createFolderArchive",
                    Q_ARG(QStringList, QStringList{QStringLiteral("one")})));
        QTRY_COMPARE(created.size(), 1);
        QVERIFY(!created.first().first().toString().isEmpty());
        QCOMPARE(model.rowCount(), 0);
        client.serverLimit = 1024;
        QVERIFY(QMetaObject::invokeMethod(&model, "reviewFolder",
                    Q_ARG(QUrl, QUrl::fromLocalFile(dir.path()))));
        QTRY_COMPARE(reviewed.size(), 2);
        QVERIFY(QMetaObject::invokeMethod(&model, "createFolderArchive",
                    Q_ARG(QStringList, QStringList{QStringLiteral("one")})));
        QVERIFY(QMetaObject::invokeMethod(&model, "cancelFolderReview"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(created.size(), 1);
        QCOMPARE(model.rowCount(), 0);
    }


    void inspectedPhotoSendsTheReportedExifBytes_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("location") << QStringLiteral("location");
        QTest::newRow("all") << QStringLiteral("all");
        QTest::newRow("keep") << QStringLiteral("keep");
    }

    void inspectedPhotoSendsTheReportedExifBytes()
    {
        QFETCH(QString, mode);
        const QByteArray timestamp("2020:01:02 03:04:05", 20);
        QByteArray tiff = QByteArray::fromHex(
            "4d4d002a000000080004"
            "011200030000000100060000"
            "88250004000000010000003e"
            "013200020000001400000068"
            "011000020000000443616d00"
            "00000000"
            "000100020005000000030000005000000000");
        tiff += kGpsMarker;
        tiff += timestamp;
        QByteArray segment = QByteArray::fromHex("ffe1");
        const quint16 length = tiff.size() + 8;
        segment += char(length >> 8);
        segment += char(length);
        segment += QByteArray("Exif\0\0", 6) + tiff;
        QByteArray original = rotatedJpeg(6);
        original.insert(2, segment);

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(mode);
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("camera.jpg"), original);
        composer.addAttachment(QUrl::fromLocalFile(path));
        QSignalSpy inspected(composer.attachments(), SIGNAL(photoInspected(quint64,QVariantMap)));
        QVERIFY(inspected.isValid());
        quint64 request = 0;
        QVERIFY(QMetaObject::invokeMethod(composer.attachments(), "inspectPhoto",
                    Q_RETURN_ARG(quint64, request), Q_ARG(int, 0), Q_ARG(QString, mode)));
        QVERIFY(request);
        QTRY_COMPARE(inspected.size(), 1);
        const auto summary = inspected.first()[1].toMap();
        const bool keepGps = mode == QStringLiteral("keep");
        const bool keepDevice = mode != QStringLiteral("all");
        QCOMPARE(summary.value("location").toString(), keepGps ? QString("Retained") : QString("Removed"));
        QCOMPARE(summary.value("timestamp").toString(), keepDevice ? QString("Retained") : QString("Removed"));
        QCOMPARE(summary.value("device").toString(), summary.value("timestamp").toString());
        QCOMPARE(summary.value("orientation").toString(), QStringLiteral("Retained"));

        writeFile(dir, QStringLiteral("camera.jpg"), tinyPng());
        composer.send();
        QCOMPARE(client.fileSends, 1);
        const auto sent = readAll(client.lastFilePath);
        QCOMPARE(sent.contains(kGpsMarker), keepGps);
        QCOMPARE(sent.contains(timestamp), keepDevice);
        QCOMPARE(sent.contains(QByteArray("Cam\0", 4)), keepDevice);
        QCOMPARE(imagemeta::exifOrientation(sent), 6);
        QCOMPARE(sent, imagemeta::strip(original, imagemeta::modeFromString(mode)).bytes);
    }

    void imageEditUsesSessionScratchWriter()
    {
        const QString sourcePath = QFileInfo(QString::fromUtf8(__FILE__)).dir()
            .absoluteFilePath(QStringLiteral("../src/models/AttachmentQueueModel.cpp"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray code = source.readAll();
        QVERIFY(code.contains("lightning::scratch::ScratchFile file(path)"));
        QVERIFY(!code.contains("QSaveFile file(path)"));
    }

    void strippedImagesLiveUnderTheMediaScratchRootAndOnlyCrashLeftoversAreSwept()
    {
        QTemporaryDir root;
        QTemporaryDir source;
        QVERIFY(root.isValid() && source.isValid());
        lightning::portable::setMediaScratchRootOverrideForTest(root.path());
        const auto restore = qScopeGuard(
            [] { lightning::portable::setMediaScratchRootOverrideForTest({}); });
        const QString original =
            writeFile(source, QStringLiteral("holiday.jpg"), jpegWithGps());

        QString sub;
        {
            ImageSendPrep prep;
            QString path = original;
            QByteArray data;
            QVERIFY(prep.prepare(QStringLiteral("all"), QStringLiteral("image/jpeg"),
                                 path, data, &sub));
            QVERIFY(path != original);
            QVERIFY(!sub.isEmpty());
            const QString canonicalRoot = QDir(root.path()).canonicalPath();
            QVERIFY2(QFileInfo(path).canonicalFilePath().startsWith(canonicalRoot + QLatin1Char('/')),
                     qPrintable(path));
            QVERIFY(!readAll(path).contains(kGpsMarker));
            const QStringList dirs =
                QDir(root.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            QCOMPARE(dirs.size(), 1);
            QVERIFY(dirs.at(0).startsWith(QStringLiteral("lightning-stripped-")));

            // A live instance's directory survives the startup sweep.
            QCOMPARE(lightning::portable::cleanStaleTempDirs(
                         QDateTime::currentDateTimeUtc().addDays(2)), 0);
            QVERIFY(QFileInfo::exists(path));
        }
        QCOMPARE(QDir(root.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
                 QStringList());

        // A crashed run's directory (no live holder) is swept.
        const QString crashed =
            QDir(root.path()).filePath(QStringLiteral("lightning-stripped-crash1"));
        QVERIFY(QDir().mkpath(crashed + QStringLiteral("/1")));
        QCOMPARE(lightning::portable::cleanStaleTempDirs(
                     QDateTime::currentDateTimeUtc().addDays(2)), 1);
        QVERIFY(!QFileInfo::exists(crashed));
        QCOMPARE(readAll(original), jpegWithGps());
    }

    void editedImagePreviewIsExactlyTheSentFileAndOriginalSurvives()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QByteArray original = jpegWithGps();
        const QString path = writeFile(dir, QStringLiteral("holiday.jpg"), original);
        composer.addAttachment(QUrl::fromLocalFile(path));
        auto *queue = composer.attachments();
        const QVariantList stroke{QVariantMap{{QStringLiteral("x"), 0.1},
                                             {QStringLiteral("y"), 0.1}},
                                  QVariantMap{{QStringLiteral("x"), 0.9},
                                             {QStringLiteral("y"), 0.9}}};
        const QVariantMap preview = queue->renderImageEdit(
            0, 0.0, 0.0, 0.5, 1.0, 1, true, QVariantList{stroke});
        QVERIFY(preview.contains(QStringLiteral("url")));
        const QString previewPath = preview.value(QStringLiteral("url"))
                                        .toUrl().toLocalFile();
        const QByteArray previewBytes = readAll(previewPath);
        QVERIFY(previewBytes.startsWith("\xFF\xD8\xFF"));
        QVERIFY(!previewBytes.contains("Exif"));
        QVERIFY(!previewBytes.contains(kGpsMarker));
        QImage edited;
        QVERIFY(edited.loadFromData(previewBytes));
        QCOMPARE(edited.size(), QSize(8, 16));
        QVERIFY(queue->commitImageEdit(0));
        QCOMPARE(queue->entries().at(0).mime, QStringLiteral("image/jpeg"));
        QCOMPARE(queue->entries().at(0).fileName, QStringLiteral("holiday.jpg"));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastMime, QStringLiteral("image/jpeg"));
        QCOMPARE(client.lastFileWidth, edited.width());
        QCOMPARE(client.lastFileHeight, edited.height());
        QCOMPARE(readAll(client.lastFilePath), previewBytes);
        QCOMPARE(readAll(path), original);
    }

    void cancellingImageEditKeepsTheOriginalQueued()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("photo.png"), decodablePng());
        composer.addAttachment(QUrl::fromLocalFile(path));
        auto *queue = composer.attachments();
        const QVariantMap preview = queue->renderImageEdit(
            0, 0, 0, 1, 1, 0, false, {});
        const QString previewPath = preview.value(QStringLiteral("url"))
                                        .toUrl().toLocalFile();
        QVERIFY(QFileInfo::exists(previewPath));
        queue->cancelImageEdit(0);
        QVERIFY(!QFileInfo::exists(previewPath));
        QCOMPARE(queue->entries().at(0).localPath, path);
        QCOMPARE(readAll(path), decodablePng());
    }

    void cropAfterRotationUsesDisplayedEdges()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QImage source(4, 4, QImage::Format_ARGB32);
        source.fill(Qt::blue);
        for (int y = 0; y < 4; ++y)
            source.setPixelColor(3, y, Qt::red);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("edges.png"), bytes)));
        auto *queue = composer.attachments();
        const QVariantMap preview = queue->renderImageEdit(
            0, 0, 0, 1, 0.5, 1, false, {});
        QImage result(preview.value(QStringLiteral("url")).toUrl().toLocalFile());
        QCOMPARE(result.size(), QSize(4, 2));
        for (int y = 0; y < result.height(); ++y)
            for (int x = 0; x < result.width(); ++x)
                QCOMPARE(result.pixelColor(x, y), QColor(Qt::blue));
    }

    void editedPngDropsEmbeddedTextButKeepsTransparency()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QImage source(8, 8, QImage::Format_ARGB32);
        source.fill(Qt::transparent);
        source.setPixelColor(3, 3, Qt::red);
        source.setText(QStringLiteral("Location"),
                       QStringLiteral("private location marker"));
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));
        buffer.close();
        QVERIFY(encoded.contains("private location marker"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), encoded)));
        auto *queue = composer.attachments();
        const QVariantMap preview = queue->renderImageEdit(
            0, 0, 0, 1, 1, 0, false, {});
        const QByteArray sent = readAll(preview.value(QStringLiteral("url"))
                                            .toUrl().toLocalFile());
        QVERIFY(!sent.contains("private location marker"));
        QImage decoded;
        QVERIFY(decoded.loadFromData(sent));
        QCOMPARE(decoded.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(decoded.pixelColor(3, 3), QColor(Qt::red));
    }

    void editedImageKeepsThreadCaptionAndReplyRouting()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setThreadTargetRootId(QStringLiteral("$root:example.org"));
        composer.setSendTextAsCaption(true);
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), decodablePng())));
        auto *queue = composer.attachments();
        QVERIFY(queue->renderImageEdit(0, 0, 0, 1, 1, 1, false, {})
                    .contains(QStringLiteral("url")));
        QVERIFY(queue->commitImageEdit(0));
        composer.beginReply(QStringLiteral("$reply:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString(), QStringLiteral("$root:example.org"));
        composer.setText(QStringLiteral("edited"));
        composer.send();
        QCOMPARE(client.threadFileSends, 1);
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.lastThreadRoot, QStringLiteral("$root:example.org"));
        QCOMPARE(client.lastReplyTo, QStringLiteral("$reply:example.org"));
        QCOMPARE(client.lastCaption, QStringLiteral("edited"));
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
    }

    void rejectsDirectoriesEmptyAndMissing()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        composer.addAttachment(QUrl::fromLocalFile(dir.path()));
        QCOMPARE(rejected.count(), 1);

        const QString empty = writeFile(dir, QStringLiteral("empty.bin"), {});
        composer.addAttachment(QUrl::fromLocalFile(empty));
        QCOMPARE(rejected.count(), 2);

        composer.addAttachment(
            QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.bin"))));
        QCOMPARE(rejected.count(), 3);

        composer.addAttachment(QUrl(QStringLiteral("https://example.org/x")));
        QCOMPARE(rejected.count(), 4);

        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void enforcesServerUploadLimit()
    {
        FakeClient client;
        client.serverLimit = 8;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString big =
            writeFile(dir, QStringLiteral("big.bin"), QByteArray(64, 'x'));
        composer.addAttachment(QUrl::fromLocalFile(big));
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void unknownServerLimitDoesNotRejectLocally()
    {
        FakeClient client;
        client.serverLimit = 0;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("huge.bin")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(101ll * 1024 * 1024));
        f.close();

        composer.addAttachment(QUrl::fromLocalFile(f.fileName()));
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    void exactlyAtServerLimitIsAllowed()
    {
        FakeClient client;
        client.serverLimit = 64;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString exact =
            writeFile(dir, QStringLiteral("exact.bin"), QByteArray(64, 'x'));
        composer.addAttachment(QUrl::fromLocalFile(exact));
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);

        const QString over =
            writeFile(dir, QStringLiteral("over.bin"), QByteArray(65, 'x'));
        composer.addAttachment(QUrl::fromLocalFile(over));
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    void voiceMessageIsPreflightedAgainstServerLimit()
    {
        FakeClient client;
        client.serverLimit = 16;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("voice.ogg"), QByteArray(64, 'v'));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 1200,
                                  QVariantList{});
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(client.voiceSends, 0);
        QVERIFY(!QFile::exists(rec));
    }

    void lateVoiceFailureDoesNotBleedIntoAnotherRoom()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!a:example.org"));

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("voice.ogg"), QByteArray(8, 'v'));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 900,
                                  QVariantList{});
        QCOMPARE(client.voiceSends, 1);
        const quint64 op = client.lastOpId;

        composer.setRoomId(QStringLiteral("!b:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        Q_EMIT client.attachmentQueueFinished(op, QStringLiteral("!a:example.org"),
                                              false, QString());
        QCOMPARE(rejected.count(), 0);
        QVERIFY(!QFile::exists(rec));
    }

    void voiceFailureInCurrentRoomIsStillReported()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!a:example.org"));

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("voice.ogg"), QByteArray(8, 'v'));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 900,
                                  QVariantList{});
        const quint64 op = client.lastOpId;

        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        Q_EMIT client.attachmentQueueFinished(op, QStringLiteral("!a:example.org"),
                                              false, QString());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!QFile::exists(rec));
    }

    void detectsMimeFromContentNotExtension()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString disguised =
            writeFile(dir, QStringLiteral("actually-a-png.txt"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(disguised));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        const QModelIndex idx = composer.attachments()->index(0, 0);
        QCOMPARE(composer.attachments()
                     ->data(idx, AttachmentQueueModel::MimeRole).toString(),
                 QStringLiteral("image/png"));
        QVERIFY(composer.attachments()
                    ->data(idx, AttachmentQueueModel::IsImageRole).toBool());
    }

    void chipNameLosesBidiControls()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString path = writeFile(
            dir, QStringLiteral("r\u202Etxt.exe\u202C.txt"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::FileNameRole).toString(),
                 QStringLiteral("rtxt.exe.txt"));
    }

    void duplicatePathIsRejected()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(rejected.count(), 1);
    }

    void sendDispatchesThenRemovesOnSuccess()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("photo.png"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QVERIFY(composer.canSend());

        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(composer.attachments()->rowCount(), 1);

        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void anAttachmentSentWhileReplyingCarriesTheReplyAndSpendsIt()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        QVERIFY(composer.isReplying());

        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
        QVERIFY(!composer.isReplying());
        QVERIFY(composer.replyingToEventId().isEmpty());
    }

    void anAttachmentSentWithoutAReplyIsPlain()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        client.lastReplyTo = QStringLiteral("stale");
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QVERIFY(client.lastReplyTo.isEmpty());
    }

    void theMetadataModeDefaultsToLocationAndNeverFallsToKeep()
    {
        MessageComposer composer;
        QCOMPARE(composer.imageMetadataMode(), QStringLiteral("location"));
        composer.setImageMetadataMode(QStringLiteral("keep"));
        QCOMPARE(composer.imageMetadataMode(), QStringLiteral("keep"));
        composer.setImageMetadataMode(QStringLiteral("from-the-future"));
        QCOMPARE(composer.imageMetadataMode(), QStringLiteral("location"));
    }

    void aPickedPhotoIsSentWithoutItsLocationAndTheOriginalIsUntouched()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QByteArray original = jpegWithGps();
        const QString path =
            writeFile(dir, QStringLiteral("holiday.jpg"), original);
        QVERIFY(original.contains(kGpsMarker));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QVERIFY(client.lastFilePath != path);
        QCOMPARE(QFileInfo(client.lastFilePath).fileName(),
                 QStringLiteral("holiday.jpg"));
        const QByteArray sent = readAll(client.lastFilePath);
        QVERIFY(!sent.isEmpty());
        QVERIFY(!sent.contains(kGpsMarker));
        QVERIFY(!imagemeta::containsLocation(sent));
        QImage image;
        QVERIFY(image.loadFromData(sent));
        QCOMPARE(readAll(path), original);

        const QString copy = client.lastFilePath;
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true,
            QString());
        QVERIFY(!QFileInfo::exists(copy));
    }

    void keepSendsTheOriginalFileUntouched()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(QStringLiteral("keep"));
        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("holiday.jpg"), jpegWithGps());
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastFilePath, path);
    }

    void removeAllAlsoDropsTheRestOfTheExif()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(QStringLiteral("all"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("holiday.jpg"), jpegWithGps())));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        const QByteArray sent = readAll(client.lastFilePath);
        QVERIFY(!sent.contains("Exif"));
    }

    void aPastedPhotoIsStrippedToo()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        const QByteArray original = jpegWithGps();
        QVERIFY(composer.attachments()->addImageData(
                    original, QStringLiteral("image/jpeg"), 16, 16)
                    .isEmpty());
        composer.send();
        QCOMPARE(client.byteSends, 1);
        QVERIFY(!client.lastBytes.isEmpty());
        QVERIFY(!client.lastBytes.contains(kGpsMarker));
        QVERIFY(client.lastBytes.size() <= original.size());
    }

    void aPhotoWhoseMetadataCannotBeRemovedIsNotSent()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        QByteArray broken = jpegWithGps();
        broken.chop(20);
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("broken.jpg"), broken)));
        composer.send();
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(composer.attachments()->entries().size(), 1);
        QCOMPARE(composer.attachments()->entries().first().state,
                 QStringLiteral("failed"));
        composer.setImageMetadataMode(QStringLiteral("keep"));
        composer.attachments()->retryAt(0);
        composer.send();
        QCOMPARE(client.fileSends, 1);
    }

    void nonImagesAreNotTouched()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString doc =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(doc));
        composer.send();
        QCOMPARE(client.lastFilePath, doc);
    }

    void aCaptionedAttachmentKeepsItsCaptionAndItsReply()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setSendTextAsCaption(true);
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.setText(QStringLiteral("look at this"));

        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastCaption, QStringLiteral("look at this"));
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
    }

    void sendLaterNeverCaptionsARowAnEarlierSendAlreadyTook()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setSendTextAsCaption(true);
        QTemporaryDir dir;
        for (const char *name : { "a.png", "b.png" })
            composer.addAttachment(QUrl::fromLocalFile(
                writeFile(dir, QLatin1String(name), tinyPng())));
        composer.send();
        // a.png is out; b.png waits behind it as part of that earlier Send.
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(composer.attachments()->rowCount(), 2);
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("c.png"), tinyPng())));

        const QVariantMap prepared = composer.prepareAttachmentsForSchedule(
            QStringLiteral("!room:example.org"), QStringLiteral("later"), {});
        const QVariantList media = prepared.value(QStringLiteral("media")).toList();
        QCOMPARE(media.size(), 1);
        QCOMPARE(client.scheduledCaptions, QStringList{ QStringLiteral("later") });
        QVERIFY(prepared.value(QStringLiteral("captioned")).toBool());

        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(client.fileSends, 2);
        QCOMPARE(client.lastCaption, QString());
    }

    void anInlineThreadAttachmentTakesTheTextAsItsCaption()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setSendTextAsCaption(true);
        composer.setThreadTargetRootId(QStringLiteral("$root:example.org"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.setText(QStringLiteral("look at this"));

        composer.send();
        QCOMPARE(client.threadFileSends, 1);
        QCOMPARE(client.lastThreadRoot, QStringLiteral("$root:example.org"));
        QCOMPARE(client.lastCaption, QStringLiteral("look at this"));
        QCOMPARE(composer.text(), QString());
    }

    void aPastedImageInAReplyOrThreadIsOneCaptionedEvent()
    {
        for (const QString &root : { QString(), QStringLiteral("$root:example.org") }) {
            FakeClient client;
            client.captionedBytes = true;
            MessageComposer composer;
            composer.setClient(&client);
            composer.setRoomId(QStringLiteral("!room:example.org"));
            composer.setSendTextAsCaption(true);
            composer.setThreadTargetRootId(root);
            QVERIFY(composer.attachments()->addImageData(
                        tinyPng(), QStringLiteral("image/png"), 1, 1).isEmpty());
            composer.beginReply(QStringLiteral("$target:example.org"),
                                QStringLiteral("Alice"), QStringLiteral("hello"),
                                QString(), root);
            composer.setText(QStringLiteral("look at this"));
            composer.send();
            QCOMPARE(client.captionedByteSends, 1);
            QCOMPARE(client.byteSends, 0);
            QCOMPARE(client.lastCaptionedRoot, root);
            QCOMPARE(client.lastCaption, QStringLiteral("look at this"));
            QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
            QCOMPARE(composer.text(), QString());
        }
    }

    // A backend without the captioned bytes send keeps the text as its own
    // reply rather than losing either.
    void aPastedImageInAReplyKeepsItsTextSeparate()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setSendTextAsCaption(true);
        QVERIFY(composer.attachments()->addImageData(
                    tinyPng(), QStringLiteral("image/png"), 1, 1).isEmpty());
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.setText(QStringLiteral("look at this"));
        composer.send();
        QCOMPARE(client.byteSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
    }

    void aVideoThatWaitsForItsPosterStillCarriesTheReply()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header())));
        QVERIFY(!capturedTag.isEmpty());
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());

        composer.send();
        QCOMPARE(client.videoSends, 0);
        QVERIFY(!composer.isReplying());

        composer.attachments()->applyPoster(capturedTag, tinyJpeg(),
                                            QSize(320, 180), QSize(1920, 1080),
                                            4200);
        QCOMPARE(client.videoSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
    }

    void aClipboardImageAndAVoiceMessageCarryTheReplyToo()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QVERIFY(composer.attachments()->addImageData(
                    tinyPng(), QStringLiteral("image/png"), 1, 1).isEmpty());
        composer.beginReply(QStringLiteral("$bytes:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.send();
        QCOMPARE(client.byteSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$bytes:example.org"));

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("rec.ogg"), QByteArray(32, 'v'));
        composer.beginReply(QStringLiteral("$voice:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.setText(QStringLiteral("draft stays"));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 1200,
                                  QVariantList{ 10, 20, 30 });
        QCOMPARE(client.voiceSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$voice:example.org"));
        QVERIFY(!composer.isReplying());
        QCOMPARE(composer.text(), QStringLiteral("draft stays"));
    }

    void aClipboardImageTakesTheTextAsItsCaption()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setSendTextAsCaption(true);
        QVERIFY(composer.attachments()->addImageData(
                    tinyPng(), QStringLiteral("image/png"), 1, 1).isEmpty());
        composer.setText(QStringLiteral("look at this"));
        composer.send();
        QCOMPARE(client.roomBytesSends, 1);
        QCOMPARE(client.byteSends, 0);
        QCOMPARE(client.lastCaption, QStringLiteral("look at this"));
    }

    void failedDispatchIsRetryable()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.fileSends, 1);

        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), false,
            QStringLiteral("rejected"));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        const QModelIndex idx = composer.attachments()->index(0, 0);
        QCOMPARE(composer.attachments()
                     ->data(idx, AttachmentQueueModel::StateRole).toString(),
                 QStringLiteral("failed"));

        composer.attachments()->retryAt(0);
        composer.send();
        QCOMPARE(client.fileSends, 2);
    }

    void audioSendCarriesItsDecodedDuration()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &path) {
                QVERIFY(!path.isEmpty());
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("song.mp3"),
                                       QByteArray("ID3\x03\x00\x00\x00", 7)
                                           + QByteArray(64, '\x00'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QVERIFY2(!capturedTag.isEmpty(),
                 "an audio attachment was never decoded, so nothing could "
                 "have learned its duration");

        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 185000);
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastDurationMs, 185000);
    }

    void audioThatCannotBeDecodedStillSends()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("broken.ogg"),
                                       QByteArray("not really ogg"));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 0);
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastDurationMs, 0);
    }

    void videoSendCarriesExtractedPoster()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &path) {
                QVERIFY(!path.isEmpty());
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::MimeRole)
                     .toString(),
                 QStringLiteral("video/mp4"));
        QVERIFY(!capturedTag.isEmpty());

        composer.send();
        QCOMPARE(client.videoSends, 0);
        QCOMPARE(client.fileSends, 0);

        const QByteArray poster = tinyJpeg();
        QVERIFY(!poster.isEmpty());
        composer.attachments()->applyPoster(capturedTag, poster, QSize(320, 180),
                                            QSize(1920, 1080), 4200);

        QCOMPARE(client.videoSends, 1);
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.lastMime, QStringLiteral("video/mp4"));
        QCOMPARE(client.lastThumbnail, poster);
        QCOMPARE(client.lastThumbWidth, 320);
        QCOMPARE(client.lastThumbHeight, 180);
        QCOMPARE(client.lastVideoWidth, 1920);
        QCOMPARE(client.lastVideoHeight, 1080);
        QCOMPARE(client.lastDurationMs, 4200);
        QCOMPARE(client.lastThumbWidth * client.lastVideoHeight,
                 client.lastThumbHeight * client.lastVideoWidth);
    }

    void removingAnAttachmentCancelsItsPosterJobAndParkingDoesNot()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QStringList requested;
        QStringList cancelled;
        model->setPosterRequestHook(
            [&requested](const QString &tag, const QString &) { requested.append(tag); });
        model->setPosterCancelHook(
            [&cancelled](const QString &tag) { cancelled.append(tag); });

        QTemporaryDir dir;
        for (int i = 0; i < 5; ++i)
            composer.addAttachment(QUrl::fromLocalFile(writeFile(
                dir, QStringLiteral("clip%1.mp4").arg(i), tinyMp4Header())));
        QCOMPARE(model->rowCount(), 5);
        QCOMPARE(requested.size(), 5);
        const QStringList tags = requested;

        model->applyPoster(tags.at(4), {}, {}, {}, 1000);
        model->removeAt(4);
        model->removeAt(0);
        model->removeAt(0);
        QCOMPARE(cancelled, (QStringList{tags.at(0), tags.at(1)}));

        const QList<AttachmentQueueModel::Entry> taken = model->takeAll();
        QCOMPARE(taken.size(), 2);
        QCOMPARE(cancelled.size(), 2);
        model->restoreEntries(taken);
        QCOMPARE(cancelled.size(), 2);
        QCOMPARE(requested.mid(5), (QStringList{tags.at(2), tags.at(3)}));

        composer.discardAllAttachments();
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(cancelled, (QStringList{tags.at(0), tags.at(1), tags.at(2), tags.at(3)}));
    }

    void videoSendsWithoutPosterWhenExtractionFails()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("broken.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.videoSends, 0);

        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 0);

        QCOMPARE(client.videoSends, 1);
        QVERIFY(client.lastThumbnail.isEmpty());
        QCOMPARE(client.lastThumbWidth, 0);
        QCOMPARE(client.lastThumbHeight, 0);
        QCOMPARE(client.lastVideoWidth, 0);
        QCOMPARE(client.lastVideoHeight, 0);
        QCOMPARE(client.lastDurationMs, 0);
    }

    void posterWithoutSendRequestDoesNotDispatch()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));

        composer.attachments()->applyPoster(capturedTag, tinyJpeg(),
                                            QSize(64, 36), QSize(640, 360), 1000);
        QCOMPARE(client.videoSends, 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.send();
        QCOMPARE(client.videoSends, 1);

        composer.attachments()->applyPoster(capturedTag, tinyJpeg(),
                                            QSize(64, 36), QSize(640, 360), 1000);
        QCOMPARE(client.videoSends, 1);
    }

    void posterForRemovedEntryIsIgnored()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.attachments()->setPosterRequestHook(
            [](const QString &, const QString &) {});

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.attachments()->removeAt(0);
        QCOMPARE(composer.attachments()->rowCount(), 0);

        composer.attachments()->applyPoster(QStringLiteral("send:1"), tinyJpeg(),
                                            QSize(64, 36), QSize(640, 360), 1000);
        QCOMPARE(client.videoSends, 0);
    }

    void nonVideoAttachmentsNeverRequestAPoster()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        int posterRequests = 0;
        composer.attachments()->setPosterRequestHook(
            [&posterRequests](const QString &, const QString &) {
                ++posterRequests;
            });

        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'))));
        QCOMPARE(posterRequests, 0);

        composer.send();
        QCOMPARE(posterRequests, 0);
        QCOMPARE(client.fileSends, 1);
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(client.fileSends, 2);
        QCOMPARE(client.videoSends, 0);
    }

    void staleCompletionIsIgnored()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();

        Q_EMIT client.attachmentQueueFinished(
            9999, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    void roomSwitchAndLogoutClearQueue()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!a:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.setRoomId(QStringLiteral("!b:example.org"));
        QCOMPARE(composer.attachments()->rowCount(), 0);

        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        client.logout();
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void pastedImageDataQueuesWithoutTempFile()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        const QString reason = composer.attachments()->addImageData(
            tinyPng(), QStringLiteral("image/png"), 1, 1);
        QVERIFY(reason.isEmpty());
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.send();
        QCOMPARE(client.byteSends, 1);
        QCOMPARE(client.lastFilename, QStringLiteral("pasted-image.png"));
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
    }

    void init() { lightning::svgraster::hooks() = {}; }
    void cleanup() { lightning::svgraster::hooks() = {}; }

    void svgScreenRefusesAnythingThatReachesOutsideTheDocument()
    {
        using lightning::svgraster::screen;
        const QByteArray head =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"10\" "
            "height=\"10\">";
        const QByteArray tail = "</svg>";
        const auto wrap = [&](const QByteArray &body) {
            return head + body + tail;
        };

        const QList<std::pair<QByteArray, QString>> refused = {
            { wrap("<image href=\"/home/user/private.png\"/>"),
              QStringLiteral("external_image") },
            { wrap("<image xlink:href=\"data:image/png;base64,AAAA\"/>"),
              QStringLiteral("external_image") },
            { wrap("<s:image xmlns:s=\"http://www.w3.org/2000/svg\" "
                   "href=\"private.png\"/>"),
              QStringLiteral("external_image") },
            { wrap("<filter id=\"f\"><feImage href=\"private.png\"/></filter>"),
              QStringLiteral("external_image") },
            { wrap("<use xlink:href=\"other.svg#a\"/>"),
              QStringLiteral("external_reference") },
            { wrap("<font-face><font-face-src><font-face-uri "
                   "xlink:href=\"font.svg\"/></font-face-src></font-face>"),
              QStringLiteral("external_reference") },
            { wrap("<rect fill=\"url(pattern.png)\"/>"),
              QStringLiteral("external_reference") },
            { wrap("<rect style=\"fill: url( 'file:///x.png' )\"/>"),
              QStringLiteral("external_reference") },
            { wrap("<style>@import 'x.css';</style>"),
              QStringLiteral("external_reference") },
            { wrap("<style><![CDATA[rect { fill: url(x.png) }]]></style>"),
              QStringLiteral("external_reference") },
            { wrap("<style>rect {}<x/>@import 'x.css';</style>"),
              QStringLiteral("external_reference") },
            { QByteArray("<!DOCTYPE svg [<!ENTITY e \"") + QByteArray(4000, 'a')
                  + QByteArray("\">]>")
                  + wrap("<desc t=\"" + QByteArray("&e;").repeated(1000)
                         + "\"/>"),
              QStringLiteral("entities") },
            { QByteArray("<?xml version=\"1.0\"?><!DOCTYPE svg [<!ENTITY e "
                         "SYSTEM \"file:///etc/passwd\">]>")
                  + wrap("<text>&e;</text>"),
              QStringLiteral("entities") },
            { QByteArray("<?xml-stylesheet href=\"x.css\"?>") + wrap(""),
              QStringLiteral("processing_instruction") },
            { QByteArray("\x1f\x8b", 2) + QByteArray(30, '\x08'),
              QStringLiteral("compressed") },
            { QByteArray("<html><body/></html>"), QStringLiteral("not_svg") },
            { wrap("<rect>"), QStringLiteral("malformed") },
            { QByteArray(), QStringLiteral("empty") },
            { QByteArray(lightning::svgraster::kMaxSourceBytes + 1, ' '),
              QStringLiteral("too_large") },
            { wrap(QByteArray("<g>").repeated(64) + QByteArray("</g>").repeated(64)),
              QStringLiteral("too_deep") },
            { wrap(QByteArray("<rect/>").repeated(lightning::svgraster::kMaxElements)),
              QStringLiteral("too_many_elements") },
        };
        for (const auto &[bytes, reason] : refused)
            QCOMPARE(screen(bytes), reason);

        const QList<QByteArray> accepted = {
            redSquareSvg(),
            wrap("<defs><linearGradient id=\"g\"/></defs>"
                 "<rect fill=\"url(#g)\"/><use xlink:href=\"#g\"/>"),
            wrap("<a xlink:href=\"https://example.org/\"><rect/></a>"),
            QByteArray("<!DOCTYPE svg [<!ENTITY w \"10\">]>")
                + wrap("<rect width=\"&w;\" height=\"&w;\"/>"),
        };
        for (const QByteArray &bytes : accepted)
            QCOMPARE(screen(bytes), QString());
    }

    void aPickedSvgIsUploadedAsAPngNeverAsSvg()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();

        QTemporaryDir dir;
        const QString svgPath =
            writeFile(dir, QStringLiteral("logo.svg"), redSquareSvg());
        composer.addAttachment(QUrl::fromLocalFile(svgPath));
        QCOMPARE(model->rowCount(), 1);
        QVERIFY(model->data(model->index(0, 0),
                            AttachmentQueueModel::PreviewSourceRole)
                    .toString().isEmpty());
        composer.send();
        QCOMPARE(client.fileSends, 0);

        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 10000);
        QCOMPARE(client.imageThumbSends, 0);
        QCOMPARE(client.videoSends, 0);
        QCOMPARE(client.byteSends, 0);
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
        QVERIFY(client.lastFilePath.endsWith(QStringLiteral("/logo.png")));
        QVERIFY(client.lastFilePath != svgPath);

        QFile file(client.lastFilePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QCOMPARE(lightning::imagefmt::sniffRasterMime(bytes),
                 QStringLiteral("image/png"));
        const QImage decoded = QImage::fromData(bytes, "PNG");
        QCOMPARE(decoded.size(), QSize(1600, 1200));
        QCOMPARE(client.lastFileWidth, 1600);
        QCOMPARE(client.lastFileHeight, 1200);
        QCOMPARE(decoded.pixelColor(800, 600), QColor(Qt::red));

        QCOMPARE(model->data(model->index(0, 0), AttachmentQueueModel::FileNameRole)
                     .toString(), QStringLiteral("logo.png"));
        QCOMPARE(model->data(model->index(0, 0), AttachmentQueueModel::MimeRole)
                     .toString(), QStringLiteral("image/png"));
        QCOMPARE(model->data(model->index(0, 0), AttachmentQueueModel::SizeBytesRole)
                     .toLongLong(), qint64(bytes.size()));
    }

    void aTinySvgIsConvertedAtTheAttachmentFloor()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("icon.svg"),
            QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" "
                              "viewBox=\"0 0 24 12\"><rect width=\"24\" "
                              "height=\"12\"/></svg>"))));
        composer.send();
        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 10000);
        QCOMPARE(client.lastFileWidth, 1024);
        QCOMPARE(client.lastFileHeight, 512);
        QCOMPARE(QImage(client.lastFilePath).size(), QSize(1024, 512));
    }

    void aRefusedSvgFailsWithItsReasonAndSendsNothing()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();

        QTemporaryDir dir;
        const QString secret =
            writeFile(dir, QStringLiteral("private.png"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("trap.svg"), svgReadingALocalFile(secret))));
        QCOMPARE(model->rowCount(), 1);
        composer.send();
        QTRY_COMPARE_WITH_TIMEOUT(
            model->data(model->index(0, 0), AttachmentQueueModel::StateRole)
                .toString(),
            QStringLiteral("failed"), 10000);
        const QString error =
            model->data(model->index(0, 0), AttachmentQueueModel::ErrorRole)
                .toString();
        QVERIFY2(error.contains(QStringLiteral("links to other files")),
                 qPrintable(error));
        QVERIFY(!model->data(model->index(0, 0),
                             AttachmentQueueModel::RetryableRole).toBool());
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.imageThumbSends, 0);
        QCOMPARE(client.byteSends, 0);
    }

    void aConvertedSvgPastTheUploadLimitFails()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        client.serverLimit = 200;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("big.svg"),
            QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" "
                              "viewBox=\"0 0 8 8\"><circle cx=\"4\" cy=\"4\" "
                              "r=\"3\"/></svg>"))));
        QCOMPARE(model->rowCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            model->data(model->index(0, 0), AttachmentQueueModel::StateRole)
                .toString(),
            QStringLiteral("failed"), 10000);
        QVERIFY(model->data(model->index(0, 0), AttachmentQueueModel::ErrorRole)
                    .toString().contains(QStringLiteral("upload limit")));
    }

    void theSameSvgCannotBeQueuedTwice()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QTemporaryDir dir;
        const QUrl url = QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("logo.svg"), redSquareSvg()));
        QVERIFY(model->addFile(url).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!model->entries().at(0).posterPending, 10000);
        QVERIFY(!model->entries().at(0).isSvg);
        QVERIFY(!model->addFile(url).isEmpty());
        QCOMPARE(model->rowCount(), 1);
    }

    void removingAnEntryDeletesItsConvertedPngButNeverTheSource()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QTemporaryDir dir;
        const QString svgPath =
            writeFile(dir, QStringLiteral("logo.svg"), redSquareSvg());
        QVERIFY(model->addFile(QUrl::fromLocalFile(svgPath)).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!model->entries().at(0).posterPending, 10000);
        const QString png = model->entries().at(0).localPath;
        QVERIFY(QFileInfo::exists(png));
        QVERIFY(png != svgPath);
        model->removeAt(0);
        QVERIFY(!QFileInfo::exists(png));
        QVERIFY(QFileInfo::exists(svgPath));
    }

    void receivedSvgStaysRefused()
    {
        const QByteArray svg = redSquareSvg();
        QVERIFY(lightning::imagefmt::sniffRasterMime(svg).isEmpty());
        QVERIFY(lightning::imagefmt::sniffRasterMime(
                    QByteArray("<?xml version=\"1.0\"?>") + svg).isEmpty());
        if (lightning::svgraster::available()) {
            const auto result = lightning::svgraster::rasterize(
                svg, lightning::svgraster::attachmentPolicy());
            QCOMPARE(lightning::imagefmt::sniffRasterMime(result.png),
                     QStringLiteral("image/png"));
        }
    }

    void detectMimeNarrowsAGenericContainerByItsExtension()
    {
        QTemporaryDir dir;
        struct Case {
            const char *name;
            QByteArray bytes;
            const char *expected;
        };
        const Case cases[] = {
            {"recording.mkv", tinyMatroskaHeader(), "video/x-matroska"},
            {"RECORDING.MKV", tinyMatroskaHeader(), "video/x-matroska"},
            {"album.mka", tinyMatroskaHeader(), "audio/x-matroska"},
            {"clip.wmv", tinyAsfHeader(), "video/x-ms-wmv"},
            {"song.wma", tinyAsfHeader(), "audio/x-ms-wma"},
            {"picture.mkv", tinyPng(), "image/png"},
            {"clip.mp4", tinyMp4Header(), "video/mp4"},
            {"camera.mp4", unknownBrandMp4Header(), "video/mp4"},
            {"no-extension", tinyMatroskaHeader(), "application/x-matroska"},
        };
        for (const Case &c : cases) {
            const QString path =
                writeFile(dir, QString::fromLatin1(c.name), c.bytes);
            QVERIFY(!path.isEmpty());
            QCOMPARE(AttachmentQueueModel::detectMime(path),
                     QString::fromLatin1(c.expected));
        }
        const QString playlist = writeFile(
            dir, QStringLiteral("list.vlc"),
            QByteArrayLiteral("song one.mp3\nsong two.mp3\n"));
        QVERIFY(!AttachmentQueueModel::detectMime(playlist)
                     .startsWith(QLatin1String("audio/")));
        const QString archive = writeFile(
            dir, QStringLiteral("bundle.yt"),
            QByteArrayLiteral("PK\x03\x04") + QByteArray(64, '\0'));
        QCOMPARE(AttachmentQueueModel::detectMime(archive),
                 QStringLiteral("application/zip"));
    }

    void aPickedMatroskaVideoIsSentAsAVideo()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("obs-recording.mkv"),
                                       tinyMatroskaHeader());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QVERIFY2(!capturedTag.isEmpty(),
                 "a Matroska video was never postered: it was not classified "
                 "as a video");
        composer.send();
        const QByteArray poster = tinyJpeg();
        composer.attachments()->applyPoster(capturedTag, poster, QSize(320, 180),
                                            QSize(1920, 1080), 9000);
        QCOMPARE(client.videoSends, 1);
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.lastMime, QStringLiteral("video/x-matroska"));
        QCOMPARE(client.lastThumbnail, poster);
        QCOMPARE(client.lastDurationMs, 9000);
    }

    void aPickedMatroskaOrWmaAudioIsSentAsAudio()
    {
        for (const auto &[name, bytes, mime] :
             {std::tuple{"album.mka", tinyMatroskaHeader(), "audio/x-matroska"},
              std::tuple{"song.wma", tinyAsfHeader(), "audio/x-ms-wma"}}) {
            FakeClient client;
            MessageComposer composer;
            composer.setClient(&client);
            composer.setRoomId(QStringLiteral("!room:example.org"));
            QString capturedTag;
            composer.attachments()->setPosterRequestHook(
                [&capturedTag](const QString &tag, const QString &) {
                    capturedTag = tag;
                });
            QTemporaryDir dir;
            const QString path =
                writeFile(dir, QString::fromLatin1(name), bytes);
            composer.addAttachment(QUrl::fromLocalFile(path));
            QVERIFY2(!capturedTag.isEmpty(),
                     "audio was never decoded for its duration: it was not "
                     "classified as audio");
            composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 61000);
            composer.send();
            QCOMPARE(client.fileSends, 1);
            QCOMPARE(client.videoSends, 0);
            QCOMPARE(client.lastMime, QString::fromLatin1(mime));
            QCOMPARE(client.lastDurationMs, 61000);
        }
    }

    void aLocalhostFileUrlIsTheLocalPath()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("a b#1.bin"),
                                       QByteArray(16, 'a'));
        QUrl localhost = QUrl::fromLocalFile(path);
        localhost.setHost(QStringLiteral("LocalHost"));
        QVERIFY(!QFileInfo(localhost.toLocalFile()).isReadable());
        QCOMPARE(AttachmentQueueModel::normalizeDroppedUrl(localhost)
                     .toLocalFile(),
                 path);
        const QUrl sloppy = QUrl::fromEncoded(
            QUrl::fromLocalFile(dir.path()).toEncoded()
            + QByteArrayLiteral("/a%20b#1.bin"));
        QCOMPARE(sloppy.fragment(), QStringLiteral("1.bin"));
        QCOMPARE(AttachmentQueueModel::normalizeDroppedUrl(sloppy)
                     .toLocalFile(),
                 path);
        const QUrl web(QStringLiteral("https://example.org/a#b"));
        QCOMPARE(AttachmentQueueModel::normalizeDroppedUrl(web), web);

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        composer.addDroppedAttachments({localhost}, QString());
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    void thePortalKeyIsReadWithOrWithoutItsNul()
    {
        QCOMPARE(AttachmentQueueModel::portalKeyFrom(
                     QByteArray("12345\0", 6)),
                 QStringLiteral("12345"));
        QCOMPARE(AttachmentQueueModel::portalKeyFrom("12345"),
                 QStringLiteral("12345"));
        QCOMPARE(AttachmentQueueModel::portalKeyFrom({}), QString());
        QMimeData mime;
        QCOMPARE(AttachmentQueueModel::portalKeyFromMimeData(&mime), QString());
        mime.setData(QStringLiteral("application/vnd.portal.files"),
                     QByteArray("legacy\0", 7));
        QCOMPARE(AttachmentQueueModel::portalKeyFromMimeData(&mime),
                 QStringLiteral("legacy"));
        mime.setData(QStringLiteral("application/vnd.portal.filetransfer"),
                     "current");
        QCOMPARE(AttachmentQueueModel::portalKeyFromMimeData(&mime),
                 QStringLiteral("current"));
    }

    void documentPortalUrlsAreClassifiedWithoutHostPathGuessing()
    {
#ifdef Q_OS_LINUX
        const QUrl exported(QStringLiteral(
            "file:///run/user/1000/doc/abcdef/photo%20one.png"));
        QVERIFY(AttachmentQueueModel::isDocumentPortalUrl(exported));
        AttachmentQueueModel model;
        QList<QUrl> resolved;
        AttachmentQueueModel::resolveDroppedUrls(
            {exported}, {}, &model,
            [&](const QList<QUrl> &urls) { resolved = urls; });
        QCOMPARE(resolved, (QList<QUrl>{exported}));
        QVERIFY(!AttachmentQueueModel::isDocumentPortalUrl(QUrl(
            QStringLiteral("file:///run/user/1000/doc/../private/photo.png"))));
#endif
        QVERIFY(!AttachmentQueueModel::isDocumentPortalUrl(QUrl(
            QStringLiteral("file:///home/user/photo.png"))));
        QVERIFY(!AttachmentQueueModel::isDocumentPortalUrl(QUrl(
            QStringLiteral("https://example.org/run/user/1000/doc/abc"))));
    }

    void aSandboxedDropIsResolvedThroughThePortal()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
        });
        QTemporaryDir dir;
        const QString exported = writeFile(dir, QStringLiteral("clip.bin"),
                                           QByteArray(32, 'v'));
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("not-in-the-sandbox/clip.bin")));
        QStringList askedKeys;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &key,
                std::function<void(bool, const QStringList &)> done) {
                askedKeys.append(key);
                done(true, {exported});
            });

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        composer.addDroppedAttachments({hostPath}, QStringLiteral("key-1"));
        QCOMPARE(askedKeys, QStringList{QStringLiteral("key-1")});
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::LocalUrlRole)
                     .toUrl()
                     .toLocalFile(),
                 exported);

        composer.attachments()->clearAll();
        const QString readable = writeFile(dir, QStringLiteral("here.bin"),
                                           QByteArray(8, 'h'));
        composer.addDroppedAttachments({QUrl::fromLocalFile(readable)},
                                       QStringLiteral("key-2"));
        QCOMPARE(askedKeys, (QStringList{QStringLiteral("key-1"),
                                        QStringLiteral("key-2")}));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::LocalUrlRole)
                     .toUrl().toLocalFile(), exported);

        composer.attachments()->clearAll();
        composer.addDroppedAttachments({QUrl::fromLocalFile(readable)}, {});
        QCOMPARE(askedKeys.size(), 2);
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::LocalUrlRole)
                     .toUrl().toLocalFile(), readable);
    }

    void aPortalFailureOrALateAnswerIsHandled()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
            AttachmentQueueModel::setSandboxKindForTesting({});
        });
        AttachmentQueueModel::setSandboxKindForTesting(QStringLiteral("flatpak"));
        std::function<void(bool, const QStringList &)> pending;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &,
                std::function<void(bool, const QStringList &)> done) {
                pending = std::move(done);
            });
        QTemporaryDir dir;
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("elsewhere/clip.bin")));

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        composer.addDroppedAttachments({hostPath}, QStringLiteral("k"));
        QVERIFY(pending);
        pending(false, {});
        QCOMPARE(rejected.count(), 1);
        QVERIFY2(rejected.at(0).at(0).toString().contains(
                     QLatin1String("sandbox")),
                 qPrintable(rejected.at(0).at(0).toString()));
        QCOMPARE(composer.attachments()->rowCount(), 0);

        pending = nullptr;
        composer.addDroppedAttachments({hostPath}, QString());
        QVERIFY(!pending);
        QCOMPARE(rejected.count(), 2);

        const QString exported = writeFile(dir, QStringLiteral("late.bin"),
                                           QByteArray(8, 'l'));
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k2"));
        QVERIFY(pending);
        composer.setRoomId(QStringLiteral("!other:example.org"));
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 0);
        QCOMPARE(rejected.count(), 2);
    }

    void aLatePortalAnswerNeverCrossesIntoAnotherAccount()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
        });
        std::function<void(bool, const QStringList &)> pending;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &,
                std::function<void(bool, const QStringList &)> done) {
                pending = std::move(done);
            });
        QTemporaryDir dir;
        const QString exported = writeFile(dir, QStringLiteral("late.bin"),
                                           QByteArray(8, 'l'));
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("elsewhere/clip.bin")));
        const QString room = QStringLiteral("!room:example.org");

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(room);

        composer.addDroppedAttachments({hostPath}, QStringLiteral("k1"));
        QVERIFY(pending);
        client.userId = QStringLiteral("@someone-else:example.org");
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 0);

        pending = nullptr;
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k2"));
        QVERIFY(pending);
        client.logout();
        composer.setRoomId(room);
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 0);

        QSignalSpy resolved(&composer,
                            &MessageComposer::droppedAttachmentsResolved);
        pending = nullptr;
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k3"));
        QVERIFY(pending);
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(resolved.count(), 1);
    }

    void aLargePhotoIsStrippedOffTheGuiThreadAndKeepsTheTrayOrder()
    {
        QTemporaryDir root;
        QTemporaryDir dir;
        QVERIFY(root.isValid() && dir.isValid());
        lightning::portable::setMediaScratchRootOverrideForTest(root.path());
        const auto restore = qScopeGuard(
            [] { lightning::portable::setMediaScratchRootOverrideForTest({}); });
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        const QByteArray original = largeJpegWithGps();
        QVERIFY(original.size() > 8 * 1024 * 1024);
        const QString big = writeFile(dir, QStringLiteral("camera.jpg"), original);
        const QString small =
            writeFile(dir, QStringLiteral("small.jpg"), jpegWithGps());
        composer.addAttachment(QUrl::fromLocalFile(big));
        composer.addAttachment(QUrl::fromLocalFile(small));

        stalltrace::resetGuiWork();
        composer.send();
        logGuiWork("image-send-prep");
        QCOMPARE(stalltrace::guiWorkCalls("image-send-prep"), 0);
        QCOMPARE(stalltrace::guiWorkBytes("image-send-prep"), qint64(0));
        // The small photo waits behind the large one.
        QCOMPARE(client.fileSends, 0);
        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 30000);
        QCOMPARE(stalltrace::guiWorkCalls("image-send-prep"), 0);
        QCOMPARE(QFileInfo(client.lastFilePath).fileName(),
                 QStringLiteral("camera.jpg"));
        QVERIFY(client.lastFilePath != big);
        const QByteArray sent = readAll(client.lastFilePath);
        QVERIFY(!sent.contains(kGpsMarker));
        QVERIFY(!imagemeta::containsLocation(sent));
        QCOMPARE(readAll(big), original);

        const QString copy = client.lastFilePath;
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(client.fileSends, 2);
        QCOMPARE(QFileInfo(client.lastFilePath).fileName(),
                 QStringLiteral("small.jpg"));
        // Below the threshold the strip stays inline.
        QCOMPARE(stalltrace::guiWorkCalls("image-send-prep"), 1);
    }

    void aLargePastedPngAndALargeAnimatedGifAreReadOffTheGuiThread()
    {
        QTemporaryDir root;
        QTemporaryDir dir;
        QVERIFY(root.isValid() && dir.isValid());
        lightning::portable::setMediaScratchRootOverrideForTest(root.path());
        const auto restore = qScopeGuard(
            [] { lightning::portable::setMediaScratchRootOverrideForTest({}); });
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(QStringLiteral("all"));

        const QByteArray png = largePngWithText();
        QVERIFY(png.size() > 2 * 1024 * 1024);
        QVERIFY(composer.attachments()
                    ->addImageData(png, QStringLiteral("image/png"), 64, 48)
                    .isEmpty());
        stalltrace::resetGuiWork();
        composer.send();
        logGuiWork("image-send-prep");
        QCOMPARE(stalltrace::guiWorkBytes("image-send-prep"), qint64(0));
        QTRY_COMPARE_WITH_TIMEOUT(client.byteSends, 1, 30000);
        QVERIFY(!client.lastBytes.contains(kPngTextMarker));
        QVERIFY(client.lastBytes.size() < 64 * 1024);
        QImage decoded;
        QVERIFY(decoded.loadFromData(client.lastBytes));
        QCOMPARE(decoded.size(), QSize(64, 48));
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});

        // A format with no editable metadata goes out as it is.
        const QString gif =
            writeFile(dir, QStringLiteral("loop.gif"), largeAnimatedGif());
        QVERIFY(QFileInfo(gif).size() > ImageSendPrep::kBackgroundBytes);
        composer.addAttachment(QUrl::fromLocalFile(gif));
        stalltrace::resetGuiWork();
        composer.send();
        logGuiWork("image-send-prep");
        QCOMPARE(stalltrace::guiWorkBytes("image-send-prep"), qint64(0));
        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 30000);
        QCOMPARE(client.lastFilePath, gif);
        QCOMPARE(strippedCopies(root.path()), 0);
    }

    void aStripThatOutlivesItsRowIsDeletedAndNeverSent()
    {
        QTemporaryDir root;
        QTemporaryDir dir;
        QVERIFY(root.isValid() && dir.isValid());
        lightning::portable::setMediaScratchRootOverrideForTest(root.path());
        const auto restore = qScopeGuard(
            [] { lightning::portable::setMediaScratchRootOverrideForTest({}); });
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        const QString roomA = QStringLiteral("!a:example.org");
        const QString roomB = QStringLiteral("!b:example.org");
        composer.setRoomId(roomA);
        const QString big =
            writeFile(dir, QStringLiteral("camera.jpg"), largeJpegWithGps());

        // Removed while stripping.
        composer.addAttachment(QUrl::fromLocalFile(big));
        {
            PoolHold hold;
            QVERIFY(hold.held());
            composer.send();
            QCOMPARE(composer.attachments()->rowCount(), 1);
            composer.attachments()->removeAt(0);
            QCOMPARE(composer.attachments()->rowCount(), 0);
        }
        settlePool();
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(strippedCopies(root.path()), 0);

        // The room changes while stripping: the row was sent, so it goes to
        // the room it was sent from without the user returning, ahead of the
        // row queued behind it, and nothing leaves before the strip lands.
        const QString small =
            writeFile(dir, QStringLiteral("small.jpg"), jpegWithGps());
        composer.addAttachment(QUrl::fromLocalFile(big));
        composer.addAttachment(QUrl::fromLocalFile(small));
        {
            PoolHold hold;
            QVERIFY(hold.held());
            composer.send();
            composer.setRoomId(roomB);
            QCOMPARE(composer.attachments()->rowCount(), 0);
            QCOMPARE(client.fileSends, 0);
        }
        settlePool();
        QCOMPARE(client.fileSends, 2);
        QCOMPARE(client.sent.size(), 2);
        QVERIFY(client.sent.at(0).startsWith(roomA + QLatin1Char('|')));
        QVERIFY(client.sent.at(1).startsWith(roomA + QLatin1Char('|')));
        QVERIFY(client.sent.at(0).endsWith(QStringLiteral("/camera.jpg")));
        QVERIFY(client.sent.at(1).endsWith(QStringLiteral("/small.jpg")));
        QVERIFY(!client.sent.contains(roomA + QLatin1Char('|') + big));
        QVERIFY(!client.sent.contains(roomA + QLatin1Char('|') + small));
        const QString bigCopy = client.sent.at(0).section(QLatin1Char('|'), 1);
        QVERIFY(!readAll(bigCopy).contains(kGpsMarker));
        QCOMPARE(strippedCopies(root.path()), 2);
        for (quint64 op = client.lastOpId - 1; op <= client.lastOpId; ++op)
            Q_EMIT client.attachmentQueueFinished(op, roomA, true, {});
        QCOMPARE(strippedCopies(root.path()), 0);
        composer.setRoomId(roomA);
        QCOMPARE(composer.attachments()->rowCount(), 0);
        QCOMPARE(client.fileSends, 2);
        client.sent.clear();
        client.fileSends = 0;

        // Back before the first strip ran: the restarted strip and the stale
        // one never share a copy, and the stale one never deletes the sent one.
        composer.addAttachment(QUrl::fromLocalFile(big));
        {
            PoolHold hold;
            QVERIFY(hold.held());
            composer.send();
            composer.setRoomId(roomB);
            composer.setRoomId(roomA);
        }
        settlePool();
        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 30000);
        settlePool();
        QCOMPARE(client.fileSends, 1);
        QVERIFY(QFileInfo::exists(client.lastFilePath));
        QVERIFY(!readAll(client.lastFilePath).contains(kGpsMarker));
        QCOMPARE(strippedCopies(root.path()), 1);
    }

    void aStripThatCannotWriteFailsHonestlyAndLeavesNothing()
    {
        QTemporaryDir root;
        QTemporaryDir dir;
        QVERIFY(root.isValid() && dir.isValid());
        lightning::portable::setMediaScratchRootOverrideForTest(root.path());
        const auto restore = qScopeGuard(
            [] { lightning::portable::setMediaScratchRootOverrideForTest({}); });
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        const QString big =
            writeFile(dir, QStringLiteral("camera.jpg"), largeJpegWithGps());
        const auto expectFailed = [&](const char *why) {
            QTRY_VERIFY2_WITH_TIMEOUT(
                composer.attachments()->entries().value(0).state
                    == QLatin1String("failed"),
                why, 30000);
            QVERIFY2(composer.attachments()->entries().at(0).error.contains(
                         QStringLiteral("Photo details could not be removed")),
                     why);
            QVERIFY2(client.fileSends == 0, why);
            QVERIFY2(strippedCopies(root.path()) == 0, why);
        };

#ifdef Q_OS_UNIX
        composer.addAttachment(QUrl::fromLocalFile(big));
        {
            FileSizeLimit full(64 * 1024);
            QVERIFY(full.ok());
            composer.send();
            settlePool();
        }
        expectFailed("disk full");
        if (QTest::currentTestFailed())
            return;
        composer.attachments()->removeAt(0);
#endif

        // The scratch directory vanishes while the strip is queued.
        composer.addAttachment(QUrl::fromLocalFile(big));
        {
            PoolHold hold;
            QVERIFY(hold.held());
            composer.send();
            const QStringList owners = QDir(root.path()).entryList(
                {QStringLiteral("lightning-stripped-*")}, QDir::Dirs);
            QCOMPARE(owners.size(), 1);
            QVERIFY(QDir(QDir(root.path()).filePath(owners.at(0)))
                        .removeRecursively());
        }
        expectFailed("scratch directory removed");
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(QDir(root.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot),
                 QStringList());

        // Retrying once the storage is back sends the stripped copy.
        composer.attachments()->retryAt(0);
        composer.send();
        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 30000);
        QVERIFY(!readAll(client.lastFilePath).contains(kGpsMarker));
    }

    void aLargeImageEditRendersOffTheGuiThread()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString big =
            writeFile(dir, QStringLiteral("camera.jpg"), largeJpegWithGps());
        composer.addAttachment(QUrl::fromLocalFile(big));
        auto *queue = composer.attachments();
        QSignalSpy rendered(queue, SIGNAL(imageEditRendered(int,QVariantMap)));

        stalltrace::resetGuiWork();
        const QVariantMap first = queue->renderImageEdit(
            0, 0.0, 0.0, 1.0, 1.0, 1, false, {});
        const QVariantMap second = queue->renderImageEdit(
            0, 0.0, 0.0, 0.5, 1.0, 0, false, {});
        logGuiWork("image-edit");
        QCOMPARE(stalltrace::guiWorkCalls("image-edit"), 0);
        QCOMPARE(stalltrace::guiWorkBytes("image-edit"), qint64(0));
        QVERIFY(first.value(QStringLiteral("pending")).toBool());
        QVERIFY(second.value(QStringLiteral("pending")).toBool());
        // Nothing can be committed until the latest render lands.
        QVERIFY(!queue->commitImageEdit(0));
        settlePool();
        QCOMPARE(stalltrace::guiWorkCalls("image-edit"), 0);
        // The superseded render is dropped with its file.
        QCOMPARE(rendered.count(), 1);
        QCOMPARE(rendered.at(0).at(0).toInt(),
                 second.value(QStringLiteral("request")).toInt());
        const QVariantMap result = rendered.at(0).at(1).toMap();
        const QString preview =
            result.value(QStringLiteral("url")).toUrl().toLocalFile();
        QVERIFY(QFileInfo::exists(preview));
        QCOMPARE(result.value(QStringLiteral("width")).toInt(), 2048);
        QVERIFY(result.value(QStringLiteral("height")).toInt() > 2700);
        const QDir converted = QFileInfo(preview).dir().absolutePath()
            + QStringLiteral("/..");
        QCOMPARE(converted.entryList({QStringLiteral("edit-*")}, QDir::Dirs).size(), 1);
        QVERIFY(!readAll(preview).contains(kGpsMarker));

        // Cancelled while rendering: the late file is deleted.
        QVERIFY(queue->renderImageEdit(0, 0.1, 0.1, 0.9, 0.9, 0, true, {})
                    .value(QStringLiteral("pending")).toBool());
        queue->cancelImageEdit(0);
        settlePool();
        QCOMPARE(rendered.count(), 1);
        QCOMPARE(converted.entryList({QStringLiteral("edit-*")}, QDir::Dirs).size(), 0);

        // An animated GIF renders off the thread too and says it was animated.
        const QString gif =
            writeFile(dir, QStringLiteral("loop.gif"), largeAnimatedGif());
        composer.addAttachment(QUrl::fromLocalFile(gif));
        QVERIFY(queue->renderImageEdit(1, 0.0, 0.0, 1.0, 1.0, 0, false, {})
                    .value(QStringLiteral("pending")).toBool());
        settlePool();
        QCOMPARE(rendered.count(), 2);
        QCOMPARE(rendered.at(1).at(1).toMap().value(QStringLiteral("animated")).toBool(),
                 true);
        QVERIFY(queue->commitImageEdit(1));
        QCOMPARE(stalltrace::guiWorkCalls("image-edit"), 0);
    }

    void anEditThatCannotBeSavedFailsHonestlyAndLeavesNothing()
    {
#ifndef Q_OS_UNIX
        QSKIP("needs RLIMIT_FSIZE");
#else
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("small.png"), decodablePng())));
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("camera.jpg"), largeJpegWithGps())));
        auto *queue = composer.attachments();
        const QVariantMap small = queue->renderImageEdit(0, 0, 0, 1, 1, 0, false, {});
        const QString smallPreview =
            small.value(QStringLiteral("url")).toUrl().toLocalFile();
        QVERIFY(QFileInfo::exists(smallPreview));
        const QDir converted = QFileInfo(smallPreview).dir().absolutePath()
            + QStringLiteral("/..");
        queue->cancelImageEdit(0);
        QCOMPARE(converted.entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList());

        QSignalSpy rendered(queue, SIGNAL(imageEditRendered(int,QVariantMap)));
        {
            FileSizeLimit full(64 * 1024);
            QVERIFY(full.ok());
            QVERIFY(queue->renderImageEdit(1, 0, 0, 1, 1, 0, false, {})
                        .value(QStringLiteral("pending")).toBool());
            settlePool();
        }
        QCOMPARE(rendered.count(), 1);
        QCOMPARE(rendered.at(0).at(1).toMap().value(QStringLiteral("error")).toString(),
                 QStringLiteral("The image edit could not be saved."));
        QCOMPARE(converted.entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList());
        QVERIFY(!queue->commitImageEdit(1));
        QCOMPARE(queue->entries().at(1).state, QStringLiteral("queued"));
#endif
    }
    void aRotatedPhotoIsSentWithItsDisplayedSize_data()
    {
        QTest::addColumn<int>("orientation");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QSize>("displayed");
        QTest::addColumn<QPoint>("redAt");
        for (const QString &mode : {QStringLiteral("location"),
                                    QStringLiteral("all"),
                                    QStringLiteral("keep")}) {
            QTest::addRow("180-%s", qPrintable(mode))
                << 3 << mode << QSize(64, 32) << QPoint(56, 24);
            QTest::addRow("90cw-%s", qPrintable(mode))
                << 6 << mode << QSize(32, 64) << QPoint(24, 8);
            QTest::addRow("90ccw-%s", qPrintable(mode))
                << 8 << mode << QSize(32, 64) << QPoint(8, 56);
        }
    }

    void aRotatedPhotoIsSentWithItsDisplayedSize()
    {
        QFETCH(int, orientation);
        QFETCH(QString, mode);
        QFETCH(QSize, displayed);
        QFETCH(QPoint, redAt);
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(mode);
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("phone.jpg"), rotatedJpeg(orientation))));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(QSize(client.lastFileWidth, client.lastFileHeight), displayed);
        const QImage shown = renderedUpright(readAll(client.lastFilePath));
        QCOMPARE(shown.size(), displayed);
        QVERIFY(isRed(shown.pixel(redAt)));
        QVERIFY(!isRed(shown.pixel(displayed.width() - 1 - redAt.x(),
                                   displayed.height() - 1 - redAt.y())));
    }

    void aRotatedClipboardJpegIsPastedUpright_data()
    {
        QTest::addColumn<int>("orientation");
        QTest::addColumn<QSize>("displayed");
        QTest::addColumn<QPoint>("redAt");
        QTest::newRow("180") << 3 << QSize(64, 32) << QPoint(56, 24);
        QTest::newRow("90cw") << 6 << QSize(32, 64) << QPoint(24, 8);
        QTest::newRow("90ccw") << 8 << QSize(32, 64) << QPoint(8, 56);
    }

    void aRotatedClipboardJpegIsPastedUpright()
    {
        QFETCH(int, orientation);
        QFETCH(QSize, displayed);
        QFETCH(QPoint, redAt);
        QMimeData mime;
        mime.setData(QStringLiteral("image/jpeg"), rotatedJpeg(orientation));
        const QImage pasted = AttachmentQueueModel::clipboardImage(&mime);
        QCOMPARE(pasted.size(), displayed);
        QVERIFY(isRed(pasted.pixel(redAt)));
        QVERIFY(!isRed(pasted.pixel(displayed.width() - 1 - redAt.x(),
                                    displayed.height() - 1 - redAt.y())));
    }

    void aDecodedClipboardImageIsPastedAsIs()
    {
        QImage image(5, 3, QImage::Format_RGB32);
        image.fill(Qt::green);
        QMimeData mime;
        mime.setImageData(image);
        QCOMPARE(AttachmentQueueModel::clipboardImage(&mime).size(), QSize(5, 3));
        const QMimeData empty;
        QVERIFY(AttachmentQueueModel::clipboardImage(&empty).isNull());
    }
};

QTEST_GUILESS_MAIN(AttachmentQueueTest)
#include "AttachmentQueueTest.moc"
