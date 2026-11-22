#include "models/QuickSwitcherModel.h"

#include "matrix/MatrixClient.h"
#include "matrix/RoomUnreadRule.h"
#include "models/TextFold.h"
#include "models/UserLookup.h"
#include "spaces/SpaceManager.h"

#include <QCoreApplication>

#include <algorithm>

QuickSwitcherModel::QuickSwitcherModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void QuickSwitcherModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::loggedOut, this,
                &QuickSwitcherModel::reset);
        connect(m_client, &MatrixClient::roomsChanged, this, [this] {
            if (m_active)
                rebuild();
        });
    }
    reset();
}

void QuickSwitcherModel::setSpaceManager(SpaceManager *spaces)
{
    m_spaces = spaces;
}

int QuickSwitcherModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_results.size());
}

QVariant QuickSwitcherModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_results.size())
        return {};
    const Result &r = m_results.at(index.row());
    switch (role) {
    case RoomIdRole:    return r.roomId;
    case IdentityColorKeyRole:
        return r.identityColorKey.isEmpty() ? r.roomId : r.identityColorKey;
    case NameRole:      return r.name;
    case SubtitleRole:  return r.subtitle;
    case CategoryRole:  return r.category;
    case AvatarUrlRole: return r.avatarUrl;
    case UnreadRole:    return r.unread;
    case HighlightRole: return r.highlight;
    case HasUnreadRole: return r.hasUnread;
    case IsSpaceRole:   return r.isSpace;
    case IsInviteRole:  return r.isInvite;
    case LinkRole:      return r.link;
    default:            return {};
    }
}

QHash<int, QByteArray> QuickSwitcherModel::roleNames() const
{
    return {
        { RoomIdRole,    "roomId" },
        { IdentityColorKeyRole, "identityColorKey" },
        { NameRole,      "name" },
        { SubtitleRole,  "subtitle" },
        { CategoryRole,  "category" },
        { AvatarUrlRole, "avatarUrl" },
        { UnreadRole,    "unread" },
        { HighlightRole, "highlight" },
        { HasUnreadRole, "hasUnread" },
        { IsSpaceRole,   "isSpace" },
        { IsInviteRole,  "isInvite" },
        { LinkRole,      "link" },
    };
}

void QuickSwitcherModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();
    m_active = true;
    rebuild();
}

void QuickSwitcherModel::refresh()
{
    m_active = true;
    rebuild();
}

void QuickSwitcherModel::reset()
{
    const bool hadQuery = !m_query.isEmpty();
    m_query.clear();
    m_active = false;
    if (!m_results.isEmpty() || !m_hidden.isEmpty()) {
        Q_EMIT resultsAboutToChange();
        beginResetModel();
        m_results.clear();
        m_hidden.clear();
        endResetModel();
        Q_EMIT resultsChanged();
    }
    if (hadQuery)
        Q_EMIT queryChanged();
}

QVariantMap QuickSwitcherModel::resultAt(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_results.size())
        return map;
    const Result &r = m_results.at(row);
    map.insert(QStringLiteral("roomId"), r.roomId);
    map.insert(QStringLiteral("category"), r.category);
    map.insert(QStringLiteral("isSpace"), r.isSpace);
    map.insert(QStringLiteral("isInvite"), r.isInvite);
    map.insert(QStringLiteral("name"), r.name);
    map.insert(QStringLiteral("subtitle"), r.subtitle);
    map.insert(QStringLiteral("avatarUrl"), r.avatarUrl);
    map.insert(QStringLiteral("unread"), r.unread);
    map.insert(QStringLiteral("highlight"), r.highlight);
    map.insert(QStringLiteral("hasUnread"), r.hasUnread);
    map.insert(QStringLiteral("link"), r.link);
    return map;
}

int QuickSwitcherModel::indexOfRoom(const QString &roomId) const
{
    for (int row = 0; row < m_results.size(); ++row)
        if (m_results.at(row).roomId == roomId)
            return row;
    return -1;
}

int QuickSwitcherModel::matchScore(const QString &query, const QString &name,
                                   const QStringList &alternates)
{
    const QString q = textfold::fold(query.trimmed());
    const QString lowerName = textfold::fold(name);
    if (q.isEmpty())
        return 0;

    QString haystack = lowerName;
    bool exactAlternate = false;
    for (const QString &alt : alternates) {
        const QString folded = textfold::fold(alt);
        exactAlternate = exactAlternate || folded == q;
        haystack += QLatin1Char(' ');
        haystack += folded;
    }
    if (exactAlternate)
        return 3000;
    if (lowerName == q)
        return 2000;

    const auto tokens = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (!haystack.contains(token))
            return -1;
    }

    int score = 100;
    if (lowerName.startsWith(q))
        score = 1000;
    else if (lowerName.contains(QLatin1Char(' ') + q))
        score = 500;
    else if (lowerName.contains(q))
        score = 300;
    return score;
}

void QuickSwitcherModel::setNotificationModeSource(
    std::function<int(const QString &)> modeOf)
{
    m_modeOf = std::move(modeOf);
}

