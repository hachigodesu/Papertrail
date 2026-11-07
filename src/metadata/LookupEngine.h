#pragma once

#include "core/repo/LookupRepository.h"
#include "metadata/Providers.h"
#include "metadata/Transport.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::metadata {

struct LookupSummary {
    int applied = 0;       // Filled in, automatically.
    int forReview = 0;     // Waiting for the user to decide.
    int notFound = 0;
    bool stopped = false;  // Stopped early. What's left carries on next time.
};

// Works through the lookups queued in the database, one at a time, at a pace each source allows.
// The rules for what happens to each result are in decide(). This never parses a document, and only
// sends identifiers and titles: never authors, and nothing from inside a file.
class LookupEngine : public QObject {
    Q_OBJECT

public:
    LookupEngine(core::Database &db, Transport &transport, QObject *parent = nullptr);

    void setProviderUrls(const ProviderUrls &urls) { m_urls = urls; }
    void setContactEmail(const QString &email) { m_email = email; }
    void setRetryDelays(int firstMs, int longestMs);

    // Starts working through the queue. Doing this again while it's running changes nothing.
    void start();
    void stop();
    bool isRunning() const { return m_running; }

signals:
    void progress(qint64 done, qint64 remaining, const QString &source);

    // A publication's details have changed.
    void publicationChanged(qint64 publicationId);

    // Something has been added to what's waiting for review.
    void reviewChanged();

    // Finished, or stopped: nothing further is happening.
    void finished(const papertrail::metadata::LookupSummary &summary);

    // Can't reach a source right now. It'll try again by itself.
    void problem(const QString &message);

private:
    enum class Ask { CrossrefDoi, OpenAlexDoi, Arxiv, OpenLibrary, CrossrefTitle, OpenAlexTitle };

    void next();
    void askNext();
    void fetch(const QString &provider, const QString &pace, const QUrl &url, const std::function<void(int, const QByteArray &)> &then);
    void onAnswer(Ask ask, int status, const QByteArray &body);
    void decide();
    void finishCurrent();
    void hitProblem(const QString &message, int retryAfterSeconds);
    int intervalMsFor(const QString &pace) const;
    QUrl withContact(const QString &provider, const QUrl &url) const;

    core::Database &m_db;
    Transport &m_transport;
    core::LookupRepository m_lookups;
    ProviderUrls m_urls;
    QString m_email;

    bool m_running = false;
    bool m_stopping = false;
    bool m_requestInFlight = false;
    qint64 m_done = 0;
    LookupSummary m_summary;

    std::optional<core::LookupJob> m_job;
    core::Publication m_publication;
    QStringList m_fileNames;
    QList<Ask> m_asks;
    QList<FoundRecord> m_found; // From searching. Scored, and best first.
    std::optional<FoundRecord> m_exact;

    // By what each source paces separately: for Crossref, single works and searches differ.
    QHash<QString, qint64> m_nextAllowed;
    QHash<QString, qint64> m_blockedUntil; // A source that's said it has had enough for now.
    bool m_anyAnswered = false;
    bool m_skippedBlocked = false;
    QHash<QString, int> m_reportedRates;
    QTimer m_retryTimer;
    int m_firstRetryMs = 60000;
    int m_longestRetryMs = 15 * 60000;
    int m_retryMs = 0;
};

} // namespace papertrail::metadata

Q_DECLARE_METATYPE(papertrail::metadata::LookupSummary)
