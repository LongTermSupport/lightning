#include "app/UrlLauncher.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QProcess>
#include <QRegularExpression>
#include <QUrlQuery>

#include <cmath>

namespace lightning::urls {

QProcessEnvironment childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!env.contains(QStringLiteral("APPDIR")))
        return env;

    const QString original =
        env.value(QStringLiteral("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH"));
    if (original.isEmpty())
        env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    else
        env.insert(QStringLiteral("LD_LIBRARY_PATH"), original);

    for (const char *key : { "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_PLUGIN_PATH_1_0",
                             "GST_PLUGIN_SCANNER_1_0", "GST_PLUGIN_SCANNER",
                             "GST_REGISTRY_1_0",
                             "SPA_PLUGIN_DIR", "PIPEWIRE_MODULE_DIR",
                             "PIPEWIRE_CONFIG_DIR", "QT_PLUGIN_PATH",
                             "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                             "PYTHONHOME", "PERLLIB", "GSETTINGS_SCHEMA_DIR",
                             "XDG_DATA_DIRS_APPIMAGE" }) {
        const QString name = QString::fromLatin1(key);
        const QString saved =
            env.value(QStringLiteral("APPIMAGE_ORIGINAL_") + name);
        if (saved.isEmpty())
            env.remove(name);
        else
            env.insert(name, saved);
        env.remove(QStringLiteral("APPIMAGE_ORIGINAL_") + name);
    }
    return env;
}

bool isCredentialBearingMatrixUrl(const QUrl &url)
{
    static const QRegularExpression mediaPath(
        QStringLiteral("/_matrix/(?:client/(?:[^/]+/)+)?media(?:/|$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (mediaPath.match(url.path(QUrl::FullyDecoded)).hasMatch())
        return true;
    for (const QUrlQuery &query :
         {QUrlQuery(url), QUrlQuery(url.fragment(QUrl::FullyEncoded))}) {
        for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
            if (item.first.compare(QLatin1String("access_token"),
                                   Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    return false;
}

QString refusalNotice(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if ((scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        && isCredentialBearingMatrixUrl(url))
        return QCoreApplication::translate(
            "UrlLauncher",
            "This link points at Matrix media and can't be opened in a browser.");
    return {};
}

bool isOpenableExternally(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return false;
    // Only web and mail links. Everything else (file:, javascript:, data:, and
    // Windows handlers that have been RCE vectors such as ms-msdt:) is refused
    // here; the only other exit is openMapPoint's geo: URI built from numbers.
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return !url.host().isEmpty() && url.userInfo().isEmpty()
            && !isCredentialBearingMatrixUrl(url);
    if (scheme == QLatin1String("mailto"))
        return !url.path().isEmpty();
    return false;
}

namespace {
bool launch(const QUrl &url)
{
    const QProcessEnvironment env = childEnvironment();
    if (!env.contains(QStringLiteral("APPDIR")))
        return QDesktopServices::openUrl(url);

    QProcess opener;
    opener.setProgram(QStringLiteral("xdg-open"));
    opener.setArguments({ url.toString(QUrl::FullyEncoded) });
    opener.setProcessEnvironment(env);
    if (opener.startDetached())
        return true;
    return QDesktopServices::openUrl(url);
}
}

bool openExternally(const QUrl &url)
{
    return isOpenableExternally(url) && launch(url);
}

QUrl mapUrl(double lat, double lon, const QString &provider)
{
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0
        || lon < -180.0 || lon > 180.0)
        return {};
    const QString la = QString::number(lat, 'f', 6);
    const QString lo = QString::number(lon, 'f', 6);
    if (provider == QLatin1String("geo"))
        return QUrl(QStringLiteral("geo:%1,%2").arg(la, lo));
    if (provider == QLatin1String("osm"))
        return QUrl(QStringLiteral(
            "https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2").arg(la, lo));
    return {};
}

bool openMapPoint(double lat, double lon, const QString &provider)
{
    const QUrl url = mapUrl(lat, lon, provider);
    if (url.isEmpty())
        return false;
    // A geo: URI is built here from two numbers, never taken from a message.
    return url.scheme() == QLatin1String("geo") ? launch(url) : openExternally(url);
}

}
