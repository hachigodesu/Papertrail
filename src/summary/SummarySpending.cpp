#include "summary/SummarySpending.h"

#include "core/db/Database.h"

#include <QDateTime>

namespace papertrail::summary {

void recordSpend(core::Database &db, const QString &key, qint64 publicationId, const QString &model, SpendKind kind,
                 const Usage &usage, double priceFactor, qint64 when)
{
    if (usage.inputTokens == 0 && usage.outputTokens == 0 && usage.webSearches == 0)
        return; // Nothing was charged.
    QString title;
    if (publicationId > 0) {
        auto select = db.prepare("SELECT title FROM publication WHERE id = ?");
        select->bind(1, publicationId);
        if (select->step())
            title = select->textAt(0);
    }
    auto insert = db.prepare(
        "INSERT OR IGNORE INTO summary_spend (key, spent_at, publication_id, title, model, kind, cost_dollars, input_tokens, "
        "output_tokens, web_searches) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert->bind(1, key);
    insert->bind(2, when > 0 ? when : QDateTime::currentMSecsSinceEpoch());
    if (publicationId > 0)
        insert->bind(3, publicationId);
    else
        insert->bindNull(3);
    insert->bind(4, title.isNull() ? QStringLiteral("") : title);
    insert->bind(5, model);
    insert->bind(6, kind == SpendKind::Batch ? QStringLiteral("batch") : QStringLiteral("instant"));
    if (const std::optional<double> cost = costOf(model, usage))
        insert->bind(7, *cost * priceFactor);
    else
        insert->bindNull(7);
    insert->bind(8, usage.inputTokens + usage.cacheReadTokens + usage.cacheWriteTokens);
    insert->bind(9, usage.outputTokens);
    insert->bind(10, usage.webSearches);
    insert->step();
}

QList<SpendEntry> spendEntries(core::Database &db, int limit)
{
    auto select = db.prepare(
        "SELECT key, spent_at, publication_id, title, model, kind, cost_dollars, input_tokens, output_tokens, web_searches "
        "FROM summary_spend ORDER BY spent_at DESC, key LIMIT ?");
    select->bind(1, limit);
    QList<SpendEntry> entries;
    while (select->step()) {
        SpendEntry entry;
        entry.key = select->textAt(0);
        entry.spentAt = select->int64At(1);
        if (!select->isNull(2))
            entry.publicationId = select->int64At(2);
        entry.title = select->textAt(3);
        entry.model = select->textAt(4);
        entry.kind = select->textAt(5) == QLatin1StringView("batch") ? SpendKind::Batch : SpendKind::Instant;
        if (!select->isNull(6))
            entry.cost = select->doubleAt(6);
        entry.inputTokens = select->int64At(7);
        entry.outputTokens = select->int64At(8);
        entry.webSearches = select->int64At(9);
        entries << entry;
    }
    return entries;
}

SpendTotals spendTotals(core::Database &db, qint64 now)
{
    const QDateTime current = QDateTime::fromMSecsSinceEpoch(now);
    const qint64 monthStart = QDateTime(QDate(current.date().year(), current.date().month(), 1), QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 thirtyDaysAgo = now - 30LL * 24 * 60 * 60 * 1000;
    auto select = db.prepare(
        "SELECT count(*), count(*) - count(cost_dollars), total(cost_dollars), "
        "total(CASE WHEN spent_at >= ? THEN cost_dollars END), total(CASE WHEN spent_at >= ? THEN cost_dollars END), "
        "total(CASE WHEN kind = 'instant' THEN cost_dollars END), total(CASE WHEN kind = 'batch' THEN cost_dollars END) "
        "FROM summary_spend");
    select->bind(1, monthStart);
    select->bind(2, thirtyDaysAgo);
    SpendTotals totals;
    if (select->step()) {
        totals.requests = select->intAt(0);
        totals.unpriced = select->intAt(1);
        totals.allTime = select->doubleAt(2);
        totals.thisMonth = select->doubleAt(3);
        totals.last30Days = select->doubleAt(4);
        totals.instant = select->doubleAt(5);
        totals.batch = select->doubleAt(6);
    }
    return totals;
}

} // namespace papertrail::summary
