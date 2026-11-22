#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

class ImageSendPrep
{
public:
    ImageSendPrep();
    ~ImageSendPrep();

    // Inputs at least this large are stripped on a worker thread: one frame at
    // 60 Hz on ~60 MB/s storage (USB 2, SD card, network share).
    static constexpr qint64 kBackgroundBytes = 1024 * 1024;

    // What to send: `path`/`data` start as the inputs and are replaced when a
    // stripped copy was made; `sub` is the directory holding it.
    struct Prepared {
        bool ok = true;
        QString path;
        QByteArray data;
        QString sub;
    };
    static bool needsStrip(const QString &mode, const QString &mime);
    // Reads, strips and (for a file) writes `sub/<name>`; touches no member,
    // so it may run on any thread. A cancel or a failure leaves no file.
    static Prepared run(const QString &mode, const QString &path,
                        const QByteArray &data, const QString &sub,
                        const std::atomic<bool> *cancelled = nullptr);
    // A fresh, not yet created directory under this instance's live scratch
    // directory; empty when that directory cannot be made.
    QString nextSub();

    // `mode` is the stored setting string. `path` or `data` is replaced by what
    // must be sent; `*tempSub` receives the directory to hand to track() (empty
    // when nothing was written). False = the image must NOT be sent (removal
    // was requested and could not be done).
    bool prepare(const QString &mode, const QString &mime, QString &path,
                 QByteArray &data, QString *tempSub);
    void track(quint64 opId, const QString &sub);
    void finished(quint64 opId);
    static void discard(const QString &sub);
    void releaseAll();

private:
    struct Tracked {
        QString sub;
        QDateTime at;
    };
    void purgeOld();

    std::unique_ptr<QTemporaryDir> m_dir;
    QHash<quint64, Tracked> m_ops;
    quint64 m_next = 1;
};
