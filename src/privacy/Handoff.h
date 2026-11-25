#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimeZone>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// Handing one message to another application: a task or calendar file the
// user saves, or plain text on the clipboard. Nothing here sends anything.
namespace lightning::handoff {

// Removes what may carry a credential (Matrix media endpoints, access_token
// links, mxc: URIs) and a reply's quoted fallback, which is another message.
QString scrubText(const QString &text);

struct Fields {
    QString kind;   // "task" or "event"
    QString title;
    QString notes;
    QString link;   // empty unless the user chose to include it
    QString uidSeed;
    QDateTime start; // events only
    int durationMinutes = 60;
};

// Reads the QML map; the link survives only with includeLink true and only
// as a matrix.to or matrix: permalink.
Fields fieldsFrom(const QVariantMap &map, const QTimeZone &zone);
QString plainText(const Fields &fields);
// RFC 5545 VTODO or VEVENT in UTC, with no ORGANIZER or ATTENDEE, so no
// calendar app can send an invitation from it.
QString ics(const Fields &fields, const QDateTime &now);

// The size and name of a file another app asked us to share, or "reason"
// when it is not a readable local regular file.
QVariantMap inspectSharedFile(const QUrl &url);

}

class HandoffExporter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
public:
    using QObject::QObject;

    // The exact bytes "Copy as text" or "Save" would hand over.
    Q_INVOKABLE QString preview(const QVariantMap &fields, bool asFile) const;
    Q_INVOKABLE void copyText(const QVariantMap &fields) const;
    // Writes owner-only to a local file the user chose; false otherwise.
    Q_INVOKABLE bool saveFile(const QUrl &target, const QVariantMap &fields) const;
    Q_INVOKABLE QVariantMap inspectSharedFile(const QUrl &url) const
    {
        return lightning::handoff::inspectSharedFile(url);
    }
};
