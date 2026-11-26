#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

// Forum presentation of a room's thread list: titles, #tags, sort and filter.
// Pure view logic over ThreadController's list; nothing is sent or stored.
namespace forum {

QStringList tagsIn(const QString &text);
// Adds "title", "preview", "tags" and "unanswered" to every entry, then
// filters ("all", "unread", "unanswered", plus an optional tag) and sorts
// ("activity" keeps the list's own order, "newest", "replies").
QVariantList arrange(const QVariantList &threads, const QString &sort,
                     const QString &filter, const QString &tag);
// The tags used in `threads`, most used first.
QStringList allTags(const QVariantList &threads);

} // namespace forum
