#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <functional>

// Personal "Needs reply" / "Waiting" marks on messages for the signed-in
// account. Local only: the file under the account's recorded root holds ids,
// a state and a time, never text, and nothing here talks to the server.
class FollowUpController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(int openCount READ openCount NOTIFY changed)
public:
    static constexpr int kMaxEntries = 500;

    struct Item {
        QString roomId;
        QString eventId;
        QString rootId;
        QString state;
        qint64 atMs = 0;
    };

    explicit FollowUpController(QObject *parent = nullptr);
    void setRoomNameSource(std::function<QString(const QString &)> nameOf)
    { m_nameOf = std::move(nameOf); }
    void setClock(std::function<qint64()> now) { m_now = std::move(now); }

    void openFor(const QString &filePath);
    void close();
    QString filePath() const { return m_path; }

    int revision() const { return m_revision; }
    int openCount() const { return int(m_items.size()); }
    QVariantList items() const;
    QVector<Item> entries() const { return m_items; }

    // "reply", "waiting" or "" for an unmarked message.
    Q_INVOKABLE QString stateOf(const QString &roomId, const QString &eventId) const;
    Q_INVOKABLE void setState(const QString &roomId, const QString &eventId,
                              const QString &threadRootId, const QString &state);
    Q_INVOKABLE void markDone(const QString &roomId, const QString &eventId);
    // True once the user sent a reply that may answer this mark.
    Q_INVOKABLE bool replied(const QString &roomId, const QString &eventId) const;

    // The user's own message: a reply to a "reply" mark, or a message in its
    // thread, offers completion. It never completes anything by itself.
    void noteOwnMessage(const QString &roomId, const QString &replyToEventId,
                        const QString &threadRootId);

Q_SIGNALS:
    void changed();
    void completionOffered(const QString &roomId, const QString &eventId);

private:
    static bool validId(const QString &id, QChar sigil);
    qint64 nowMs() const;
    void load();
    void write();
    void bump();

    std::function<QString(const QString &)> m_nameOf;
    std::function<qint64()> m_now;
    QString m_path;
    QVector<Item> m_items;
    QSet<QString> m_replied;
    int m_revision = 0;
};
