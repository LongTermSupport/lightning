#include "profile/ProfileBioManager.h"
#include "profile/ProfileFieldsManager.h"

namespace {
void reserveBio(const QString &id, const QString &own, int capacity, QStringList &order,
                QSet<QString> &asked, QHash<QString, QString> &cache,
                const QHash<quint64, QString> &inFlight)
{
    if (asked.contains(id))
        return;
    if (asked.size() >= capacity) {
        for (qsizetype i = 0; i < order.size(); ++i) {
            if (order.at(i) == own || inFlight.key(order.at(i)))
                continue;
            const QString oldest = order.takeAt(i);
            cache.remove(oldest);
            asked.remove(oldest);
            break;
        }
    }
    asked.insert(id);
}
}

ProfileBioManager::ProfileBioManager(QObject *parent)
    : QObject(parent)
{
}

void ProfileBioManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clearSession();
    if (!m_client) {
        Q_EMIT availableChanged();
        return;
    }
    connect(m_client, &MatrixClient::profileBioReceived, this,
            &ProfileBioManager::handleReceived);
    connect(m_client, &MatrixClient::profileBioSet, this,
            &ProfileBioManager::handleSet);
    // Answers belong to the account that fetched them.
    connect(m_client, &MatrixClient::loggedOut, this,
            &ProfileBioManager::clearSession);
    Q_EMIT availableChanged();
}

bool ProfileBioManager::available() const
{
    return m_client && m_client->supportsProfileBios();
}

QString ProfileBioManager::bioFor(const QString &userId) const
{
    return m_cache.value(userId);
}

QString ProfileBioManager::ownBio() const
{
    if (!m_client)
        return {};
    return bioFor(m_client->currentUserId());
}

void ProfileBioManager::request(const QString &userId)
{
    if (!m_asked.contains(userId))
        refresh(userId);
}

void ProfileBioManager::refresh(const QString &userId)
{
    if (!available() || userId.isEmpty() || !m_supported || m_inFlight.key(userId)
        || profile_requests::deferIfBusy(m_inFlight, userId))
        return;
    reserveBio(userId, m_client->currentUserId(), kMaxCached, m_order, m_asked,
               m_cache, m_inFlight);
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, userId);
    m_client->fetchProfileBio(userId, opId);
}

void ProfileBioManager::setOwnBio(const QString &text)
{
    if (!available() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setProfileBio(text, m_pendingWrite);
}

void ProfileBioManager::clearOwnBio()
{
    setOwnBio(QString());
}

void ProfileBioManager::cache(const QString &userId, const QString &bio)
{
    if (userId.isEmpty())
        return;
    const QString previous = m_cache.value(userId);
    const bool existed = m_cache.contains(userId);
    if (!existed)
        m_order.append(userId);
    m_cache.insert(userId, bio);
    if (existed && previous == bio)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBioManager::handleReceived(quint64 opId, const QString &userId,
                                       const QString &bio, bool supported)
{
    const auto it = m_inFlight.constFind(opId);
    if (!opId || it == m_inFlight.constEnd() || *it != userId)
        return;
    m_inFlight.erase(it);
    const bool lostSupport = !supported && m_supported;
    if (lostSupport)
        m_supported = false;
    cache(userId, bio);
    refresh(m_inFlight.take(profile_requests::kDeferred));
    if (lostSupport)
        Q_EMIT supportedChanged();
}

void ProfileBioManager::handleSet(quint64 opId, bool ok, const QString &bio,
                                  const QString &category)
{
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        if (category == QLatin1String("unsupported") && m_supported) {
            m_supported = false;
            Q_EMIT supportedChanged();
        }
        return;
    }
    setLastError({});
    if (m_client) {
        const QString own = m_client->currentUserId();
        if (own.isEmpty())
            return;
        reserveBio(own, own, kMaxCached, m_order, m_asked, m_cache, m_inFlight);
        cache(own, bio);
    }
}

void ProfileBioManager::clearSession()
{
    m_cache.clear();
    m_order.clear();
    m_asked.clear();
    m_inFlight.clear();
    m_pendingWrite = 0;
    m_supported = true;
    m_lastError.clear();
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT supportedChanged();
    Q_EMIT busyChanged();
    Q_EMIT lastErrorChanged();
}

void ProfileBioManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
