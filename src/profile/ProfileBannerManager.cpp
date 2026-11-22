#include "profile/ProfileBannerManager.h"
#include "profile/ProfileFieldsManager.h"

#include <QRegularExpression>
#include <QUrl>

namespace {
bool reserveBanner(const QString &id, const QString &pinned, int capacity,
                   QSet<QString> &asked, QHash<QString, QString> &cache,
                   const QHash<quint64, QString> &inFlight, QSet<QString> *writable = nullptr)
{
    if (asked.contains(id))
        return false;
    const bool evict = asked.size() >= capacity;
    if (evict) {
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it.key() == pinned || inFlight.key(it.key()))
                continue;
            asked.remove(it.key());
            if (writable)
                writable->remove(it.key());
            cache.erase(it);
            break;
        }
    }
    asked.insert(id);
    return evict;
}
}

ProfileBannerManager::ProfileBannerManager(QObject *parent)
    : QObject(parent)
{
}

void ProfileBannerManager::setClient(MatrixClient *client)
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
    connect(m_client, &MatrixClient::profileBannerReceived, this,
            &ProfileBannerManager::handleReceived);
    connect(m_client, &MatrixClient::profileBannerSet, this,
            &ProfileBannerManager::handleSet);
    connect(m_client, &MatrixClient::roomBannerReceived, this,
            &ProfileBannerManager::handleRoomReceived);
    connect(m_client, &MatrixClient::roomBannerSet, this,
            &ProfileBannerManager::handleRoomSet);
    connect(m_client, &MatrixClient::loggedOut, this,
            &ProfileBannerManager::clearSession);
    Q_EMIT availableChanged();
}

bool ProfileBannerManager::available() const
{
    return m_client && m_client->supportsProfileBanners();
}

bool ProfileBannerManager::roomBannersAvailable() const
{
    return m_client && m_client->supportsRoomBanners();
}

QString ProfileBannerManager::bannerFor(const QString &userId) const
{
    return m_cache.value(userId);
}

QString ProfileBannerManager::ownBanner() const
{
    if (!m_client)
        return {};
    return bannerFor(m_client->currentUserId());
}

void ProfileBannerManager::request(const QString &userId)
{
    if (!available() || userId.isEmpty() || !m_supported)
        return;
    if (m_asked.contains(userId))
        return;
    if (profile_requests::deferIfBusy(m_inFlight, userId))
        return;
    const bool evicted = reserveBanner(userId, m_client->currentUserId(), kMaxCached,
                                       m_asked, m_cache, m_inFlight);
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, userId);
    m_client->fetchProfileBanner(userId, opId);
    if (evicted) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
}

