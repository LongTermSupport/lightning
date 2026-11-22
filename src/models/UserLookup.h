#pragma once

#include <QString>

namespace matrix::user_lookup {

QString serverNameFromUserId(const QString &userId);

// Strict Matrix localpart grammar. Candidates are only built from it, so no
// malformed id is fabricated (historical ids still work when typed in full).
bool isValidLocalpart(const QString &localpart);

bool isValidServerName(const QString &serverName);

// The spec's historical grammar, which servers still hold ids in: printable
// ASCII except ':'. Accepted only when the user typed the whole id.
bool isValidHistoricalLocalpart(const QString &localpart);

// A room id, alias or matrix.to / matrix: room link, parsed locally.
struct RoomTarget {
    QString id;
    QString eventId;
    bool isLink = false;
    bool isValid() const { return !id.isEmpty(); }
};
RoomTarget parseRoomTarget(const QString &text);

QString exactCandidate(const QString &rawQuery, const QString &ownServerName);

bool queryNamesServer(const QString &rawQuery);

QString localpartOrUserId(const QString &userId);

}
