#include "metadata/Transport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslSocket>

#include <memory>

namespace papertrail::metadata {

namespace {

constexpr qint64 kMaxBodyBytes = 8 * 1024 * 1024; // Far more than any record. This isn't reading a file.
constexpr int kTimeoutMs = 30000;
constexpr qint64 kLongestWaitSeconds = 24 * 60 * 60;

// Sources give either a number of seconds, or the moment their limit resets. Nothing's believed
// beyond a day, because a mistaken header mustn't stop lookups for good.
int secondsToWait(const QByteArray &header)
{
    qint64 value = header.trimmed().toLongLong();
    if (value > 1000000000)
        value -= QDateTime::currentSecsSinceEpoch();
    return int(qBound<qint64>(0, value, kLongestWaitSeconds));
}

// Whether a download may be sent on to `to`: securely, to a host it allows, or here for tests.
bool mayRedirectTo(const QUrl &to, const QStringList &hosts)
{
    const QString host = to.host().toLower();
    if (to.scheme() != QLatin1StringView("https") && host != QLatin1StringView("127.0.0.1"))
        return false;
    if (!to.userInfo().isEmpty())
        return false;
    for (const QString &allowed : hosts) {
        if (allowed.startsWith(QLatin1Char('.')) ? host.endsWith(allowed.toLower()) : host == allowed.toLower())
            return true;
    }
    return false;
}

constexpr int kMostRedirects = 3;

} // namespace

// One download as it goes, carried from request to request as redirects are followed.
struct NetworkTransport::Download {
    DownloadRequest request;
    std::function<void(const QString &)> done;
    std::unique_ptr<QSaveFile> file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 received = 0;
    QString problem;
    int redirects = 0;
};

void Transport::send(const HttpRequest &, const Callback &callback)
{
    TransportReply reply;
    reply.error = QStringLiteral("this can't send requests of this kind");
    callback(reply);
}

void getWithHeaders(Transport &transport, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers,
                    const Transport::Callback &callback)
{
    if (headers.isEmpty()) {
        transport.get(url, callback);
        return;
    }
    HttpRequest request;
    request.url = url;
    request.headers = headers;
    transport.send(request, callback);
}

void Transport::download(const DownloadRequest &, const std::function<void(const QString &)> &done)
{
    done(QStringLiteral("this can't fetch files"));
}

void NetworkTransport::download(const DownloadRequest &request, const std::function<void(const QString &)> &done)
{
    if (request.url.scheme() != QLatin1StringView("https") && request.url.host() != QLatin1StringView("127.0.0.1")) {
        done(QStringLiteral("files are only fetched securely"));
        return;
    }
    auto download = std::make_shared<Download>();
    download->request = request;
    download->done = done;
    download->file = std::make_unique<QSaveFile>(request.path);
    if (!download->file->open(QIODevice::WriteOnly)) {
        done(QStringLiteral("it couldn't be saved: %1").arg(download->file->errorString()));
        return;
    }
    fetch(download, request.url);
}

void NetworkTransport::fetch(const std::shared_ptr<Download> &download, const QUrl &url)
{
    QNetworkRequest request(url);
    const QString version = QCoreApplication::applicationVersion();
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Papertrail/%1 (https://github.com/hachigodesu/Papertrail)").arg(version.isEmpty() ? QStringLiteral("dev") : version));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(download->request.timeoutMs);

    QNetworkReply *reply = m_network->get(request);
    const auto stopped = [download] { return download->request.stop && *download->request.stop; };
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, download, stopped] {
        const QByteArray chunk = reply->readAll();
        // Only the file itself is kept, never what a redirect says alongside it.
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || !download->problem.isEmpty())
            return;
        if (stopped()) {
            download->problem = kDownloadStopped;
            reply->abort();
            return;
        }
        download->received += chunk.size();
        if (download->received > download->request.maxBytes) {
            download->problem = QStringLiteral("it was larger than it should be");
            reply->abort();
            return;
        }
        download->hash.addData(chunk);
        if (download->file->write(chunk) != chunk.size()) {
            download->problem = QStringLiteral("it couldn't be saved: %1").arg(download->file->errorString());
            reply->abort();
            return;
        }
        if (download->request.progress)
            download->request.progress(download->received);
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply, download, stopped] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error = download->problem;
        if (error.isEmpty() && stopped())
            error = kDownloadStopped;
        if (error.isEmpty() && status >= 300 && status < 400) {
            const QUrl to = reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (!mayRedirectTo(to, download->request.redirectHosts)) {
                error = QStringLiteral("it was somewhere else, which isn't followed");
            } else if (++download->redirects > kMostRedirects) {
                error = QStringLiteral("it was sent on too many times");
            } else {
                fetch(download, to);
                return;
            }
        }
        if (error.isEmpty() && status != 200)
            error = status > 0 ? QStringLiteral("the server said %1").arg(status) : reply->errorString();
        if (error.isEmpty() && download->hash.result().toHex() != download->request.sha256.toLower())
            error = QStringLiteral("it wasn't the file expected");
        if (!error.isEmpty()) {
            download->file->cancelWriting();
            download->done(error);
            return;
        }
        if (!download->file->commit()) {
            download->done(QStringLiteral("it couldn't be saved: %1").arg(download->file->errorString()));
            return;
        }
        download->done(QString());
    });
}

