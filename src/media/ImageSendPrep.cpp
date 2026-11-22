#include "media/ImageSendPrep.h"

#include "app/GuiStallTracer.h"
#include "media/ImageMetadata.h"
#include "storage/PortableMode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {
constexpr auto kPrefix = "lightning-stripped-";
}

ImageSendPrep::ImageSendPrep() = default;

ImageSendPrep::~ImageSendPrep()
{
    // The live lock must go before the directory: Windows cannot delete a
    // lock file that is still open.
    if (m_dir)
        lightning::portable::releaseScratchDir(m_dir->path());
}

void ImageSendPrep::purgeOld()
{
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-15 * 60);
    for (auto it = m_ops.begin(); it != m_ops.end();) {
        if (it->at < cutoff) {
            QDir(it->sub).removeRecursively();
            it = m_ops.erase(it);
        } else {
            ++it;
        }
    }
}

bool ImageSendPrep::needsStrip(const QString &mode, const QString &mime)
{
    return imagemeta::modeFromString(mode) != imagemeta::Mode::Keep
        && mime.startsWith(QLatin1String("image/"));
}

ImageSendPrep::Prepared ImageSendPrep::run(const QString &modeString,
                                           const QString &path,
                                           const QByteArray &data,
                                           const QString &sub,
                                           const std::atomic<bool> *cancelled)
{
    Prepared out{true, path, data, {}};
    QByteArray bytes = data;
    if (!path.isEmpty()) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {false, path, data, {}};
        bytes = file.readAll();
    }
    qint64 work = bytes.size();
    const auto done = [&work](Prepared result) {
        stalltrace::noteGuiWork("image-send-prep", work);
        return result;
    };
    const imagemeta::Result r =
        imagemeta::strip(bytes, imagemeta::modeFromString(modeString));
    switch (r.status) {
    case imagemeta::Status::Unchanged:
    case imagemeta::Status::Unsupported:
        return done(out);
    case imagemeta::Status::Failed:
        out.ok = false;
        return done(out);
    case imagemeta::Status::Stripped:
        break;
    }
    if (path.isEmpty()) {
        out.data = r.bytes;
        return done(out);
    }
    work += r.bytes.size();
    // mkdir, not mkpath: a scratch root removed underneath must not be
    // recreated without its live lock.
    if (sub.isEmpty() || (cancelled && cancelled->load()) || !QDir().mkdir(sub))
        return done({false, path, data, {}});
    const QString target = sub + QLatin1Char('/') + QFileInfo(path).fileName();
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(r.bytes) != r.bytes.size() || !file.flush()) {
        file.close();
        QDir(sub).removeRecursively();
        return done({false, path, data, {}});
    }
    file.close();
    out.path = target;
    out.sub = sub;
    return done(out);
}

QString ImageSendPrep::nextSub()
{
    if (m_dir && m_dir->isValid() && !QFileInfo::exists(m_dir->path())) {
        lightning::portable::releaseScratchDir(m_dir->path());
        m_dir.reset();
    }
    if (!m_dir) {
        // Crash leftovers are swept at startup by cleanStaleTempDirs().
        m_dir = std::make_unique<QTemporaryDir>(
            lightning::portable::mediaScratchRoot() + QLatin1Char('/')
            + QString::fromLatin1(kPrefix) + QStringLiteral("XXXXXX"));
        if (m_dir->isValid())
            lightning::portable::holdScratchDirLive(m_dir->path());
    }
    if (!m_dir->isValid())
        return {};
    return m_dir->filePath(QString::number(m_next++));
}

bool ImageSendPrep::prepare(const QString &mode, const QString &mime,
                            QString &path, QByteArray &data, QString *tempSub)
{
    if (tempSub)
        tempSub->clear();
    if (!needsStrip(mode, mime))
        return true;
    const Prepared r =
        run(mode, path, data, path.isEmpty() ? QString() : nextSub());
    if (!r.ok)
        return false;
    path = r.path;
    data = r.data;
    if (tempSub)
        *tempSub = r.sub;
    return true;
}

void ImageSendPrep::track(quint64 opId, const QString &sub)
{
    if (sub.isEmpty())
        return;
    purgeOld();
    m_ops.insert(opId, {sub, QDateTime::currentDateTime()});
}

void ImageSendPrep::finished(quint64 opId)
{
    const auto it = m_ops.find(opId);
    if (it == m_ops.end())
        return;
    QDir(it->sub).removeRecursively();
    m_ops.erase(it);
}

void ImageSendPrep::discard(const QString &sub)
{
    if (!sub.isEmpty())
        QDir(sub).removeRecursively();
}

void ImageSendPrep::releaseAll()
{
    for (const Tracked &t : std::as_const(m_ops))
        QDir(t.sub).removeRecursively();
    m_ops.clear();
}
