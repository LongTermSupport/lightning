#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>
#include <atomic>

namespace folderarchive {

struct File {
    QString name;
    QByteArray bytes;
};

struct Review {
    QString fileName;
    QList<File> files;
    QStringList omissions;
    QString error;
};

Review scan(const QString &folder, const std::atomic_bool &cancelled);

}
