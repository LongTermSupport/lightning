#include "profile/UserProfileResolver.h"

#include "matrix/MatrixClient.h"

UserProfileResolver::UserProfileResolver(QObject *parent)
    : QObject(parent), m_retryExpiry(this)
{
    m_clock.start();
    m_retryExpiry.setSingleShot(true);
    connect(&m_retryExpiry, &QTimer::timeout, this, &UserProfileResolver::expireFailures);
}

void UserProfileResolver::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clear();
    if (m_client) {
        connect(m_client, &MatrixClient::userProfileFinished, this,
                &UserProfileResolver::onFinished);
        // Profiles belong to the account that resolved them. An account switch
        // reuses the same MatrixClient (detachSession() emits loggedOut), so
        // setClient() alone would carry names, avatars and refusals over to the
        // next account. The sibling profile managers do the same.
        connect(m_client, &MatrixClient::loggedOut, this,
                &UserProfileResolver::clear);
        connect(m_client, &MatrixClient::userProfileChanged, this,
                [this](const QString &userId) {
                    if (m_profiles.contains(userId) && !m_asking.contains(userId))
                        fetch(userId);
                });
    }
}

void UserProfileResolver::clear()
{
    m_profiles.clear();
    m_inFlight.clear();
    m_asking.clear();
    m_failedAt.clear();
    m_profileOrder.clear();
    m_failureOrder.clear();
    m_waiting.clear();
    m_retryExpiry.stop();
}

UserProfileResolver::Profile
UserProfileResolver::profile(const QString &userId) const
{
    return m_profiles.value(userId);
}

QVariantMap UserProfileResolver::lookup(const QString &userId)
{
    request(userId, true);
    const Profile p = m_profiles.value(userId);
    QVariantMap out;
    out.insert(QStringLiteral("displayName"), p.displayName);
    out.insert(QStringLiteral("avatarUrl"), p.avatarUrl);
    out.insert(QStringLiteral("known"), p.known);
    return out;
}

void UserProfileResolver::request(const QString &userId)
{
    request(userId, false);
}

void UserProfileResolver::request(const QString &userId, bool interactive)
{
    if (!m_client || userId.isEmpty() || !userId.startsWith(QLatin1Char('@'))
        || !userId.contains(QLatin1Char(':')))
        return;
    if (m_profiles.contains(userId) || m_asking.contains(userId))
        return;
    fetch(userId, interactive);
}

void UserProfileResolver::expireFailures()
{
    const qint64 now = m_clock.elapsed();
    while (!m_failureOrder.isEmpty()) {
        const qint64 remaining = m_failedAt.value(m_failureOrder.first()) + m_failureRetryMs - now;
        if (remaining > 0) {
            m_retryExpiry.start(int(remaining));
            return;
        }
        m_failedAt.remove(m_failureOrder.takeFirst());
    }
    m_retryExpiry.stop();
}

void UserProfileResolver::makeRoom(const QString &userId)
{
    if (m_profiles.size() + m_failedAt.size() + m_inFlight.size() < kMaxProfiles)
        return;
    if (!m_failureOrder.isEmpty()) {
        m_failedAt.remove(m_failureOrder.takeFirst());
        return;
    }
    const auto own = m_client->currentUserId();
    for (qsizetype i = 0; i < m_profileOrder.size(); ++i) {
        const auto &id = m_profileOrder.at(i);
        if (id != own && id != userId && !m_asking.contains(id)) {
            m_profiles.remove(m_profileOrder.takeAt(i));
            return;
        }
    }
}

void UserProfileResolver::fetch(const QString &userId, bool interactive)
{
    const auto failed = m_failedAt.constFind(userId);
    if (failed != m_failedAt.constEnd()) {
        if (m_clock.elapsed() - *failed < m_failureRetryMs)
            return;
        expireFailures();
    }
    if (m_inFlight.size() >= kMaxInFlight) {
        if (interactive) {
            m_waiting.removeOne(userId);
            if (m_waiting.size() == kMaxWaiting)
                m_waiting.removeLast();
            m_waiting.prepend(userId);
        } else if (m_waiting.size() < kMaxWaiting && !m_waiting.contains(userId)) {
            m_waiting.append(userId);
        }
        return;
    }
    const quint64 op = m_client->fetchUserProfile(userId);
    if (op == 0)
        return;
    makeRoom(userId);
    m_inFlight.insert(op, userId);
    m_asking.insert(userId);
}

void UserProfileResolver::onFinished(quint64 opId, bool ok,
                                     const QString &userId,
                                     const QString &displayName,
                                     const QString &avatarUrl,
                                     const QString &category)
{
    Q_UNUSED(category);
    const auto it = m_inFlight.find(opId);
    if (sender() != m_client || it == m_inFlight.end())
        return;
    const QString asked = it.value();
    m_inFlight.erase(it);
    m_asking.remove(asked);
    if (!ok || (!userId.isEmpty() && userId != asked)) {
        m_failedAt.insert(asked, m_clock.elapsed());
        m_failureOrder.append(asked);
        if (!m_retryExpiry.isActive())
            expireFailures();
    } else {
        Profile p;
        p.displayName = displayName.trimmed();
        p.avatarUrl = avatarUrl.startsWith(QLatin1String("mxc://")) ? avatarUrl : QString();
        p.known = true;
        if (!m_profiles.contains(asked))
            m_profileOrder.append(asked);
        m_profiles.insert(asked, p);
        Q_EMIT resolved(asked, p.displayName, p.avatarUrl);
    }
    while (!m_waiting.isEmpty() && m_inFlight.size() < kMaxInFlight) {
        const auto next = m_waiting.takeFirst();
        if (!m_asking.contains(next))
            fetch(next);
    }
}
