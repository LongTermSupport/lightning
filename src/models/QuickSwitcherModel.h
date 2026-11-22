#pragma once

#include "matrix/RoomInfo.h"

#include <functional>

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class SpaceManager;

class QuickSwitcherModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY resultsChanged)
    // Matches each category's cap left out, keyed by category.
    Q_PROPERTY(QVariantMap hiddenByCategory READ hiddenByCategory NOTIFY resultsChanged)

public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        SubtitleRole,
        CategoryRole,
        AvatarUrlRole,
        UnreadRole,
        HighlightRole,
        HasUnreadRole,
        IsSpaceRole,
        IsInviteRole,
        IdentityColorKeyRole,
        LinkRole,
    };

    struct Result {
        QString roomId;
        QString identityColorKey;
        QString name;
        QString subtitle;
        QString avatarUrl;
        QString category;
        int unread = 0;
        int highlight = 0;
        bool hasUnread = false;
        bool isSpace = false;
        bool isInvite = false;
        int score = 0;
        // Set when Enter must go through the link flow (an event target, or an
        // address that is not one of our rooms): it previews, never joins.
        QString link;

        bool operator==(const Result &) const = default;
    };

    explicit QuickSwitcherModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setSpaceManager(SpaceManager *spaces);
    void setNotificationModeSource(std::function<int(const QString &)> modeOf);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QVariantMap hiddenByCategory() const { return m_hidden; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reset();
    Q_INVOKABLE QVariantMap resultAt(int row) const;
    Q_INVOKABLE int indexOfRoom(const QString &roomId) const;

    static int matchScore(const QString &query, const QString &name,
                          const QStringList &alternates);

    const QList<Result> &resultsForTest() const { return m_results; }

Q_SIGNALS:
    void queryChanged();
    // Emitted just before the rows are replaced, so a view can remember which
    // entity was selected and find it again on resultsChanged.
    void resultsAboutToChange();
    void resultsChanged();

private:
    void rebuild();
    bool rebuildForRoomTarget(QList<Result> &next) const;
    void collectMatches(QList<Result> &next, QVariantMap &hidden) const;
    Result resultFor(const RoomInfo &room, int score) const;

    MatrixClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    std::function<int(const QString &)> m_modeOf;
    QString m_query;
    QList<Result> m_results;
    QVariantMap m_hidden;
    // True between refresh()/setQuery() and reset(): only then do room
    // changes rebuild the open palette.
    bool m_active = false;

    static constexpr int kMaxPerCategory = 50;
};
