#include "notifications/QuietHours.h"

namespace quiethours {

bool activeAt(const Schedule &schedule, const QDateTime &when)
{
    if (!schedule.enabled || schedule.startMinute == schedule.endMinute
        || !when.isValid())
        return false;
    const QTime t = when.time();
    const int m = t.hour() * 60 + t.minute();
    if (schedule.startMinute < schedule.endMinute)
        return m >= schedule.startMinute && m < schedule.endMinute;
    return m >= schedule.startMinute || m < schedule.endMinute;
}

bool lets(const Schedule &schedule, const QString &roomId, bool direct,
          bool mention)
{
    return (mention && schedule.allowMentions)
        || (direct && schedule.allowDirect)
        || (!roomId.isEmpty() && schedule.allowedRooms.contains(roomId));
}

}
