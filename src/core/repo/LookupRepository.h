#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

struct LookupJob {
    qint64 jobId = 0;
    qint64 publicationId = 0;
    QString mode;     // "online_review" or "online_auto".
    bool fresh = false; // Asked for by hand, so an earlier "not found" doesn't count.
    int attempts = 0;
};

struct LookupCandidate {
    qint64 id = 0;
    qint64 publicationId = 0;
    QString provider;
    QString matchedBy; // "doi", "isbn", "arxiv" or "title".
    double score = 0;
    QString recordJson;
};

// The persistent side of online lookups: what's waiting, what's been fetched, and what's waiting for
// the user to decide. Offline mode never adds to this, so nothing here can start a network request.
class LookupRepository {
public:
    explicit LookupRepository(Database &db) : m_db(db) {}

    void enqueue(const QList<qint64> &publicationIds, const QString &mode, bool fresh = false);
    std::optional<LookupJob> nextJob();
    void finishJob(qint64 jobId);
    void putBack(qint64 jobId); // To try again later, as though it hadn't been tried.
    void recoverInterrupted(); // A job left running was interrupted when the app last stopped.
    qint64 pendingCount();
    void clearPending();

    // A stored response, if there's one newer than this many days.
    struct CachedResponse {
        int status = 0;
        QByteArray body;
    };
    std::optional<CachedResponse> cached(const QString &key, int maxAgeDays);
    void store(const QString &key, int status, const QByteArray &body);

    void addCandidate(const LookupCandidate &candidate);
    QList<LookupCandidate> candidatesFor(qint64 publicationId);
    QList<qint64> publicationsWaitingForReview();
    qint64 waitingForReviewCount();
    void resolveCandidates(qint64 publicationId);

private:
    Database &m_db;
};

} // namespace papertrail::core