QuickSwitcherModel::Result QuickSwitcherModel::resultFor(const RoomInfo &room,
                                                        int score) const
{
    const bool isInvite = room.membership == RoomInfo::Invited;
    Result r;
    r.roomId = room.id;
    r.identityColorKey = identityColorKey(room);
    r.name = room.name.isEmpty() ? room.id : room.name;
    r.avatarUrl = room.avatarUrl;
    const int mode = (m_modeOf && !isInvite)
        ? m_modeOf(room.id) : roomunread::kModeAllMessages;
    r.unread = roomunread::visibleUnreadCount(room, mode);
    r.highlight = roomunread::mentionCount(room, mode);
    r.hasUnread = room.markedUnread
                  || roomunread::countsAsUnread(room, mode);
    r.isSpace = room.isSpace;
    r.isInvite = isInvite;
    r.score = score;
    if (isInvite) {
        r.category = QStringLiteral("invite");
        r.subtitle = QCoreApplication::translate(
            "QuickSwitcher", "Invitation");
    } else if (room.isSpace) {
        r.category = QStringLiteral("space");
        r.subtitle =
            QCoreApplication::translate("QuickSwitcher", "Space");
    } else if (room.isDirect) {
        r.category = QStringLiteral("dm");
        r.subtitle = room.directUserId.isEmpty()
            ? QCoreApplication::translate("QuickSwitcher",
                                          "Direct message")
            : room.directUserId;
    } else {
        r.category = QStringLiteral("room");
        r.subtitle = room.canonicalAlias;
    }
    return r;
}

// A pasted room id, alias or room link is answered from this account's own
// rooms only; nothing is sent anywhere until the user activates a row.
bool QuickSwitcherModel::rebuildForRoomTarget(QList<Result> &next) const
{
    const auto target = matrix::user_lookup::parseRoomTarget(m_query);
    if (!target.isValid())
        return false;
    const bool alias = target.id.startsWith(QLatin1Char('#'));
    if (m_client) {
        const auto rooms = m_client->rooms();
        for (const auto &room : rooms) {
            if (room.membership != RoomInfo::Joined
                && room.membership != RoomInfo::Invited)
                continue;
            if (alias ? room.canonicalAlias != target.id : room.id != target.id)
                continue;
            Result r = resultFor(room, 3000);
            if (!target.eventId.isEmpty())
                r.link = m_query.trimmed();
            next.append(std::move(r));
            return true;
        }
        if (m_client->supportsRoomDiscovery()) {
            Result r;
            r.roomId = target.id;
            r.name = target.id;
            r.category = QStringLiteral("address");
            r.subtitle = QCoreApplication::translate(
                "QuickSwitcher", "Not in your rooms. Preview it before joining");
            r.link = m_query.trimmed();
            r.score = 3000;
            next.append(std::move(r));
        }
    }
    return true;
}

void QuickSwitcherModel::rebuild()
{
    QList<Result> next;
    QVariantMap hidden;
    if (!rebuildForRoomTarget(next))
        collectMatches(next, hidden);

    // A sync that changes nothing shown must not reset the open list.
    if (next == m_results && hidden == m_hidden)
        return;
    Q_EMIT resultsAboutToChange();
    beginResetModel();
    m_results = std::move(next);
    m_hidden = std::move(hidden);
    endResetModel();
    Q_EMIT resultsChanged();
}

void QuickSwitcherModel::collectMatches(QList<Result> &next, QVariantMap &hidden) const
{
    if (m_client) {
        const auto rooms = m_client->rooms();
        for (const auto &room : rooms) {
            if (room.membership != RoomInfo::Joined
                && room.membership != RoomInfo::Invited)
                continue;

            QStringList alternates;
            if (!room.canonicalAlias.isEmpty())
                alternates << room.canonicalAlias;
            if (room.isDirect && !room.directUserId.isEmpty())
                alternates << room.directUserId;

            const QString displayName =
                room.name.isEmpty() ? room.id : room.name;
            const int score = matchScore(m_query, displayName, alternates);
            if (score < 0)
                continue;
            next.append(resultFor(room, score));
        }
    }

    // Category is the primary sort key so the switcher renders contiguous
    // sections: rooms, people, spaces, then invites (Enter on an invite only
    // opens its accept/decline view). The cap applies per category, after the
    // relevance sort, so a scope never sees a list another category truncated.
    static const auto categoryRank = [](const Result &r) {
        if (r.category == QStringLiteral("room"))  return 0;
        if (r.category == QStringLiteral("dm"))     return 1;
        if (r.category == QStringLiteral("space"))  return 2;
        return 3;
    };
    std::sort(next.begin(), next.end(), [](const Result &a, const Result &b) {
        const int rankA = categoryRank(a);
        const int rankB = categoryRank(b);
        if (rankA != rankB)
            return rankA < rankB;
        if (a.score != b.score)
            return a.score > b.score;
        if (a.highlight != b.highlight)
            return a.highlight > b.highlight;
        if (a.hasUnread != b.hasUnread)
            return a.hasUnread;
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        if (byName != 0)
            return byName < 0;
        return a.roomId < b.roomId;
    });
    int kept = 0;
    int inCategory = 0;
    int previousRank = -1;
    for (int i = 0; i < next.size(); ++i) {
        const int rank = categoryRank(next.at(i));
        if (rank != previousRank)
            inCategory = 0;
        previousRank = rank;
        if (inCategory++ >= kMaxPerCategory) {
            const QString cat = next.at(i).category;
            hidden.insert(cat, hidden.value(cat).toInt() + 1);
            continue;
        }
        if (kept != i)
            next[kept] = std::move(next[i]);
        ++kept;
    }
    next.erase(next.begin() + kept, next.end());
}
