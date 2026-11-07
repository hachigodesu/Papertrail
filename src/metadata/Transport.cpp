#include "metadata/Transport.h"

#include <QCoreApplication>
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
