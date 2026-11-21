#include "models/UserLookup.h"

#include <QRegularExpression>

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
        if (!isValidLocalpart(localpart) || !isValidServerName(server))
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

}
