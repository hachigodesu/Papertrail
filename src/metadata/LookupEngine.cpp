#include "metadata/LookupEngine.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "metadata/Matching.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <exception>

namespace papertrail::metadata {

using namespace core;

namespace {

constexpr int kFoundCacheDays = 90;
constexpr int kNotFoundCacheDays = 7;
constexpr double kSearchIsGoodEnoughToStop = 0.85;
constexpr double kWorthShowing = 0.5;
constexpr int kMaxCandidates = 3;
constexpr int kMaxAttempts = 4;
constexpr int kAnsweredButBusy = -1;
constexpr int kMinBlockSeconds = 60;

// Slower than each source's published limit, because a library can be very large, and there's no hurry.
constexpr int kCrossrefMs = 250;
constexpr int kCrossrefSearchMs = 1100; // Searches are limited to one a second.
constexpr int kOpenAlexMs = 200;
constexpr int kArxivMs = 3000; // They ask for one request every 3 seconds.
constexpr int kOpenLibraryMs = 1000;

const QString kCrossrefSearchPace = QStringLiteral("crossref-search");

QString providerOf(int ask)
{
    switch (ask) {
    case 0: case 4: return kCrossref;
    case 1: case 5: return kOpenAlex;
    case 2: return kArxiv;
    default: return kOpenLibrary;
    }
}

} // namespace

LookupEngine::LookupEngine(Database &db, Transport &transport, QObject *parent)
    : QObject(parent), m_db(db), m_transport(transport), m_lookups(db)
{
    qRegisterMetaType<LookupSummary>();
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_running)
            next();
    });
}

void LookupEngine::setRetryDelays(int firstMs, int longestMs)
{
    m_firstRetryMs = firstMs;
    m_longestRetryMs = longestMs;
}

void LookupEngine::start()
{
    m_stopping = false;
    if (m_running)
        return;
    m_running = true;
    m_summary = {};
    m_done = 0;
    m_retryMs = 0;
    try {
        m_lookups.recoverInterrupted();
    } catch (const std::exception &) {
        // Those jobs wait until next time.
    }
    next();
}

void LookupEngine::stop()
{
    m_stopping = true;
    m_retryTimer.stop();
    if (!m_running)
        return;
    if (!m_requestInFlight) {
        if (m_job) {
            m_lookups.putBack(m_job->jobId);
            m_job.reset();
        }
        m_running = false;
        m_summary.stopped = true;
        emit finished(m_summary);
    }
    // Otherwise, it finishes when the answer in progress arrives.
}

int LookupEngine::intervalMsFor(const QString &pace) const
{
    int interval = kOpenLibraryMs;
    if (pace == kCrossref)
        interval = kCrossrefMs;
    else if (pace == kCrossrefSearchPace)
        interval = kCrossrefSearchMs;
    else if (pace == kOpenAlex)
        interval = kOpenAlexMs;
    else if (pace == kArxiv)
        interval = kArxivMs;

    // A source can lower its limit at any time, and says so in its answers.
    const int reported = m_reportedRates.value(pace);
    if (reported > 0)
        interval = qMax(interval, 1100 / reported);
    return interval;
}

QUrl LookupEngine::withContact(const QString &provider, const QUrl &url) const
{
    // Only if the user has chosen to give one, and only to the two sources that ask for it.
    if (m_email.isEmpty() || (provider != kCrossref && provider != kOpenAlex))
        return url;
    QUrl contacted = url;
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("mailto"), QString::fromLatin1(QUrl::toPercentEncoding(m_email)));
    contacted.setQuery(query);
    return contacted;
}

