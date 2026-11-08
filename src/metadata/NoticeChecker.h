#pragma once

#include "metadata/Providers.h"
#include "metadata/Transport.h"

#include <QList>
#include <QObject>
#include <QString>

namespace papertrail::core {
class Database;
}

namespace papertrail::metadata {

// Asks Crossref, now and then, whether anything in the library has been retracted, corrected, or had concerns
// raised about it. Only DOIs are sent, several to a request, and each publication is asked about again only after
// a while. It's only ever started when online lookups are allowed; it doesn't check that itself.
class NoticeChecker : public QObject {
    Q_OBJECT

public:
    NoticeChecker(core::Database &db, Transport &transport, QObject *parent = nullptr);

    void setProviderUrls(const ProviderUrls &urls) { m_urls = urls; }
    void setContactEmail(const QString &email) { m_email = email; }
    void setRecheckAfterDays(int days) { m_recheckDays = days; }
    void setBatchSize(int size) { m_batchSize = size; }
    void setIntervalMs(int ms) { m_intervalMs = ms; }

    // Checks everything that's due, a batch at a time. Doing this again while it's running changes nothing.
    void start();
    void stop();
    bool isRunning() const { return m_running; }

    static constexpr int kRecheckAfterDays = 30;

signals:
    // These publications' notices have changed.
    void noticesChanged(const QList<qint64> &publicationIds);
    // Finished or stopped. `newWarnings` is how many publications now have a warning that didn't before.
    void finished(int checked, int newWarnings);
    // Crossref couldn't be asked. What's left is checked next time.
    void problem(const QString &message);

private:
    void next();
    void finish();

    core::Database &m_db;
    Transport &m_transport;
    ProviderUrls m_urls;
    QString m_email;
    int m_recheckDays = kRecheckAfterDays;
    int m_batchSize = 20;
    int m_intervalMs = 1000;
    bool m_running = false;
    bool m_stopping = false;
    int m_checked = 0;
    int m_newWarnings = 0;
    qint64 m_startedAt = 0;
};

} // namespace papertrail::metadata
