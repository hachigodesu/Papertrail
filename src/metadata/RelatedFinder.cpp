#include "metadata/RelatedFinder.h"

#include "core/db/Database.h"
#include "core/model/Identifiers.h"
#include "core/repo/PublicationRepository.h"
#include "metadata/Matching.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>

namespace papertrail::metadata {

using namespace core;

namespace {

constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;
constexpr int kPerPage = 50;

// Only what the list needs, and what a publication added from it is made from.
const QString kListFields = QStringLiteral("id,doi,title,publication_year,language,authorships,primary_location,biblio,ids,"
                                           "abstract_inverted_index,type,cited_by_count");

// "https://openalex.org/W123" or "W123" as just "W123", or empty if it's anything else.
QString workId(const QString &value)
{
    const QString id = value.section(u'/', -1);
    return isOpenAlexWorkId(id) ? id : QString();
}

QUrl listUrl(const ProviderUrls &urls, const QString &filter, bool byCitations)
{
    QUrl url(urls.openAlex + QStringLiteral("/works"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("filter"), filter);
    if (byCitations)
        query.addQueryItem(QStringLiteral("sort"), QStringLiteral("cited_by_count:desc"));
    query.addQueryItem(QStringLiteral("per-page"), QString::number(kPerPage));
    query.addQueryItem(QStringLiteral("select"), kListFields);
    url.setQuery(query);
    return url;
}

} // namespace

bool isOpenAlexWorkId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^W[0-9]{1,15}$"));
    return pattern.match(id).hasMatch();
}

QUrl openAlexRelatedUrl(const ProviderUrls &urls, const QString &doi)
{
    QUrl url = openAlexWorkUrl(urls, doi);
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("select"), QStringLiteral("id,doi,cited_by_count,referenced_works"));
    url.setQuery(query);
    return url;
}

QUrl openAlexWorksByIdUrl(const ProviderUrls &urls, const QStringList &ids)
{
    return listUrl(urls, QStringLiteral("openalex:") + ids.join(u'|'), false);
}

QUrl openAlexCitingUrl(const ProviderUrls &urls, const QString &id)
{
    return listUrl(urls, QStringLiteral("cites:") + id, true);
}

std::optional<OpenAlexWorkLinks> parseOpenAlexWorkLinks(const QByteArray &body)
{
    const QJsonObject work = QJsonDocument::fromJson(body).object();
    OpenAlexWorkLinks links;
    links.id = workId(work.value(QStringLiteral("id")).toString());
    if (links.id.isEmpty())
        return std::nullopt;
    links.citedByCount = qMax(0, work.value(QStringLiteral("cited_by_count")).toInt());
    for (const QJsonValue &reference : work.value(QStringLiteral("referenced_works")).toArray()) {
        const QString id = workId(reference.toString());
        if (!id.isEmpty() && !links.references.contains(id))
            links.references << id;
    }
    return links;
}

QList<RelatedWork> parseOpenAlexWorkList(const QByteArray &body, int *total)
{
    const QJsonObject reply = QJsonDocument::fromJson(body).object();
    if (total)
        *total = qMax(0, reply.value(QStringLiteral("meta")).toObject().value(QStringLiteral("count")).toInt());
    QList<RelatedWork> works;
    for (const QJsonValue &item : reply.value(QStringLiteral("results")).toArray()) {
        const QJsonObject work = item.toObject();
        RelatedWork related;
        related.openAlexId = workId(work.value(QStringLiteral("id")).toString());
        if (related.openAlexId.isEmpty())
            continue;
        related.citedByCount = qMax(0, work.value(QStringLiteral("cited_by_count")).toInt());
        related.record = openAlexRecord(work);
        if (related.record.publication.title.isEmpty())
            continue;
        works << related;
    }
    return works;
}

