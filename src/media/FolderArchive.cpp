#include "media/FolderArchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <filesystem>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace folderarchive {
namespace {

constexpr int maxEntries = 256;
constexpr int maxDepth = 8;
constexpr qint64 maxBytes = 16 * 1024 * 1024;

struct Scanner {
    const std::atomic_bool &cancelled;
    Review review;
    QSet<QString> names;
    int seen = 0;
    qint64 total = 0;

    bool include(const QString &name, const QString &relative)
    {
        static const QRegularExpression unsafe(QStringLiteral(
            R"rx([\x00-\x1f\x7f<>:"/\\|?*]|[. ]$|^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$))rx"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression sensitive(QStringLiteral(
            R"(^(id_(rsa|dsa|ecdsa|ed25519).*|credentials(\..*)?|.*\.(pem|key|p12|pfx|kdbx))$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (name.startsWith(u'.') || name.contains(QChar::ReplacementCharacter)
            || unsafe.match(name).hasMatch() || sensitive.match(name).hasMatch()
            || relative.toUtf8().size() > 1024
            || std::any_of(name.cbegin(), name.cend(), [](QChar ch) {
                return ch.category() == QChar::Other_Format;
            }))
            return false;
        const QString key = relative.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (names.contains(key))
            return false;
        names.insert(key);
        return true;
    }

    void recordEmpty(const QString &prefix, int before)
    {
        if (seen == before && !prefix.isEmpty())
            review.omissions.append(prefix);
    }

    bool next()
    {
        if (++seen > maxEntries)
            review.error = QCoreApplication::translate("FolderArchive", "This folder exceeds the 256-entry review limit.");
        return !cancelled.load() && review.error.isEmpty();
    }

    void snapshot(QFile &file, const QString &relative)
    {
        const qint64 size = file.size();
        if (size < 0 || size > maxBytes - total) {
            review.omissions.append(relative);
            return;
        }
        QByteArray bytes = file.read(size + 1);
        if (bytes.size() != size || file.error() != QFileDevice::NoError) {
            review.omissions.append(relative);
            return;
        }
        total += size;
        review.files.append({relative, bytes});
    }

#ifdef Q_OS_UNIX
    bool directory(int fd, const QString &prefix, int depth)
    {
        DIR *dir = fdopendir(fd);
        if (!dir) {
            close(fd);
            return false;
        }
        const auto cleanup = qScopeGuard([dir] { closedir(dir); });
        const int before = seen;
        while (!cancelled.load()) {
            errno = 0;
            const dirent *entry = readdir(dir);
            if (!entry) {
                if (errno != 0)
                    return false;
                recordEmpty(prefix, before);
                return true;
            }
            const QByteArray leaf(entry->d_name);
            if (leaf == "." || leaf == "..")
                continue;
            if (!next())
                return false;
            const QString name = QFile::decodeName(leaf);
            const QString relative = prefix + name;
            struct stat st{};
            if (include(name, relative)
                && fstatat(fd, leaf.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISDIR(st.st_mode) && depth < maxDepth) {
                    const int child = openat(fd, leaf.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
                    if (child >= 0 && directory(child, relative + u'/', depth + 1))
                        continue;
                } else if (S_ISREG(st.st_mode)) {
                    const int child = openat(fd, leaf.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
                    if (child >= 0) {
                        const auto closeFile = qScopeGuard([child] { close(child); });
                        struct stat opened{};
                        QFile file;
                        if (fstat(child, &opened) == 0 && S_ISREG(opened.st_mode)
                            && opened.st_dev == st.st_dev && opened.st_ino == st.st_ino
                            && file.open(child, QIODevice::ReadOnly)) {
                            snapshot(file, relative);
                            continue;
                        }
                    }
                }
            }
            review.omissions.append(relative);
            if (!review.error.isEmpty())
                return false;
        }
        return false;
    }
#elif defined(Q_OS_WIN)
    QString rootFinalPath;

    bool walk(const QString &path, const QString &relative, int depth)
    {
        // Holding each ancestor without delete-sharing pins the reviewed directory tree.
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        const auto closeHandle = qScopeGuard([&] {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        });
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info)
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
            || (!relative.isEmpty() && (info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)))
            return false;
        const DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
        if (!length || length > 32768)
            return false;
        std::wstring finalPath(length, wchar_t(0));
        const DWORD copied = GetFinalPathNameByHandleW(handle, finalPath.data(), length, FILE_NAME_NORMALIZED);
        if (!copied || copied >= length)
            return false;
        finalPath.resize(copied);
        const QString actual = QDir::fromNativeSeparators(QString::fromStdWString(finalPath));
        if (relative.isEmpty())
            rootFinalPath = actual.endsWith(u'/') ? actual : actual + u'/';
        else if (actual != rootFinalPath + relative)
            return false;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (relative.isEmpty())
                return false;
            const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
            if (fd < 0)
                return false;
            handle = INVALID_HANDLE_VALUE;
            const auto closeFile = qScopeGuard([fd] { _close(fd); });
            QFile file;
            if (!file.open(fd, QIODevice::ReadOnly))
                return false;
            snapshot(file, relative);
            return true;
        }
        if (depth > maxDepth)
            return false;
        const int before = seen;
        std::error_code ec;
        std::filesystem::directory_iterator it(path.toStdWString(), ec), end;
        while (!ec && it != end) {
            if (!next())
                return false;
            const QString name = QString::fromStdWString(it->path().filename().wstring());
            const QString child = relative.isEmpty() ? name : relative + u'/' + name;
            if (!include(name, child) || !walk(path + u'/' + name, child, depth + 1))
                review.omissions.append(child);
            if (!review.error.isEmpty())
                return false;
            it.increment(ec);
        }
        if (!ec)
            recordEmpty(relative, before);
        return !ec;
    }
#endif
};
}

Review scan(const QString &folder, const std::atomic_bool &cancelled)
{
    Scanner scanner{cancelled, {}, {}};
    const QFileInfo info(folder);
    bool ok = false;
    if (info.isAbsolute() && !info.isSymLink() && info.isDir()) {
#ifdef Q_OS_UNIX
        const int fd = open(QFile::encodeName(info.absoluteFilePath()).constData(),
                            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        ok = fd >= 0 && scanner.directory(fd, {}, 0);
#elif defined(Q_OS_WIN)
        ok = scanner.walk(info.absoluteFilePath(), {}, 0);
#endif
    }
    if (!ok && scanner.review.error.isEmpty())
        scanner.review.error = QCoreApplication::translate("FolderArchive", "The folder could not be reviewed.");
    scanner.review.fileName = info.fileName() + QStringLiteral(".zip");
    std::sort(scanner.review.files.begin(), scanner.review.files.end(),
              [](const File &a, const File &b) { return a.name < b.name; });
    scanner.review.omissions.sort();
    return scanner.review;
}
}
