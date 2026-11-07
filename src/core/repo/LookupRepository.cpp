#include "core/repo/LookupRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

namespace papertrail::core {

namespace {

constexpr qint64 kDayMs = 24 * 60 * 60 * 1000;

} // namespace

void LookupRepository::enqueue(const QList<qint64> &publicationIds, const QString &mode, bool fresh)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction transaction(m_db);
    auto insert = m_db.prepare(
        "INSERT OR IGNORE INTO job (kind, payload, created_at, updated_at) VALUES ('lookup', ?, ?, ?)");
    for (const qint64 id : publicationIds) {
        QJsonObject payload{{QStringLiteral("publication"), id}, {QStringLiteral("mode"), mode}};
        if (fresh)
            payload.insert(QStringLiteral("fresh"), true);
        insert->reset();
        insert->bind(1, QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        insert->bind(2, now);
        insert->bind(3, now);
        insert->step();
    }
    transaction.commit();
}

std::optional<LookupJob> LookupRepository::nextJob()
{
    auto select = m_db.prepare(
        "SELECT id, payload, attempts FROM job WHERE kind = 'lookup' AND status = 'pending' ORDER BY id LIMIT 1");
    if (!select->step())
        return std::nullopt;
    LookupJob job;
    job.jobId = select->int64At(0);
    const QJsonObject payload = QJsonDocument::fromJson(select->textAt(1).toUtf8()).object();
    job.publicationId = payload.value(QStringLiteral("publication")).toInteger();
    job.mode = payload.value(QStringLiteral("mode")).toString();
    job.fresh = payload.value(QStringLiteral("fresh")).toBool();
    job.attempts = select->intAt(2);
    select.reset();

    auto claim = m_db.prepare("UPDATE job SET status = 'running', attempts = attempts + 1, updated_at = ? WHERE id = ?");
    claim->bind(1, QDateTime::currentMSecsSinceEpoch());
    claim->bind(2, job.jobId);
    claim->step();
    return job;
}

void LookupRepository::finishJob(qint64 jobId)
{
    auto remove = m_db.prepare("DELETE FROM job WHERE id = ?");
    remove->bind(1, jobId);
    remove->step();
}

void LookupRepository::recoverInterrupted()
{
    m_db.exec("UPDATE job SET status = 'pending', attempts = max(attempts - 1, 0) WHERE kind = 'lookup' AND status = 'running'");
}

void LookupRepository::putBack(qint64 jobId)
{
    auto update = m_db.prepare(
        "UPDATE job SET status = 'pending', attempts = max(attempts - 1, 0), updated_at = ? WHERE id = ?");
    update->bind(1, QDateTime::currentMSecsSinceEpoch());
    update->bind(2, jobId);
    update->step();
}

qint64 LookupRepository::pendingCount()
{
    auto select = m_db.prepare("SELECT count(*) FROM job WHERE kind = 'lookup' AND status IN ('pending', 'running')");
    select->step();
    return select->int64At(0);
}

void LookupRepository::clearPending()
{
    m_db.exec("DELETE FROM job WHERE kind = 'lookup'");
}

std::optional<LookupRepository::CachedResponse> LookupRepository::cached(const QString &key, int maxAgeDays)
{
    auto select = m_db.prepare("SELECT status, body FROM lookup_cache WHERE key = ? AND fetched_at >= ?");
    select->bind(1, key);
    select->bind(2, QDateTime::currentMSecsSinceEpoch() - maxAgeDays * kDayMs);
    if (!select->step())
        return std::nullopt;
    return CachedResponse{select->intAt(0), select->textAt(1).toUtf8()};
}

void LookupRepository::store(const QString &key, int status, const QByteArray &body)
{
    auto upsert = m_db.prepare(
        "INSERT INTO lookup_cache (key, status, body, fetched_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (key) DO UPDATE SET status = excluded.status, body = excluded.body, fetched_at = excluded.fetched_at");
    upsert->bind(1, key);
    upsert->bind(2, status);
    // Not found has no body, and a null one wouldn't be stored at all.
    upsert->bind(3, body.isEmpty() ? QStringLiteral("") : QString::fromUtf8(body));
    upsert->bind(4, QDateTime::currentMSecsSinceEpoch());
    upsert->step();
}

void LookupRepository::addCandidate(const LookupCandidate &candidate)
{
    auto insert = m_db.prepare(
        "INSERT INTO lookup_candidate (publication_id, provider, matched_by, score, record, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    insert->bind(1, candidate.publicationId);
    insert->bind(2, candidate.provider);
    insert->bind(3, candidate.matchedBy);
    insert->bind(4, candidate.score);
    insert->bind(5, candidate.recordJson);
    insert->bind(6, QDateTime::currentMSecsSinceEpoch());
    insert->step();
}

QList<LookupCandidate> LookupRepository::candidatesFor(qint64 publicationId)
{
    auto select = m_db.prepare(
        "SELECT id, provider, matched_by, score, record FROM lookup_candidate "
        "WHERE publication_id = ? ORDER BY score DESC, id");
    select->bind(1, publicationId);
    QList<LookupCandidate> candidates;
    while (select->step()) {
        LookupCandidate candidate;
        candidate.id = select->int64At(0);
        candidate.publicationId = publicationId;
        candidate.provider = select->textAt(1);
        candidate.matchedBy = select->textAt(2);
        candidate.score = select->doubleAt(3);
        candidate.recordJson = select->textAt(4);
        candidates << candidate;
    }
    return candidates;
}

QList<qint64> LookupRepository::publicationsWaitingForReview()
{
    auto select = m_db.prepare(
        "SELECT DISTINCT publication_id FROM lookup_candidate "
        "WHERE publication_id NOT IN (SELECT publication_id FROM hidden_publication) ORDER BY publication_id");
    QList<qint64> ids;
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

qint64 LookupRepository::waitingForReviewCount()
{
    auto select = m_db.prepare(
        "SELECT count(DISTINCT publication_id) FROM lookup_candidate "
        "WHERE publication_id NOT IN (SELECT publication_id FROM hidden_publication)");
    select->step();
    return select->int64At(0);
}

void LookupRepository::resolveCandidates(qint64 publicationId)
{
    auto remove = m_db.prepare("DELETE FROM lookup_candidate WHERE publication_id = ?");
    remove->bind(1, publicationId);
    remove->step();
}

} // namespace papertrail::core
