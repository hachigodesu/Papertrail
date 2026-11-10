#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

namespace papertrail::metadata {

struct TransportReply {
    int status = 0;      // The HTTP status, or 0 if there wasn't one.
    QByteArray body;
    QString error;       // Set if the network itself failed.
    int retryAfterSeconds = 0; // From either Retry-After, or a quota's reset time.
    int rateLimitPerSecond = 0; // What the source says its current limit is, if it says.
};

// For requests with headers of their own, which at the moment means article summaries. These don't follow redirects,
// because a redirect would carry their headers somewhere else.
struct HttpRequest {
    QUrl url;
    QByteArray method = "GET";
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int timeoutMs = 30000;
    qint64 maxReplyBytes = 8 * 1024 * 1024;
};

// A file to fetch and keep, such as a language's model for recognising text. It's written to a temporary file as it
// arrives, and only kept at `path` if it's exactly `sha256`, no larger than `maxBytes`. Redirects aren't followed.
struct DownloadRequest {
    QUrl url;
    QString path;
    qint64 maxBytes = 0;
    QByteArray sha256; // In hex.
    int timeoutMs = 120000; // Without anything arriving.
};

// The only way anything in Papertrail reaches the network. Offline mode never queues any work that
// would call this, and tests swap it to prove that.
class Transport {
public:
    virtual ~Transport() = default;
    using Callback = std::function<void(const TransportReply &)>;
    virtual void get(const QUrl &url, const Callback &callback) = 0;
    virtual void send(const HttpRequest &request, const Callback &callback);
    // Calls `done` with an empty string once the file is kept, or what went wrong.
    virtual void download(const DownloadRequest &request, const std::function<void(const QString &error)> &done);
};

class NetworkTransport : public Transport {
public:
    NetworkTransport();
    ~NetworkTransport() override;

    void get(const QUrl &url, const Callback &callback) override;
    void send(const HttpRequest &request, const Callback &callback) override;
    void download(const DownloadRequest &request, const std::function<void(const QString &error)> &done) override;

    // Whether this build can make secure connections at all.
    static bool canUseTls();

private:
    QNetworkAccessManager *m_network;
};

} // namespace papertrail::metadata