// Converts a picked file URL to a path via QUrl: stripping "file://" by hand
// turns file:///C:/x.png into /C:/x.png on Windows. Plain paths pass through.
static QString localPathFrom(const QString &pathOrUrl)
{
    if (pathOrUrl.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl url(pathOrUrl);
        return url.isLocalFile() ? url.toLocalFile() : QString();
    }
    static const QRegularExpression scheme(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    if (scheme.match(pathOrUrl).hasMatch())
        return {};
    return pathOrUrl;
}

void ProfileBannerManager::setOwnBanner(const QString &pathOrUrl)
{
    const QString localPath = localPathFrom(pathOrUrl);
    if (!available() || localPath.isEmpty() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setProfileBanner(localPath, m_pendingWrite);
}

void ProfileBannerManager::clearOwnBanner()
{
    if (!available() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setProfileBanner(QString(), m_pendingWrite);
}

void ProfileBannerManager::handleReceived(quint64 opId, const QString &userId,
                                          const QString &mxc, bool supported)
{
    const auto it = m_inFlight.constFind(opId);
    if (!opId || it == m_inFlight.constEnd() || *it != userId)
        return;
    m_inFlight.erase(it);
    const auto deferred = m_inFlight.take(profile_requests::kDeferred);

    const bool lostSupport = !supported && m_supported;
    if (lostSupport)
        m_supported = false;
    const QString previous = m_cache.value(userId);
    m_cache.insert(userId, mxc);
    request(deferred);
    if (lostSupport)
        Q_EMIT supportedChanged();
    if (previous == mxc)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::handleSet(quint64 opId, bool ok, const QString &mxc,
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
    if (!m_client)
        return;
    const QString self = m_client->currentUserId();
    if (self.isEmpty())
        return;
    reserveBanner(self, self, kMaxCached, m_asked, m_cache, m_inFlight);
    m_cache.insert(self, mxc);
    ++m_revision;
    Q_EMIT revisionChanged();
}

QString ProfileBannerManager::roomBannerFor(const QString &roomId) const
{
    return m_roomCache.value(roomId);
}

bool ProfileBannerManager::canSetRoomBanner(const QString &roomId) const
{
    return m_roomWritable.contains(roomId);
}

void ProfileBannerManager::requestRoom(const QString &roomId)
{
    if (roomId.isEmpty() || m_roomAsked.contains(roomId))
        return;
    refreshRoom(roomId);
}

void ProfileBannerManager::refreshRoom(const QString &roomId)
{
    if (!roomBannersAvailable() || roomId.isEmpty())
        return;
    if (m_roomInFlight.key(roomId) || profile_requests::deferIfBusy(m_roomInFlight, roomId))
        return;
    const bool evicted = reserveBanner(roomId, {}, kMaxCached, m_roomAsked, m_roomCache,
                                       m_roomInFlight, &m_roomWritable);
    const quint64 opId = m_nextOpId++;
    m_roomInFlight.insert(opId, roomId);
    m_client->fetchRoomBanner(roomId, opId);
    if (evicted) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
}

void ProfileBannerManager::setRoomBanner(const QString &roomId,
                                         const QString &pathOrUrl)
{
    const QString localPath = localPathFrom(pathOrUrl);
    if (!roomBannersAvailable() || roomId.isEmpty() || localPath.isEmpty()
        || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setRoomBanner(roomId, localPath, m_pendingWrite);
}

void ProfileBannerManager::clearRoomBanner(const QString &roomId)
{
    if (!roomBannersAvailable() || roomId.isEmpty() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setRoomBanner(roomId, QString(), m_pendingWrite);
}

void ProfileBannerManager::handleRoomReceived(quint64 opId,
                                              const QString &roomId,
                                              const QString &mxc, bool canSet)
{
    const auto it = m_roomInFlight.constFind(opId);
    if (!opId || it == m_roomInFlight.constEnd() || *it != roomId)
        return;
    m_roomInFlight.erase(it);
    const auto deferred = m_roomInFlight.take(profile_requests::kDeferred);

    const QString previous = m_roomCache.value(roomId);
    const bool couldSet = m_roomWritable.contains(roomId);
    m_roomCache.insert(roomId, mxc);
    if (canSet)
        m_roomWritable.insert(roomId);
    else
        m_roomWritable.remove(roomId);
    refreshRoom(deferred);
    if (previous == mxc && couldSet == canSet)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::handleRoomSet(quint64 opId, const QString &roomId,
                                         bool ok, const QString &mxc,
                                         const QString &category)
{
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        return;
    }
    setLastError({});
    reserveBanner(roomId, {}, kMaxCached, m_roomAsked, m_roomCache, m_roomInFlight,
                  &m_roomWritable);
    m_roomCache.insert(roomId, mxc);
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::clearSession()
{
    m_cache.clear();
    m_asked.clear();
    m_inFlight.clear();
    m_roomCache.clear();
    m_roomAsked.clear();
    m_roomWritable.clear();
    m_roomInFlight.clear();
    m_pendingWrite = 0;
    setLastError({});
    if (!m_supported) {
        m_supported = true;
        Q_EMIT supportedChanged();
    }
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT busyChanged();
}

void ProfileBannerManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
