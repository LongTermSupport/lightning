#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

#include <functional>

// Transactional replacement with rollback. Verify everything before touching
// anything, perform the fewest renames possible, and undo on failure: the
// target is always entirely old or entirely new, never mixed or missing.
// Nothing outside the named paths is deleted or recursed into.

namespace updater {

enum class ReplaceError {
    None = 0,
    SourceMissing,
    SourceNotAFile,
    SourceNotADirectory,
    TargetMissing,
    TargetNotAFile,
    TargetNotADirectory,
    TargetNotWritable,
    BackupPathUnusable,
    LayoutInvalid,
    PromoteFailed,
    BackupFailed,
    CopyFailed,
    RollbackFailed,
    RefusedUnsafePath,
    RestoreIncomplete,
};

const char *replaceErrorName(ReplaceError error);

struct ReplaceResult {
    ReplaceError error = ReplaceError::None;
    QString message;
    bool rolledBack = false;
    bool usedCopyFallback = false;
    QString backupPath;

    bool ok() const { return error == ReplaceError::None; }
};

struct ReplaceHooks {
    std::function<bool()> beforeBackupRename;
    std::function<bool()> beforePromoteRename;
    // Per-entry rename in swapDirectory; returning false fails that rename.
    std::function<bool(const QString &from, const QString &to)> beforeEntryRename;
    bool forceCopyFallback = false;
};

ReplaceResult replaceFileAtomically(const QString &newFile,
                                    const QString &targetPath,
                                    const QString &backupPath,
                                    const ReplaceHooks &hooks = ReplaceHooks());

ReplaceResult swapDirectory(const QString &stagedDir, const QString &targetDir,
                            const QString &backupDir,
                            const QString &expectedExecutableName,
                            const QStringList &preserveNames = QStringList(),
                            const ReplaceHooks &hooks = ReplaceHooks());

QString resolveStagedRoot(const QString &stagedDir,
                          const QString &expectedExecutableName);

bool directoryIsWritable(const QString &directory);

// Where a portable swap puts the outgoing build: a sibling of the
// installation, never inside it (swapDirectory refuses an overlapping backup).
// Sharing the parent keeps promotion a same-filesystem rename. Shared with the
// helper so tests assert the rule it actually uses.
QString portableBackupPath(const QString &targetDir);

constexpr auto kPortableBackupSuffix = ".lightning-previous";
// A backup still holding files a failed rollback could not put back is moved
// to "<target><suffix><time>", a name no swap ever clears.
constexpr auto kUnrestoredBackupSuffix = ".lightning-unrestored-";
// Written into such a backup; if it could not be moved, swapDirectory refuses
// to clear a backup that carries it.
constexpr auto kUnrestoredBackupMarker = "LIGHTNING-RESTORE-INCOMPLETE.txt";

// True when `path` is one of the recovery folders a swap leaves beside
// `targetDir`. Header-only so the app can check a path the helper reported.
inline bool isPortableRecoveryPath(const QString &targetDir, const QString &path)
{
    const QString target = QDir::cleanPath(QDir(targetDir).absolutePath());
    const QString clean = QDir::cleanPath(path);
    if (clean.compare(target + QLatin1String(kPortableBackupSuffix), Qt::CaseInsensitive) == 0)
        return true;
    const QString parked = target + QLatin1String(kUnrestoredBackupSuffix);
    return clean.size() > parked.size() && clean.startsWith(parked, Qt::CaseInsensitive)
        && !clean.mid(parked.size()).contains(QLatin1Char('/'));
}

}
