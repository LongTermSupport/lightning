#include "models/UserLookup.h"

#include <QRegularExpression>
#include <QUrl>

namespace matrix::user_lookup {

QString localpartOrUserId(const QString &userId)
{
    if (!userId.startsWith(QLatin1Char('@')))
        return userId;
    const QString localpart = userId.mid(1).section(QLatin1Char(':'), 0, 0);
    return localpart.isEmpty() ? userId : localpart;
}

QString serverNameFromUserId(const QString &userId)
{
    const int colon = userId.indexOf(QLatin1Char(':'));
    if (!userId.startsWith(QLatin1Char('@')) || colon < 0)
        return {};
    return userId.mid(colon + 1);
}

bool isValidLocalpart(const QString &localpart)
{
    static const QRegularExpression re(
        QStringLiteral("^[a-zA-Z0-9._=/+\\-]+$"));
    return !localpart.isEmpty() && re.match(localpart).hasMatch();
}

bool isValidHistoricalLocalpart(const QString &localpart)
{
    if (localpart.isEmpty())
        return false;
    for (const QChar c : localpart)
        if (c.unicode() < 0x21 || c.unicode() > 0x7E || c == QLatin1Char(':'))
            return false;
    return true;
}

namespace {

bool isDecimal(QStringView text, int maxDigits)
{
    if (text.isEmpty() || text.size() > maxDigits)
        return false;
    for (const QChar c : text)
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
    return true;
}

bool isIpv4(QStringView text)
{
    const auto parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    for (const QStringView part : parts) {
        if (!isDecimal(part, 3) || (part.size() > 1 && part.front() == QLatin1Char('0'))
            || part.toInt() > 255)
            return false;
    }
    return true;
}

bool isHexGroup(QStringView group)
{
    if (group.isEmpty() || group.size() > 4)
        return false;
    for (const QChar c : group)
        if (!(c.isDigit() && c.unicode() < 128)
            && !(c >= QLatin1Char('a') && c <= QLatin1Char('f'))
            && !(c >= QLatin1Char('A') && c <= QLatin1Char('F')))
            return false;
    return true;
}

// RFC 4291 text form as Rust's Ipv6Addr parses it (no zone id).
bool isIpv6(QStringView text)
{
    const qsizetype gap = text.indexOf(QStringLiteral("::"));
    if (gap >= 0 && text.indexOf(QStringLiteral("::"), gap + 1) >= 0)
        return false;
    int groups = 0;
    const auto countSide = [&groups](QStringView side, bool mayEndInIpv4) {
        if (side.isEmpty())
            return true;
        const auto parts = side.split(QLatin1Char(':'));
        for (qsizetype i = 0; i < parts.size(); ++i) {
            if (mayEndInIpv4 && i == parts.size() - 1
                && parts.at(i).contains(QLatin1Char('.'))) {
                if (!isIpv4(parts.at(i)))
                    return false;
                groups += 2;
            } else if (!isHexGroup(parts.at(i))) {
                return false;
            } else {
                ++groups;
            }
        }
        return true;
    };
    if (gap < 0)
        return countSide(text, true) && groups == 8;
    return countSide(text.left(gap), false)
        && countSide(text.mid(gap + 2), true) && groups <= 7;
}

}

// The spec's server-name grammar, enforced as ruma's validator does.
bool isValidServerName(const QString &serverName)
{
    if (serverName.isEmpty() || serverName.size() > 255)
        return false;
    QStringView rest(serverName);
    QStringView host;
    if (rest.startsWith(QLatin1Char('['))) {
        const qsizetype close = rest.indexOf(QLatin1Char(']'));
        if (close < 0 || !isIpv6(rest.mid(1, close - 1)))
            return false;
        host = rest.left(close + 1);
    } else {
        const qsizetype colon = rest.indexOf(QLatin1Char(':'));
        host = colon < 0 ? rest : rest.left(colon);
        static const QRegularExpression dns(QStringLiteral("^[A-Za-z0-9.\\-]+$"));
        if (!dns.matchView(host).hasMatch())
            return false;
    }
    rest = rest.mid(host.size());
    if (rest.isEmpty())
        return true;
    return rest.front() == QLatin1Char(':') && isDecimal(rest.mid(1), 5)
        && rest.mid(1).toInt() <= 65535;
}

QString exactCandidate(const QString &rawQuery, const QString &ownServerName)
{
    QString query = rawQuery.trimmed();
    if (query.startsWith(QLatin1Char('@')))
        query.remove(0, 1);
    if (query.isEmpty())
        return {};

    const int colon = query.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        const QString localpart = query.left(colon);
        const QString server = query.mid(colon + 1);
        if (!isValidHistoricalLocalpart(localpart) || !isValidServerName(server)
            || localpart.size() + server.size() + 2 > 255)
            return {};
        return QLatin1Char('@') + localpart + QLatin1Char(':') + server;
    }

