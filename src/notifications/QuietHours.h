#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace quiethours {

struct Schedule {
    bool enabled = false;
    int startMinute = 22 * 60;
    int endMinute = 8 * 60;
    bool allowMentions = false;
    bool allowDirect = false;
    QStringList allowedRooms;
};

// Read on the wall clock of `when`'s own zone, so a DST or time-zone change
// moves the window with the clock rather than by a stored offset.
bool activeAt(const Schedule &schedule, const QDateTime &when);

// Whether a notification with these facts still comes through while the
// schedule is active.
bool lets(const Schedule &schedule, const QString &roomId, bool direct,
          bool mention);

}
