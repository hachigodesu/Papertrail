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

} // namespace

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

void NetworkTransport::download(const DownloadRequest &download, const std::function<void(const QString &)> &done)
{
    if (download.url.scheme() != QLatin1StringView("https") && download.url.host() != QLatin1StringView("127.0.0.1")) {
        done(QStringLiteral("files are only fetched securely"));
        return;
    }
    auto file = std::make_shared<QSaveFile>(download.path);
    if (!file->open(QIODevice::WriteOnly)) {
        done(QStringLiteral("it couldn't be saved: %1").arg(file->errorString()));
        return;
    }
    QNetworkRequest request(download.url);
    const QString version = QCoreApplication::applicationVersion();
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Papertrail/%1 (https://github.com/hachigodesu/Papertrail)").arg(version.isEmpty() ? QStringLiteral("dev") : version));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(download.timeoutMs);

    QNetworkReply *reply = m_network->get(request);
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto received = std::make_shared<qint64>(0);
    auto problem = std::make_shared<QString>();
    const qint64 limit = download.maxBytes;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, file, hash, received, problem, limit] {
        const QByteArray chunk = reply->readAll();
        *received += chunk.size();
        if (*received > limit) {
            *problem = QStringLiteral("it was larger than it should be");
            reply->abort();
            return;
        }
        hash->addData(chunk);
        if (file->write(chunk) != chunk.size()) {
            *problem = QStringLiteral("it couldn't be saved: %1").arg(file->errorString());
            reply->abort();
        }
    });
    const QByteArray expected = download.sha256.toLower();
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, file, hash, problem, expected, done] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error = *problem;
        if (error.isEmpty() && status != 200)
            error = status >= 300 && status < 400 ? QStringLiteral("it was somewhere else, which isn't followed")
                  : status > 0                    ? QStringLiteral("the server said %1").arg(status)
                                                  : reply->errorString();
        if (error.isEmpty() && hash->result().toHex() != expected)
            error = QStringLiteral("it wasn't the file expected");
        reply->deleteLater();
        if (!error.isEmpty()) {
            file->cancelWriting();
            done(error);
            return;
        }
        if (!file->commit()) {
            done(QStringLiteral("it couldn't be saved: %1").arg(file->errorString()));
            return;
        }
        done(QString());
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
