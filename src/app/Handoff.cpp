#include "app/Handoff.h"

#include "app/UrlLauncher.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace lightning::handoff {

namespace {

constexpr int kMaxTitle = 200;
constexpr int kMaxNotes = 20000;
constexpr int kMaxDurationMinutes = 7 * 24 * 60;

QString stripReplyFallback(const QString &text)
{
    if (!text.startsWith(QLatin1String("> <")))
        return text;
    const QStringList lines = text.split(QLatin1Char('\n'));
    int i = 0;
    while (i < lines.size() && lines.at(i).startsWith(QLatin1Char('>')))
        ++i;
    if (i < lines.size() && lines.at(i).trimmed().isEmpty())
        ++i;
    return lines.mid(i).join(QLatin1Char('\n'));
}

bool isPermalink(const QString &link)
{
    return (link.startsWith(QLatin1String("https://matrix.to/#/"))
            || link.startsWith(QLatin1String("matrix:")))
        && !link.contains(QRegularExpression(QStringLiteral("\\s")))
        && !link.contains(QLatin1String("access_token"), Qt::CaseInsensitive)
        && !urls::isCredentialBearingMatrixUrl(QUrl(link));
}

QString icsText(QString value)
{
    value.remove(QLatin1Char('\r'));
    value.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    value.replace(QLatin1Char(';'), QLatin1String("\\;"));
    value.replace(QLatin1Char(','), QLatin1String("\\,"));
    value.replace(QLatin1Char('\n'), QLatin1String("\\n"));
    return value;
}

// RFC 5545 3.1: at most 75 octets a line, never splitting a UTF-8 sequence.
QByteArray fold(const QString &line)
{
    const QByteArray utf8 = line.toUtf8();
    QByteArray out;
    int width = 0;
    for (int i = 0; i < utf8.size();) {
        int len = 1;
        const auto lead = static_cast<unsigned char>(utf8.at(i));
        if (lead >= 0xf0) len = 4;
        else if (lead >= 0xe0) len = 3;
        else if (lead >= 0xc0) len = 2;
        if (width + len > 75) {
            out += "\r\n ";
            width = 1;
        }
        out += utf8.mid(i, len);
        width += len;
        i += len;
    }
    return out + "\r\n";
}

QString utcStamp(const QDateTime &when)
{
    return when.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

}

QString scrubText(const QString &text)
{
    static const QRegularExpression link(
        QStringLiteral("(?:https?|mxc)://[^\\s<>\"]+"),
        QRegularExpression::CaseInsensitiveOption);
    QString in = stripReplyFallback(text);
    QString out;
    qsizetype last = 0;
    auto it = link.globalMatch(in);
    while (it.hasNext()) {
        const auto m = it.next();
        const QUrl url(m.captured());
        const bool refuse = url.scheme().compare(QLatin1String("mxc"), Qt::CaseInsensitive) == 0
            || urls::isCredentialBearingMatrixUrl(url);
        out += in.mid(last, m.capturedStart() - last);
        out += refuse ? QCoreApplication::translate("Handoff", "[link removed]")
                      : m.captured();
        last = m.capturedEnd();
    }
    out += in.mid(last);
    return out;
}

Fields fieldsFrom(const QVariantMap &map, const QTimeZone &zone)
{
    Fields f;
    f.kind = map.value(QStringLiteral("kind")).toString() == QLatin1String("event")
        ? QStringLiteral("event") : QStringLiteral("task");
    f.title = scrubText(map.value(QStringLiteral("title")).toString())
                  .simplified().left(kMaxTitle);
    f.notes = scrubText(map.value(QStringLiteral("notes")).toString()).left(kMaxNotes);
    const QString link = map.value(QStringLiteral("link")).toString().trimmed();
    if (map.value(QStringLiteral("includeLink")).toBool() && isPermalink(link))
        f.link = link;
    f.uidSeed = map.value(QStringLiteral("uidSeed")).toString();
    const QDate date = QDate::fromString(map.value(QStringLiteral("date")).toString(),
                                         QStringLiteral("yyyy-MM-dd"));
    const QTime time = QTime::fromString(map.value(QStringLiteral("time")).toString(),
                                         QStringLiteral("HH:mm"));
    if (date.isValid() && time.isValid())
        f.start = QDateTime(date, time, zone);
    f.durationMinutes = std::clamp(
        map.value(QStringLiteral("durationMinutes"), 60).toInt(), 1, kMaxDurationMinutes);
    return f;
}

QString plainText(const Fields &f)
{
    QStringList parts;
    if (!f.title.isEmpty())
        parts << f.title;
    if (f.kind == QLatin1String("event") && f.start.isValid())
        parts << QLocale().toString(f.start.toLocalTime(), QLocale::LongFormat);
    if (!f.notes.trimmed().isEmpty())
        parts << f.notes.trimmed();
    if (!f.link.isEmpty())
        parts << f.link;
    return parts.join(QStringLiteral("\n\n"));
}

QString ics(const Fields &f, const QDateTime &now)
{
    const bool event = f.kind == QLatin1String("event");
    if (event && !f.start.isValid())
        return {};
    // The same message exported again keeps its UID, so a calendar updates
    // the entry instead of adding a second one.
    const QByteArray seed = f.uidSeed.isEmpty()
        ? QUuid::createUuid().toByteArray()
        : QByteArray(f.uidSeed.toUtf8() + '\n' + f.kind.toUtf8());
    const QString uid = QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex().left(32))
        + QStringLiteral("@lightning");
    const QString component = event ? QStringLiteral("VEVENT") : QStringLiteral("VTODO");