RelatedPapers relatedPapersFor(Database &db, qint64 publicationId)
{
    RelatedPapers papers;
    {
        auto select = db.prepare("SELECT doi, reference_total, cited_by_total, fetched_at FROM related_fetch WHERE publication_id = ?");
        select->bind(1, publicationId);
        if (!select->step())
            return papers;
        papers.fetched = true;
        papers.doi = select->textAt(0);
        papers.referenceTotal = select->intAt(1);
        papers.citedByTotal = select->intAt(2);
        papers.fetchedAt = select->int64At(3);
    }
    auto select = db.prepare("SELECT relation, openalex_id, cited_by_count, record FROM related_work WHERE publication_id = ? "
                             "ORDER BY relation, position");
    select->bind(1, publicationId);
    while (select->step()) {
        RelatedWork work;
        work.openAlexId = select->textAt(1);
        work.citedByCount = select->intAt(2);
        work.record = FoundRecord::fromJson(QJsonDocument::fromJson(select->textAt(3).toUtf8()).object());
        // Hidden publications, such as those kept for a removed folder, don't count as being in the library.
        std::optional<qint64> found = publicationWithIdentifier(db, work.record.publication);
        if (!found) {
            if (const auto byTitle = publicationWithTitle(db, work.record.publication); byTitle && byTitle->second)
                found = byTitle->first;
        }
        if (found) {
            auto hidden = db.prepare("SELECT 1 FROM hidden_publication WHERE publication_id = ?");
            hidden->bind(1, *found);
            if (!hidden->step() && *found != publicationId)
                work.inLibrary = found;
        }
        (select->textAt(0) == QLatin1StringView("reference") ? papers.references : papers.citedBy) << work;
    }
    return papers;
}

qint64 addRelatedToLibrary(Database &db, const FoundRecord &record)
{
    Publication publication = record.publication;
    publication.type = record.type.value_or(PublicationType::JournalArticle);
    // Only ever marked as a preprint by the user, as with lookups. The source's word is kept for them to see.
    if (publication.type == PublicationType::Preprint)
        publication.type = PublicationType::Other;
    if (record.preprintHint)
        publication.extraJson = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("preprintHint"), kOpenAlex}}).toJson(QJsonDocument::Compact));
    Transaction transaction(db);
    PublicationRepository publications(db);
    const qint64 id = publications.create(publication);
    publications.recordProvenance(id, {QStringLiteral("added"), kOpenAlex, std::nullopt, QString(), publication.title, 0});
    transaction.commit();
    return id;
}

RelatedFinder::RelatedFinder(Database &db, Transport &transport, QObject *parent)
    : QObject(parent), m_db(db), m_transport(transport)
{
}

void RelatedFinder::find(qint64 publicationId, bool refresh)
{
    ++m_generation;
    m_publicationId = publicationId;
    m_references.clear();
    m_citedBy.clear();
    m_citedByTotal = 0;
    m_links = {};
    try {
        const std::optional<Publication> publication = PublicationRepository(m_db).get(publicationId);
        m_doi = publication ? normaliseDoi(publication->doi) : QString();
        if (m_doi.isEmpty()) {
            fail(RelatedProblem::NoDoi);
            return;
        }
        if (!refresh) {
            const RelatedPapers kept = relatedPapersFor(m_db, publicationId);
            if (kept.fetched && kept.doi == m_doi && QDateTime::currentMSecsSinceEpoch() - kept.fetchedAt < kKeepDays * kDayMs) {
                emit found(publicationId);
                return;
            }
        }
    } catch (const std::exception &error) {
        fail(RelatedProblem::Network, QString::fromUtf8(error.what()));
        return;
    }

    ask(openAlexRelatedUrl(m_urls, m_doi), [this](const TransportReply &reply) {
        if (reply.status != 200) {
            failFromReply(reply);
            return;
        }
        const std::optional<OpenAlexWorkLinks> links = parseOpenAlexWorkLinks(reply.body);
        if (!links) {
            fail(RelatedProblem::Unreadable);
            return;
        }
        m_links = *links;
        m_links.references = m_links.references.mid(0, kMostReferences);
        askForReferences(0);
    });
}

