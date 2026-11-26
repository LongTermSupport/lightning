#include "threads/ForumView.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QVariantMap>

#include <algorithm>

namespace forum {
namespace {

constexpr int kMaxTitle = 120;
constexpr int kMaxTags = 5;
constexpr int kMaxAllTags = 20;

QVariantMap decorate(QVariantMap entry)
{
    const QString text = entry.value(QStringLiteral("rootPreview")).toString();
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
        lines.removeFirst();
    QString title = lines.isEmpty() ? QString() : lines.takeFirst().trimmed();
    if (title.size() > kMaxTitle)
        title = title.left(kMaxTitle - 1) + QChar(0x2026);
    entry.insert(QStringLiteral("title"), title);
    entry.insert(QStringLiteral("preview"), lines.join(QLatin1Char(' ')).simplified());
    entry.insert(QStringLiteral("tags"), tagsIn(text));
    entry.insert(QStringLiteral("unanswered"),
                 entry.value(QStringLiteral("replyCount")).toInt() == 0);
    return entry;
}

qint64 ms(const QVariantMap &entry, const char *key)
{
    return entry.value(QLatin1String(key)).toDateTime().toMSecsSinceEpoch();
}

} // namespace

QStringList tagsIn(const QString &text)
{
    // Not inside a word or URL fragment, and not a room alias (#room:server).
    static const QRegularExpression re(
        QStringLiteral("(?<![\\w/#&])#([\\p{L}\\p{N}_-]{1,32})(?![\\p{L}\\p{N}_:-])"),
        QRegularExpression::UseUnicodePropertiesOption);
    QStringList out;
    auto it = re.globalMatch(text);
    while (it.hasNext() && out.size() < kMaxTags) {
        const QString tag = it.next().captured(1).toLower();
        if (!out.contains(tag))
            out.append(tag);
    }
    return out;
}

QVariantList arrange(const QVariantList &threads, const QString &sort,
                     const QString &filter, const QString &tag)
{
    QList<QVariantMap> rows;
    rows.reserve(threads.size());
    for (const QVariant &value : threads) {
        const QVariantMap entry = decorate(value.toMap());
        if (filter == QLatin1String("unread")
            && !entry.value(QStringLiteral("unread")).toBool())
            continue;
        if (filter == QLatin1String("unanswered")
            && !entry.value(QStringLiteral("unanswered")).toBool())
            continue;
        if (!tag.isEmpty()
            && !entry.value(QStringLiteral("tags")).toStringList().contains(tag))
            continue;
        rows.append(entry);
    }
    if (sort == QLatin1String("newest")) {
        std::stable_sort(rows.begin(), rows.end(), [](const QVariantMap &a, const QVariantMap &b) {
            return ms(a, "rootTimestamp") > ms(b, "rootTimestamp");
        });
    } else if (sort == QLatin1String("replies")) {
        std::stable_sort(rows.begin(), rows.end(), [](const QVariantMap &a, const QVariantMap &b) {
            return a.value(QStringLiteral("replyCount")).toInt()
                > b.value(QStringLiteral("replyCount")).toInt();
        });
    }
    QVariantList out;
    out.reserve(rows.size());
    for (const QVariantMap &row : std::as_const(rows))
        out.append(row);
    return out;
}

QStringList allTags(const QVariantList &threads)
{
    QHash<QString, int> counts;
    QStringList order;
    for (const QVariant &value : threads) {
        for (const QString &tag :
             tagsIn(value.toMap().value(QStringLiteral("rootPreview")).toString())) {
            if (!counts.contains(tag))
                order.append(tag);
            ++counts[tag];
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](const QString &a, const QString &b) {
        return counts.value(a) > counts.value(b);
    });
    return order.mid(0, kMaxAllTags);
}

} // namespace forum
