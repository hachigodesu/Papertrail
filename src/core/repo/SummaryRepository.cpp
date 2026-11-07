#include "core/repo/SummaryRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace papertrail::core {

void SummaryRepository::save(StoredSummary &summary)
{
    if (summary.uuid.isEmpty())
        summary.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (summary.createdAt == 0)
        summary.createdAt = QDateTime::currentMSecsSinceEpoch();

    QJsonArray sources;
    for (const auto &source : std::as_const(summary.sources))
        sources.append(QJsonObject{{QStringLiteral("url"), source.first}, {QStringLiteral("title"), source.second}});

    auto upsert = m_db.prepare(
        "INSERT INTO publication_summary (uuid, publication_id, model, effort, basis, identification_unsure, summary_text, "
        "sources_json, cost_dollars, sent_paper, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (publication_id) DO UPDATE SET uuid = excluded.uuid, model = excluded.model, effort = excluded.effort, "
        "basis = excluded.basis, identification_unsure = excluded.identification_unsure, summary_text = excluded.summary_text, "
        "sources_json = excluded.sources_json, cost_dollars = excluded.cost_dollars, sent_paper = excluded.sent_paper, "
        "created_at = excluded.created_at");
    upsert->bind(1, summary.uuid);
    upsert->bind(2, summary.publicationId);
    upsert->bind(3, summary.model);
    upsert->bind(4, summary.effort);
    upsert->bind(5, summary.basis);
    upsert->bind(6, summary.identificationUnsure ? 1 : 0);
    upsert->bind(7, summary.text);
    upsert->bind(8, QString::fromUtf8(QJsonDocument(sources).toJson(QJsonDocument::Compact)));
    if (summary.costDollars)
        upsert->bind(9, *summary.costDollars);
    else
        upsert->bindNull(9);
    upsert->bind(10, summary.sentPaper ? 1 : 0);
    upsert->bind(11, summary.createdAt);
    upsert->step();
    summary.id = m_db.lastInsertId();
}

std::optional<StoredSummary> SummaryRepository::forPublication(qint64 publicationId)
{
    auto select = m_db.prepare(
        "SELECT id, uuid, model, effort, basis, identification_unsure, summary_text, sources_json, cost_dollars, sent_paper, created_at "
        "FROM publication_summary WHERE publication_id = ?");
    select->bind(1, publicationId);
    if (!select->step())
        return std::nullopt;

    StoredSummary summary;
    summary.publicationId = publicationId;
    summary.id = select->int64At(0);
    summary.uuid = select->textAt(1);
    summary.model = select->textAt(2);
    summary.effort = select->textAt(3);
    summary.basis = select->textAt(4);
    summary.identificationUnsure = select->intAt(5) != 0;
    summary.text = select->textAt(6);
    const QJsonArray sources = QJsonDocument::fromJson(select->textAt(7).toUtf8()).array();
    for (const QJsonValue &value : sources) {
        const QJsonObject source = value.toObject();
        summary.sources << qMakePair(source.value(QStringLiteral("url")).toString(), source.value(QStringLiteral("title")).toString());
    }
    if (!select->isNull(8))
        summary.costDollars = select->doubleAt(8);
    summary.sentPaper = select->intAt(9) != 0;
    summary.createdAt = select->int64At(10);
    return summary;
}

void SummaryRepository::remove(qint64 publicationId)
{
    auto remove = m_db.prepare("DELETE FROM publication_summary WHERE publication_id = ?");
    remove->bind(1, publicationId);
    remove->step();
}

} // namespace papertrail::core