    if (!isValidLocalpart(query) || !isValidServerName(ownServerName))
        return {};
    return QLatin1Char('@') + query + QLatin1Char(':') + ownServerName;
}

bool queryNamesServer(const QString &rawQuery)
{
    QString query = rawQuery.trimmed();
    if (query.startsWith(QLatin1Char('@')))
        query.remove(0, 1);
    return query.contains(QLatin1Char(':'));
}

namespace {

bool isOpaqueIdPart(const QString &text)
{
    if (text.isEmpty())
        return false;
    for (const QChar c : text)
        if (c.unicode() < 0x21 || c.unicode() > 0x7E)
            return false;
    return true;
}

// A server-less room id is room v12's: a 43-character unpadded base64url
// hash. Aliases always name a server.
bool isValidRoomIdOrAlias(const QString &id)
{
    if (id.size() < 2 || id.size() > 255)
        return false;
    const QChar sigil = id.front();
    if (sigil != QLatin1Char('!') && sigil != QLatin1Char('#'))
        return false;
    const qsizetype colon = id.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        static const QRegularExpression v12(QStringLiteral("^![A-Za-z0-9_-]{43}$"));
        return v12.match(id).hasMatch();
    }
    return isOpaqueIdPart(id.mid(1, colon - 1)) && isValidServerName(id.mid(colon + 1));
}

QString decoded(QStringView segment)
{
    return QUrl::fromPercentEncoding(segment.toUtf8());
}

}

RoomTarget parseRoomTarget(const QString &text)
{
    const QString t = text.trimmed();
    RoomTarget target;
    QString id;
    QString eventId;
    static const QRegularExpression matrixTo(
        QStringLiteral("^(?:https?://)?matrix\\.to/#/"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch link = matrixTo.match(t);
    if (link.hasMatch()) {
        const QString path = t.mid(link.capturedLength()).section(QLatin1Char('?'), 0, 0);
        const QStringList parts = path.split(QLatin1Char('/'));
        if (parts.size() > 2)
            return {};
        id = decoded(parts.at(0));
        if (parts.size() == 2)
            eventId = decoded(parts.at(1));
        target.isLink = true;
    } else if (t.startsWith(QLatin1String("matrix:"), Qt::CaseInsensitive)) {
        const QStringList parts =
            t.mid(7).section(QLatin1Char('?'), 0, 0).split(QLatin1Char('/'));
        if (parts.size() != 2 && parts.size() != 4)
            return {};
        if (parts.at(0) == QLatin1String("roomid"))
            id = QLatin1Char('!') + decoded(parts.at(1));
        else if (parts.at(0) == QLatin1String("r"))
            id = QLatin1Char('#') + decoded(parts.at(1));
        else
            return {};
        if (parts.size() == 4) {
            if (parts.at(2) != QLatin1String("e"))
                return {};
            eventId = QLatin1Char('$') + decoded(parts.at(3));
        }
        target.isLink = true;
    } else {
        id = t;
    }
    if (!isValidRoomIdOrAlias(id))
        return {};
    if (!eventId.isEmpty()
        && (!eventId.startsWith(QLatin1Char('$')) || !isOpaqueIdPart(eventId.mid(1))))
        return {};
    target.id = id;
    target.eventId = eventId;
    return target;
}

}