NetworkTransport::NetworkTransport()
    : m_network(new QNetworkAccessManager)
{
}

NetworkTransport::~NetworkTransport()
{
    delete m_network;
}

bool NetworkTransport::canUseTls()
{
    return QSslSocket::supportsSsl();
}

void NetworkTransport::send(const HttpRequest &http, const Callback &callback)
{
    QNetworkRequest request(http.url);
    const QString version = QCoreApplication::applicationVersion();
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Papertrail/%1 (https://github.com/hachigodesu/Papertrail)").arg(version.isEmpty() ? QStringLiteral("dev") : version));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(http.timeoutMs);
    for (const auto &header : http.headers)
        request.setRawHeader(header.first, header.second);

    QNetworkReply *reply = http.method == "POST" ? m_network->post(request, http.body) : m_network->get(request);
    auto tooBig = std::make_shared<bool>(false);
    const qint64 limit = http.maxReplyBytes;
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, tooBig, limit](qint64 received, qint64) {
        if (received > limit) {
            *tooBig = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback, tooBig] {
        TransportReply result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (*tooBig) {
            result.error = QStringLiteral("the answer was far too large");
        } else {
            result.body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError && result.status == 0)
                result.error = reply->errorString();
        }
        result.retryAfterSeconds = qMax(secondsToWait(reply->rawHeader("Retry-After")), secondsToWait(reply->rawHeader("X-RateLimit-Reset")));
        result.rateLimitPerSecond = reply->rawHeader("X-Rate-Limit-Limit").toInt();
        reply->deleteLater();
        callback(result);
    });
}

void NetworkTransport::get(const QUrl &url, const Callback &callback)
{
    QNetworkRequest request(url);
    // Says what's asking, and nothing about who's using it.
    const QString version = QCoreApplication::applicationVersion();
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Papertrail/%1 (https://github.com/hachigodesu/Papertrail)").arg(version.isEmpty() ? QStringLiteral("dev") : version));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply *reply = m_network->get(request);
    auto tooBig = std::make_shared<bool>(false);
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, tooBig](qint64 received, qint64) {
        if (received > kMaxBodyBytes) {
            *tooBig = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback, tooBig] {
        TransportReply result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (*tooBig) {
            result.error = QStringLiteral("the answer was far too large");
        } else {
            result.body = reply->readAll();
            // A 404 is an answer, not a failure of the network.
            if (reply->error() != QNetworkReply::NoError && result.status == 0)
                result.error = reply->errorString();
        }
        result.retryAfterSeconds = qMax(secondsToWait(reply->rawHeader("Retry-After")), secondsToWait(reply->rawHeader("X-RateLimit-Reset")));
        result.rateLimitPerSecond = reply->rawHeader("X-Rate-Limit-Limit").toInt();
        reply->deleteLater();
        callback(result);
    });
}

} // namespace papertrail::metadata
