#pragma once

#include "summary/SummaryRequest.h"

#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::summary {

// What asking Claude has cost, noted as each reply comes back, whether or not its summary is kept. Worked out on this
// computer from the tokens Anthropic counted and the prices in knownModels(), so it's close but not the bill itself.
enum class SpendKind { Instant, Batch };

struct SpendEntry {
    QString key;
    qint64 spentAt = 0;
    std::optional<qint64> publicationId; // Nothing once the publication has gone.
    QString title;                       // As it was then, so the list still says what it was for.
    QString model;
    SpendKind kind = SpendKind::Instant;
    std::optional<double> cost; // Nothing if the model's prices weren't known.
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    qint64 webSearches = 0;
};

struct SpendTotals {
    double allTime = 0;
    double thisMonth = 0;
    double last30Days = 0;
    double instant = 0;
    double batch = 0;
    int requests = 0;
    int unpriced = 0; // Replies whose cost couldn't be worked out, which the totals leave out.
};

// `key` is unique to one reply, so noting it again changes nothing. `priceFactor` is kBatchPriceFactor for batches.
void recordSpend(core::Database &db, const QString &key, qint64 publicationId, const QString &model, SpendKind kind,
                 const Usage &usage, double priceFactor = 1.0, qint64 when = 0);

// Newest first.
QList<SpendEntry> spendEntries(core::Database &db, int limit = 5000);
SpendTotals spendTotals(core::Database &db, qint64 now);

} // namespace papertrail::summary