void LookupEngine::next()
{
    if (m_stopping) {
        m_running = false;
        m_summary.stopped = m_lookups.pendingCount() > 0;
        emit finished(m_summary);
        return;
    }

    try {
        m_job = m_lookups.nextJob();
        if (!m_job) {
            m_running = false;
            emit finished(m_summary);
            return;
        }
        if (m_job->attempts >= kMaxAttempts) {
            m_lookups.finishJob(m_job->jobId);
            m_job.reset();
            QTimer::singleShot(0, this, &LookupEngine::next);
            return;
        }

        PublicationRepository publications(m_db);
        const std::optional<Publication> publication = publications.get(m_job->publicationId);
        if (!publication) {
            // It's been removed or combined with another since this was queued.
            m_lookups.finishJob(m_job->jobId);
            m_job.reset();
            QTimer::singleShot(0, this, &LookupEngine::next);
            return;
        }
        m_publication = *publication;

        m_fileNames.clear();
        FileRepository files(m_db);
        for (const FileRecord &file : files.filesFor(m_publication.parentId.value_or(m_publication.id)))
            m_fileNames << file.relativePath.section(u'/', -1);

        m_asks.clear();
        m_found.clear();
        m_exact.reset();
        m_anyAnswered = false;
        m_skippedBlocked = false;
        if (!m_publication.doi.isEmpty())
            m_asks << Ask::CrossrefDoi << Ask::OpenAlexDoi;
        if (!m_publication.arxivId.isEmpty())
            m_asks << Ask::Arxiv;
        if (!m_publication.isbn13.isEmpty() || !m_publication.isbn10.isEmpty())
            m_asks << Ask::OpenLibrary;
        // Only worth searching on a title with enough words to tell it from others.
        if (m_publication.title.count(u' ') >= 2)
            m_asks << Ask::CrossrefTitle << Ask::OpenAlexTitle;

        emit progress(m_done, m_lookups.pendingCount(), QString());
        askNext();
    } catch (const std::exception &error) {
        hitProblem(QString::fromUtf8(error.what()), 0);
    }
}

void LookupEngine::askNext()
{
    if (m_exact || m_asks.isEmpty() || (!m_found.isEmpty() && m_found.first().score >= kSearchIsGoodEnoughToStop)) {
        decide();
        return;
    }

    const Ask ask = m_asks.takeFirst();
    const QString provider = providerOf(int(ask));
    QUrl url;
    switch (ask) {
    case Ask::CrossrefDoi: url = crossrefWorkUrl(m_urls, m_publication.doi); break;
    case Ask::OpenAlexDoi: url = openAlexWorkUrl(m_urls, m_publication.doi); break;
    case Ask::Arxiv: url = arxivUrl(m_urls, m_publication.arxivId.section(u'v', 0, 0)); break;
    case Ask::OpenLibrary: url = openLibraryUrl(m_urls, m_publication.isbn13.isEmpty() ? m_publication.isbn10 : m_publication.isbn13); break;
    case Ask::CrossrefTitle: url = crossrefSearchUrl(m_urls, m_publication.title); break;
    case Ask::OpenAlexTitle: url = openAlexSearchUrl(m_urls, m_publication.title); break;
    }
    emit progress(m_done, m_lookups.pendingCount(), provider);
    const QString pace = ask == Ask::CrossrefTitle ? kCrossrefSearchPace : provider;
    fetch(provider, pace, url, [this, ask](int status, const QByteArray &body) { onAnswer(ask, status, body); });
}