    QStringList lines{
        QStringLiteral("BEGIN:VCALENDAR"),
        QStringLiteral("VERSION:2.0"),
        QStringLiteral("PRODID:-//Lightning//Lightning Matrix client//EN"),
        QStringLiteral("CALSCALE:GREGORIAN"),
        QStringLiteral("BEGIN:") + component,
        QStringLiteral("UID:") + uid,
        QStringLiteral("DTSTAMP:") + utcStamp(now),
        QStringLiteral("LAST-MODIFIED:") + utcStamp(now),
        QStringLiteral("SUMMARY:") + icsText(f.title),
    };
    if (event) {
        lines << QStringLiteral("DTSTART:") + utcStamp(f.start)
              << QStringLiteral("DTEND:")
                     + utcStamp(f.start.addSecs(qint64(f.durationMinutes) * 60));
    } else {
        lines << QStringLiteral("STATUS:NEEDS-ACTION");
    }
    if (!f.notes.trimmed().isEmpty())
        lines << QStringLiteral("DESCRIPTION:") + icsText(f.notes.trimmed());
    if (!f.link.isEmpty())
        lines << QStringLiteral("URL:") + f.link;
    lines << QStringLiteral("END:") + component << QStringLiteral("END:VCALENDAR");

    QByteArray out;
    for (const QString &line : lines)
        out += fold(line);
    return QString::fromUtf8(out);
}

QVariantMap inspectSharedFile(const QUrl &url)
{
    auto refuse = [](const QString &reason) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("reason"), reason}};
    };
    if (!url.isLocalFile() || !url.host().isEmpty())
        return refuse(QCoreApplication::translate(
            "Handoff", "Only files on this computer can be shared."));
    const QFileInfo info(url.toLocalFile());
    // isFile() is false for directories, FIFOs and devices, which could block
    // or never end.
    if (!info.isFile() || !info.isReadable())
        return refuse(QCoreApplication::translate("Handoff", "That file cannot be read."));
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("size"), info.size()},
            {QStringLiteral("url"), QUrl::fromLocalFile(info.absoluteFilePath())}};
}

}

using namespace lightning::handoff;

QString HandoffExporter::preview(const QVariantMap &fields, bool asFile) const
{
    const Fields f = fieldsFrom(fields, QTimeZone::systemTimeZone());
    return asFile ? ics(f, QDateTime::currentDateTimeUtc()) : plainText(f);
}

void HandoffExporter::copyText(const QVariantMap &fields) const
{
    if (auto *clipboard = QGuiApplication::clipboard())
        clipboard->setText(preview(fields, false));
}

bool HandoffExporter::saveFile(const QUrl &target, const QVariantMap &fields) const
{
    if (!target.isLocalFile())
        return false;
    const QString content = preview(fields, true);
    if (content.isEmpty())
        return false;
    const QString path = target.toLocalFile();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(content.toUtf8()) < 0 || !file.commit())
        return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}
