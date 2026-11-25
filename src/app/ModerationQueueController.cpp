#include "app/ModerationQueueController.h"

#include "matrix/MatrixClient.h"

ModerationQueueController::ModerationQueueController(QObject *parent)
    : QObject(parent)
{
}

void ModerationQueueController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::spaceCommunityFinished, this,
                &ModerationQueueController::onFinished);
        connect(m_client, &MatrixClient::loggedOut, this, &ModerationQueueController::close);
    }
    close();
}

void ModerationQueueController::reset()
{
    m_cases.clear();
    m_pending.clear();
    m_results.clear();
    m_message.clear();
    m_loadOp = 0;
    m_actOp = 0;
    m_actUser.clear();
    m_canBan = false;
    m_truncated = false;
    m_listCount = 0;
}

void ModerationQueueController::open(const QString &roomId)
{
    if (roomId != m_roomId) {
        reset();
        m_roomId = roomId;
    }
    refresh();
}

void ModerationQueueController::refresh()
{
    if (!m_client || m_roomId.isEmpty() || m_loadOp)
        return;
    m_loadOp = m_client->spaceCommunityAction(m_roomId, QStringLiteral("mod_cases"), {});
    if (!m_loadOp)
        m_message = tr("The moderation queue is unavailable for this room.");
    Q_EMIT changed();
}

void ModerationQueueController::close()
{
    reset();
    m_roomId.clear();
    Q_EMIT changed();
}

QVariantMap ModerationQueueController::caseFor(const QString &userId) const
{
    for (const QVariant &c : m_cases)
        if (c.toMap().value(QStringLiteral("user")).toString() == userId)
            return c.toMap();
    return {};
}

bool ModerationQueueController::requestBan(const QString &userId)
{
    const QVariantMap c = caseFor(userId);
    if (c.isEmpty() || !m_canBan || m_actOp
        || c.value(QStringLiteral("status")).toString() == QLatin1String("banned"))
        return false;
    m_pending = c;
    Q_EMIT changed();
    return true;
}

void ModerationQueueController::cancelRequest()
{
    m_pending.clear();
    Q_EMIT changed();
}

bool ModerationQueueController::confirmBan(const QString &userId, const QString &reason)
{
    if (!m_client || m_actOp || userId.isEmpty()
        || m_pending.value(QStringLiteral("user")).toString() != userId)
        return false;
    m_pending.clear();
    m_actUser = userId;
    m_actOp = m_client->spaceCommunityAction(
        m_roomId, QStringLiteral("mod_act"),
        {{QStringLiteral("user"), userId}, {QStringLiteral("decision"), QStringLiteral("ban")},
         {QStringLiteral("reason"), reason.left(500)}});
    if (!m_actOp)
        setResult(userId, describe({}));
    Q_EMIT changed();
    return m_actOp != 0;
}

bool ModerationQueueController::setDecision(const QString &userId, const QString &decision)
{
    if (!m_client || m_actOp || caseFor(userId).isEmpty()
        || (decision != QLatin1String("dismissed") && decision != QLatin1String("open")))
        return false;
    m_actUser = userId;
    m_actOp = m_client->spaceCommunityAction(
        m_roomId, QStringLiteral("mod_act"),
        {{QStringLiteral("user"), userId}, {QStringLiteral("decision"), decision}});
    Q_EMIT changed();
    return m_actOp != 0;
}

void ModerationQueueController::setResult(const QString &userId, const QString &result)
{
    m_results.insert(userId, result);
    for (QVariant &c : m_cases) {
        QVariantMap map = c.toMap();
        if (map.value(QStringLiteral("user")).toString() == userId) {
            map.insert(QStringLiteral("result"), result);
            c = map;
        }
    }
}

QString ModerationQueueController::describe(const QString &category)
{
    if (category == QLatin1String("stale"))
        return tr("Not banned: no followed list covers this person any more.");
    if (category == QLatin1String("no_permission") || category == QLatin1String("forbidden"))
        return tr("Not banned: you no longer have permission to ban here.");
    if (category == QLatin1String("outranked"))
        return tr("Not banned: this person's power level is not below yours.");
    if (category == QLatin1String("already_banned"))
        return tr("Already banned.");
    if (category == QLatin1String("not_a_member"))
        return tr("Not banned: this person is no longer in the room.");
    if (category == QLatin1String("rate_limited"))
        return tr("Not banned: the server is rate limiting. Try again shortly.");
    return tr("Not banned: the action failed. Try again.");
}

void ModerationQueueController::onFinished(quint64 opId, const QString &roomId,
                                           const QString &action, bool ok,
                                           const QVariantMap &result)
{
    if (!opId || roomId != m_roomId)
        return;
    if (opId == m_loadOp && action == QLatin1String("mod_cases")) {
        m_loadOp = 0;
        if (ok) {
            m_cases = result.value(QStringLiteral("cases")).toList();
            m_canBan = result.value(QStringLiteral("can_ban")).toBool();
            m_truncated = result.value(QStringLiteral("truncated")).toBool();
            m_listCount = result.value(QStringLiteral("lists")).toInt();
            m_message.clear();
            const QVariantMap results = m_results;
            for (auto it = results.cbegin(); it != results.cend(); ++it)
                setResult(it.key(), it.value().toString());
            // A case that changed under an open confirmation must be reviewed again.
            if (!m_pending.isEmpty()
                && caseFor(m_pending.value(QStringLiteral("user")).toString()) != m_pending)
                m_pending.clear();
        } else {
            m_message = tr("The moderation queue could not be loaded.");
        }
        Q_EMIT changed();
        return;
    }
    if (opId != m_actOp || action != QLatin1String("mod_act"))
        return;
    m_actOp = 0;
    const QString user = m_actUser;
    m_actUser.clear();
    const QString status = result.value(QStringLiteral("status")).toString();
    if (ok && status == QLatin1String("banned"))
        setResult(user, result.value(QStringLiteral("recorded")).toBool()
                            ? tr("Banned.") : tr("Banned. The decision could not be saved."));
    else if (!ok)
        setResult(user, describe(result.value(QStringLiteral("category")).toString()));
    Q_EMIT changed();
    refresh();
}