void LookupEngine::fetch(const QString &provider, const QString &pace, const QUrl &url, const std::function<void(int, const QByteArray &)> &then)
{
    const QString key = url.toString(QUrl::FullyEncoded);
    if (m_blockedUntil.value(provider) > QDateTime::currentMSecsSinceEpoch()) {
        QTimer::singleShot(0, this, [then] { then(kAnsweredButBusy, {}); });
        return;
    }
    try {
        // A lookup asked for by hand doesn't believe an earlier "not found".
        const bool freshNotFound = m_job && m_job->fresh;
        std::optional<LookupRepository::CachedResponse> stored = m_lookups.cached(key, kFoundCacheDays);
        if (stored && stored->status != 200 && (freshNotFound || !m_lookups.cached(key, kNotFoundCacheDays)))
            stored.reset();
        if (stored) {
            const int status = stored->status;
            const QByteArray body = stored->body;
            QTimer::singleShot(0, this, [then, status, body] { then(status, body); });
            return;
        }
    } catch (const std::exception &) {
        // No cache is only a slower way of doing the same thing.
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait = qMax<qint64>(0, m_nextAllowed.value(pace) - now);
    m_nextAllowed[pace] = now + wait + intervalMsFor(pace);
    m_requestInFlight = true;
    QTimer::singleShot(int(wait), this, [this, provider, pace, url, key, then] {
        m_transport.get(withContact(provider, url), [this, provider, pace, key, then](const TransportReply &reply) {
            m_requestInFlight = false;
            if (reply.rateLimitPerSecond > 0)
                m_reportedRates[pace] = reply.rateLimitPerSecond;
            if (!reply.error.isEmpty()) {
                hitProblem(tr("Can't reach %1: %2").arg(providerLabel(provider), reply.error), 0);
                return;
            }
            if (reply.status == 429) {
                // Out of its allowance, which could be for hours. Others may still be able to answer.
                m_blockedUntil[provider] = QDateTime::currentMSecsSinceEpoch() + qMax(kMinBlockSeconds, reply.retryAfterSeconds) * 1000;
                then(kAnsweredButBusy, {});
                return;
            }
            if (reply.status >= 500) {
                hitProblem(tr("%1 is having problems right now.").arg(providerLabel(provider)), reply.retryAfterSeconds);
                return;
            }
            m_retryMs = 0;
            try {
                if (reply.status == 200 || reply.status == 404)
                    m_lookups.store(key, reply.status, reply.status == 200 ? reply.body : QByteArray());
            } catch (const std::exception &) {
                // As above: it just means asking again next time.
            }
            then(reply.status, reply.body);
        });
    });
}

void LookupEngine::onAnswer(Ask ask, int status, const QByteArray &body)
{
    if (m_stopping) {
        if (m_job) {
            m_lookups.putBack(m_job->jobId);
            m_job.reset();
        }
        next();
        return;
    }

    if (status == kAnsweredButBusy)
        m_skippedBlocked = true;
    else
        m_anyAnswered = true;

    if (status == 200) {
        std::optional<FoundRecord> exact;
        QList<FoundRecord> results;
        QString matchedBy;
        switch (ask) {
        case Ask::CrossrefDoi: exact = parseCrossrefWork(body); matchedBy = QStringLiteral("doi"); break;
        case Ask::OpenAlexDoi: exact = parseOpenAlexWork(body); matchedBy = QStringLiteral("doi"); break;
        case Ask::Arxiv: exact = parseArxiv(body); matchedBy = QStringLiteral("arxiv"); break;
        case Ask::OpenLibrary: exact = parseOpenLibrary(body); matchedBy = QStringLiteral("isbn"); break;
        case Ask::CrossrefTitle: results = parseCrossrefSearch(body); break;
        case Ask::OpenAlexTitle: results = parseOpenAlexSearch(body); break;
        }
        if (exact) {
            exact->matchedBy = matchedBy;
            m_exact = exact;
        }
        for (FoundRecord &record : results) {
            record.matchedBy = QStringLiteral("title");
            record.score = matchScore(m_publication, record);
            if (record.score >= kWorthShowing)
                m_found << record;
        }
        std::stable_sort(m_found.begin(), m_found.end(),
                         [](const FoundRecord &a, const FoundRecord &b) { return a.score > b.score; });
    }
    askNext();
}

// What happens to a result, for each mode:
//
//                           Online with review     Online, no review
//   Identifier, title fits  Applied                Applied
//   Identifier, title not   Waits for review       Applied, and marked for checking
//   Search result           Waits for review       Best one applied, and marked for checking
//
// Offline mode never gets here, because it never queues anything.
void LookupEngine::decide()
{
    // Nobody could answer, which isn't the same as nobody knowing. It stays queued for later.
    if (!m_exact && m_found.isEmpty() && m_skippedBlocked && !m_anyAnswered) {
        qint64 soonest = 0;
        for (const qint64 until : std::as_const(m_blockedUntil))
            soonest = soonest == 0 ? until : qMin(soonest, until);
        const qint64 wait = qMax<qint64>(0, soonest - QDateTime::currentMSecsSinceEpoch());
        hitProblem(tr("The online sources have asked Papertrail to wait."), int(wait / 1000) + 1);
        return;
    }

    const bool automatic = m_job->mode == QLatin1StringView("online_auto");
    try {
        Transaction transaction(m_db);
        bool waiting = false;
        bool changed = false;
        if (m_exact) {
            const bool fits = titleAgrees(m_publication, m_fileNames, *m_exact);
            if (fits || automatic) {
                // A match that fits settles any doubt about the details. One that doesn't raises some.
                applyFoundRecord(m_db, m_publication.id, *m_exact, !fits);
                changed = true;
            } else {
                LookupCandidate candidate;
                candidate.publicationId = m_publication.id;
                candidate.provider = m_exact->provider;
                candidate.matchedBy = m_exact->matchedBy;
                candidate.score = titleSimilarity(m_publication.title, m_exact->publication.title);
                candidate.recordJson = QString::fromUtf8(QJsonDocument(m_exact->toJson()).toJson(QJsonDocument::Compact));
                m_lookups.resolveCandidates(m_publication.id);
                m_lookups.addCandidate(candidate);
                waiting = true;
            }
        } else if (!m_found.isEmpty()) {
            if (automatic) {
                applyFoundRecord(m_db, m_publication.id, m_found.first(), true);
                changed = true;
            } else {
                m_lookups.resolveCandidates(m_publication.id);
                for (int i = 0; i < m_found.size() && i < kMaxCandidates; ++i) {
                    LookupCandidate candidate;
                    candidate.publicationId = m_publication.id;
                    candidate.provider = m_found.at(i).provider;
                    candidate.matchedBy = m_found.at(i).matchedBy;
                    candidate.score = m_found.at(i).score;
                    candidate.recordJson = QString::fromUtf8(QJsonDocument(m_found.at(i).toJson()).toJson(QJsonDocument::Compact));
                    m_lookups.addCandidate(candidate);
                }
                waiting = true;
            }
        }
        m_lookups.finishJob(m_job->jobId);
        transaction.commit();

        const qint64 id = m_publication.id;
        if (changed) {
            ++m_summary.applied;
            emit publicationChanged(id);
        } else if (waiting) {
            ++m_summary.forReview;
            emit reviewChanged();
        } else {
            ++m_summary.notFound;
        }
    } catch (const std::exception &error) {
        hitProblem(QString::fromUtf8(error.what()), 0);
        return;
    }
    m_job.reset();
    ++m_done;
    emit progress(m_done, m_lookups.pendingCount(), QString());
    QTimer::singleShot(0, this, &LookupEngine::next);
}

void LookupEngine::hitProblem(const QString &message, int retryAfterSeconds)
{
    m_requestInFlight = false;
    if (m_job) {
        try {
            m_lookups.putBack(m_job->jobId);
        } catch (const std::exception &) {
            // It'll be treated as interrupted next time.
        }
        m_job.reset();
    }
    emit problem(message);
    if (m_stopping) {
        next();
        return;
    }
    // Waits longer each time, and for as long as a source asks if that's longer.
    m_retryMs = m_retryMs == 0 ? m_firstRetryMs : qMin(m_retryMs * 2, m_longestRetryMs);
    m_retryTimer.start(qMax(m_retryMs, retryAfterSeconds * 1000));
}

} // namespace papertrail::metadata
