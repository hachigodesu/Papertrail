#pragma once

#include <QByteArray>
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

// The only way anything in Papertrail reaches the network. Offline mode never queues any work that
// would call this, and tests swap it to prove that.
class Transport {
public:
    virtual ~Transport() = default;
    using Callback = std::function<void(const TransportReply &)>;
    virtual void get(const QUrl &url, const Callback &callback) = 0;
};

class NetworkTransport : public Transport {
public:
    NetworkTransport();
    ~NetworkTransport() override;

    void get(const QUrl &url, const Callback &callback) override;

    // Whether this build can make secure connections at all.
    static bool canUseTls();

private:
    QNetworkAccessManager *m_network;
};

} // namespace papertrail::metadata