void RelatedFinder::askForReferences(int from)
{
    if (from >= m_links.references.size()) {
        // Then the most cited of what cites it, which is all a list can show.
        if (m_links.citedByCount == 0) {
            finish();
            return;
        }
        ask(openAlexCitingUrl(m_urls, m_links.id), [this](const TransportReply &reply) {
            if (reply.status != 200) {
                failFromReply(reply);
                return;
            }
            m_citedBy = parseOpenAlexWorkList(reply.body, &m_citedByTotal);
            finish();
        });
        return;
    }
    const QStringList batch = m_links.references.mid(from, kPerPage);
    ask(openAlexWorksByIdUrl(m_urls, batch), [this, from, batch](const TransportReply &reply) {
        if (reply.status != 200) {
            failFromReply(reply);
            return;
        }
        // In the order the paper cites them, which a list doesn't keep.
        QHash<QString, RelatedWork> byId;
        for (const RelatedWork &work : parseOpenAlexWorkList(reply.body))
            byId.insert(work.openAlexId, work);
        for (const QString &id : batch) {
            if (byId.contains(id))
                m_references << byId.value(id);
        }
        askForReferences(from + kPerPage);
    });
}

void RelatedFinder::ask(const QUrl &url, const std::function<void(const TransportReply &)> &then)
{
    QUrl asked = url;
    // Only if the user has chosen to give one, as for lookups.
    if (!m_email.isEmpty()) {
        QUrlQuery query(asked);
        query.addQueryItem(QStringLiteral("mailto"), QString::fromLatin1(QUrl::toPercentEncoding(m_email)));
        asked.setQuery(query);
    }
    const quint64 generation = m_generation;
    const auto headers = keyHeaders(kOpenAlex, m_key);
    getWithHeaders(m_transport, asked, headers, [this, generation, url, then, keyed = !headers.isEmpty()](const TransportReply &reply) {
        if (generation != m_generation)
            return;
        if (keyed && (reply.status == 401 || reply.status == 403)) {
            // A key that's wrong, or not yet activated. OpenAlex still answers without one.
            m_key.clear();
            emit keyRefused();
            ask(url, then);
            return;
        }
        then(reply);
    });
}

void RelatedFinder::finish()
{
    try {
        Transaction transaction(m_db);
        auto clear = m_db.prepare("DELETE FROM related_work WHERE publication_id = ?");
        clear->bind(1, m_publicationId);
        clear->step();
        auto fetch = m_db.prepare("INSERT OR REPLACE INTO related_fetch (publication_id, doi, openalex_id, reference_total, "
                                  "cited_by_total, fetched_at) VALUES (?, ?, ?, ?, ?, ?)");
        fetch->bind(1, m_publicationId);
        fetch->bind(2, m_doi);
        fetch->bind(3, m_links.id);
        fetch->bind(4, int(m_links.references.size()));
        fetch->bind(5, qMax(m_citedByTotal, m_links.citedByCount));
        fetch->bind(6, QDateTime::currentMSecsSinceEpoch());
        fetch->step();
        auto insert = m_db.prepare("INSERT INTO related_work (publication_id, relation, position, openalex_id, cited_by_count, "
                                   "record) VALUES (?, ?, ?, ?, ?, ?)");
        const auto keep = [&](const QList<RelatedWork> &works, const char *relation) {
            for (qsizetype i = 0; i < works.size(); ++i) {
                insert->bind(1, m_publicationId);
                insert->bind(2, relation);
                insert->bind(3, int(i));
                insert->bind(4, works.at(i).openAlexId);
                insert->bind(5, works.at(i).citedByCount);
                insert->bind(6, QString::fromUtf8(QJsonDocument(works.at(i).record.toJson()).toJson(QJsonDocument::Compact)));
                insert->step();
                insert->reset();
            }
        };
        keep(m_references, "reference");
        keep(m_citedBy, "cited_by");
        transaction.commit();
    } catch (const std::exception &error) {
        fail(RelatedProblem::Network, QString::fromUtf8(error.what()));
        return;
    }
    emit found(m_publicationId);
}

void RelatedFinder::fail(RelatedProblem problem, const QString &detail)
{
    ++m_generation;
    emit failed(m_publicationId, int(problem), detail);
}

void RelatedFinder::failFromReply(const TransportReply &reply)
{
    if (reply.status == 404)
        fail(RelatedProblem::NotFound);
    else if (reply.status == 429)
        fail(RelatedProblem::RateLimited);
    else if (!reply.error.isEmpty())
        fail(RelatedProblem::Network, reply.error);
    else
        fail(RelatedProblem::Network, QStringLiteral("HTTP %1").arg(reply.status));
}

} // namespace papertrail::metadata
