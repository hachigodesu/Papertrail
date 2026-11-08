#include "summary/SummaryBatches.h"

#include "core/db/Database.h"
#include "core/repo/SummaryRepository.h"
#include "summary/SummarySpending.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>

namespace papertrail::summary {

using metadata::HttpRequest;
using metadata::TransportReply;

namespace {

const QByteArray kApiVersion = "2026-10-21";
constexpr qint64 kLargestResultsBytes = 512LL * 1024 * 1024; // Papers fetched from the web come back in the results.

QJsonObject usageToJson(const Usage &usage)
{
    return QJsonObject{{QStringLiteral("input"), double(usage.inputTokens)},
                       {QStringLiteral("output"), double(usage.outputTokens)},
                       {QStringLiteral("cacheRead"), double(usage.cacheReadTokens)},
                       {QStringLiteral("cacheWrite"), double(usage.cacheWriteTokens)},
                       {QStringLiteral("webSearches"), double(usage.webSearches)}};
}

Usage usageFromJson(const QJsonObject &object)
{
    Usage usage;
    usage.inputTokens = qint64(object.value(QStringLiteral("input")).toDouble());
    usage.outputTokens = qint64(object.value(QStringLiteral("output")).toDouble());
    usage.cacheReadTokens = qint64(object.value(QStringLiteral("cacheRead")).toDouble());
    usage.cacheWriteTokens = qint64(object.value(QStringLiteral("cacheWrite")).toDouble());
    usage.webSearches = qint64(object.value(QStringLiteral("webSearches")).toDouble());
    return usage;
}

QString compact(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString compact(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// What went wrong sending, as the instant engine says it.
std::pair<SummaryFailure, QString> failureFor(const TransportReply &reply)
{
    if (!reply.error.isEmpty())
        return {SummaryFailure::Network, QObject::tr("Couldn't reach Anthropic: %1").arg(reply.error)};
    const ApiError error = parseError(reply.body);
    const QString message = error.message.isEmpty() ? QObject::tr("Anthropic replied with status %1.").arg(reply.status) : error.message;
    if (reply.status == 401 || reply.status == 403)
        return {SummaryFailure::BadKey, QObject::tr("Anthropic didn't accept this API key: %1").arg(message)};
    if (reply.status == 429)
        return {SummaryFailure::TooMany, QObject::tr("Too many requests for now. Try again shortly: %1").arg(message)};
    if (reply.status == 529 || reply.status >= 500)
        return {SummaryFailure::Unavailable, QObject::tr("Anthropic is having problems right now. Try again shortly: %1").arg(message)};
    if (message.contains(QStringLiteral("credit balance"), Qt::CaseInsensitive))
        return {SummaryFailure::NoCredit, QObject::tr("Your Anthropic account doesn't have the credit for this: %1").arg(message)};
    return {SummaryFailure::Rejected, QObject::tr("Anthropic didn't accept the request: %1").arg(message)};
}

} // namespace

QByteArray buildBatch(const QList<QPair<QString, QByteArray>> &requests)
{
    QJsonArray items;
    for (const auto &[customId, body] : requests) {
        items.append(QJsonObject{{QStringLiteral("custom_id"), customId},
                                 {QStringLiteral("params"), QJsonDocument::fromJson(body).object()}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("requests"), items}}).toJson(QJsonDocument::Compact);
}

BatchInfo parseBatch(const QByteArray &body)
{
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    BatchInfo info;
    info.id = object.value(QStringLiteral("id")).toString();
    info.status = object.value(QStringLiteral("processing_status")).toString();
    // An id goes into the address of later requests, so only the characters Anthropic's ids use are accepted.
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]{1,200}$"));
    info.valid = object.value(QStringLiteral("type")).toString() == QLatin1StringView("message_batch") && safe.match(info.id).hasMatch()
        && !info.status.isEmpty();
    return info;
}

QList<BatchResult> parseBatchResults(const QByteArray &jsonLines)
{
    QList<BatchResult> results;
    for (const QByteArray &line : jsonLines.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonObject object = QJsonDocument::fromJson(line).object();
        BatchResult result;
        result.customId = object.value(QStringLiteral("custom_id")).toString();
        const QJsonObject outcome = object.value(QStringLiteral("result")).toObject();
        result.type = outcome.value(QStringLiteral("type")).toString();
        if (result.customId.isEmpty() || result.type.isEmpty())
            continue;
        if (result.type == QLatin1StringView("succeeded")) {
            result.message = QJsonDocument(outcome.value(QStringLiteral("message")).toObject()).toJson(QJsonDocument::Compact);
        } else {
            const QJsonObject error = outcome.value(QStringLiteral("error")).toObject();
            const QJsonObject inner = error.value(QStringLiteral("error")).toObject();
            result.error = inner.value(QStringLiteral("message")).toString();
            if (result.error.isEmpty())
                result.error = error.value(QStringLiteral("message")).toString();
        }
        results << result;
    }
    return results;
}

SummaryBatches::SummaryBatches(core::Database &db, metadata::Transport &transport, const QString &baseUrl, QObject *parent)
    : QObject(parent), m_db(db), m_transport(transport), m_baseUrl(baseUrl)
{
    qRegisterMetaType<SummaryFailure>();
}

HttpRequest SummaryBatches::request(const QString &path, const QByteArray &method) const
{
    HttpRequest request;
    request.url = QUrl(m_baseUrl + path);
    request.method = method;
    request.headers = {{"x-api-key", m_key.toUtf8()}, {"anthropic-version", kApiVersion}};
    if (method == "POST")
        request.headers.append(qMakePair(QByteArray("content-type"), QByteArray("application/json")));
    request.timeoutMs = 120000;
    return request;
}

qint64 SummaryBatches::waitingCount(core::Database &db)
{
    auto select = db.prepare("SELECT count(DISTINCT publication_id) FROM summary_request WHERE state IN ('waiting', 'to_continue')");
    select->step();
    return select->int64At(0);
}

qint64 SummaryBatches::waitingCount()
{
    return waitingCount(m_db);
}

qint64 SummaryBatches::arrivedCount(core::Database &db)
{
    auto select = db.prepare("SELECT count(*) FROM summary_request WHERE state = 'arrived'");
    select->step();
    return select->int64At(0);
}

namespace {

const QByteArray kEntryColumns =
    "SELECT r.batch_id, r.custom_id, r.publication_id, p.title, b.model, b.effort, r.state, r.problem, r.cost_dollars, r.sent_at, "
    "r.arrived_at, r.continuations, r.text_so_far, r.sources FROM summary_request r JOIN summary_batch b ON b.id = r.batch_id "
    "JOIN publication p ON p.id = r.publication_id WHERE r.state IN ('waiting', 'to_continue', 'arrived', 'failed') ";

template<typename Row>
BatchEntry entryFrom(Row &row)
{
    BatchEntry entry;
    entry.batchId = row.textAt(0);
    entry.customId = row.textAt(1);
    entry.publicationId = row.int64At(2);
    entry.title = row.textAt(3);
    entry.model = row.textAt(4);
    entry.effort = row.textAt(5);
    const QString state = row.textAt(6);
    entry.state = state == QLatin1StringView("waiting")     ? BatchEntry::State::Waiting
        : state == QLatin1StringView("to_continue")         ? BatchEntry::State::Paused
        : state == QLatin1StringView("arrived")             ? BatchEntry::State::Arrived
                                                            : BatchEntry::State::Failed;
    entry.problem = row.textAt(7);
    if (!row.isNull(8))
        entry.cost = row.doubleAt(8);
    entry.sentAt = row.int64At(9);
    entry.arrivedAt = row.isNull(10) ? 0 : row.int64At(10);
    entry.continuations = row.intAt(11);
    entry.text = row.textAt(12);
    const QJsonArray sources = QJsonDocument::fromJson(row.textAt(13).toUtf8()).array();
    for (const QJsonValue &value : sources) {
        const QJsonObject source = value.toObject();
        entry.sources << Source{source.value(QStringLiteral("url")).toString(), source.value(QStringLiteral("title")).toString()};
    }
    return entry;
}

} // namespace

QList<BatchEntry> SummaryBatches::entries(core::Database &db)
{
    auto select = db.prepare(kEntryColumns + "ORDER BY r.sent_at DESC, r.publication_id, r.batch_id");
    QList<BatchEntry> entries;
    while (select->step())
        entries << entryFrom(*select);
    return entries;
}

std::optional<BatchEntry> SummaryBatches::entryFor(core::Database &db, qint64 publicationId)
{
    auto select = db.prepare(kEntryColumns + "AND r.publication_id = ? ORDER BY r.sent_at DESC, r.batch_id DESC LIMIT 1");
    select->bind(1, publicationId);
    if (!select->step())
        return std::nullopt;
    return entryFrom(*select);
}

void SummaryBatches::submit(const QString &model, const QString &effort, const QList<core::Publication> &publications)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<Item> items;
    for (const core::Publication &publication : publications) {
        Item item;
        item.publicationId = publication.id;
        item.customId = QStringLiteral("p%1").arg(publication.id);
        item.sentAt = now;
        // Only what identifies it, as when asking straight away without sending the paper.
        item.request = buildRequest(model, effort, publication, std::nullopt);
        items << item;
    }
    if (!items.isEmpty())
        send(model, effort, items, true, [] {});
}

void SummaryBatches::send(const QString &model, const QString &effort, const QList<Item> &items, bool announce, std::function<void()> done)
{
    QList<QPair<QString, QByteArray>> requests;
    for (const Item &item : items)
        requests.append({item.customId, item.continuations == 0 ? item.request : buildContinuation(item.request, item.contentSoFar)});
    HttpRequest post = request(QStringLiteral("/v1/messages/batches"), "POST");
    post.body = buildBatch(requests);
    post.maxReplyBytes = 4 * 1024 * 1024;
    QPointer<SummaryBatches> self(this);
    m_transport.send(post, [self, model, effort, items, announce, done](const TransportReply &reply) {
        if (!self)
            return;
        const BatchInfo info = reply.error.isEmpty() && reply.status == 200 ? parseBatch(reply.body) : BatchInfo{};
        if (!info.valid) {
            const auto [failure, message] = reply.error.isEmpty() && reply.status == 200
                ? std::pair<SummaryFailure, QString>{SummaryFailure::Empty, tr("Anthropic's reply couldn't be understood.")}
                : failureFor(reply);
            if (announce) {
                emit self->submitFailed(failure, message);
            } else if (failure == SummaryFailure::Rejected || failure == SummaryFailure::Empty) {
                // Sending it again would only be refused again, or it may have been sent after all, so it's said to have failed.
                try {
                    core::Transaction transaction(self->m_db);
                    auto mark = self->m_db.prepare(
                        "UPDATE summary_request SET state = 'failed', problem = ? WHERE batch_id = ? AND custom_id = ? AND state = 'to_continue'");
                    for (const Item &item : items) {
                        mark->reset();
                        mark->bind(1, message);
                        mark->bind(2, item.fromBatch);
                        mark->bind(3, item.fromCustomId);
                        mark->step();
                    }
                    transaction.commit();
                } catch (const std::exception &error) {
                    emit self->checkProblem(QString::fromUtf8(error.what()));
                }
                emit self->summariesFailed(int(items.size()), message);
                emit self->changed();
            } else {
                // Still noted as paused, so it's sent on next time.
                emit self->checkProblem(message);
            }
            done();
            return;
        }
        try {
            core::Database &db = self->m_db;
            core::Transaction transaction(db);
            auto batch = db.prepare("INSERT INTO summary_batch (id, model, effort, created_at, status) VALUES (?, ?, ?, ?, 'waiting')");
            batch->bind(1, info.id);
            batch->bind(2, model);
            batch->bind(3, effort);
            batch->bind(4, QDateTime::currentMSecsSinceEpoch());
            batch->step();
            auto insert = db.prepare(
                "INSERT INTO summary_request (batch_id, custom_id, publication_id, request, content_so_far, text_so_far, sources, usage, "
                "continuations, state, cost_dollars, sent_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'waiting', ?, ?)");
            auto carriedOn = db.prepare("UPDATE summary_request SET state = 'continued' WHERE batch_id = ? AND custom_id = ?");
            for (const Item &item : items) {
                insert->reset();
                insert->bind(1, info.id);
                insert->bind(2, item.customId);
                insert->bind(3, item.publicationId);
                insert->bind(4, QString::fromUtf8(item.request));
                insert->bind(5, compact(item.contentSoFar));
                insert->bind(6, item.text.isNull() ? QStringLiteral("") : item.text);
                insert->bind(7, compact(item.sources));
                insert->bind(8, compact(usageToJson(item.usage)));
                insert->bind(9, item.continuations);
                if (const std::optional<double> cost = costOf(model, item.usage); cost && item.continuations > 0)
                    insert->bind(10, *cost * kBatchPriceFactor);
                else
                    insert->bindNull(10);
                insert->bind(11, item.sentAt);
                insert->step();
                if (!item.fromBatch.isEmpty()) {
                    carriedOn->reset();
                    carriedOn->bind(1, item.fromBatch);
                    carriedOn->bind(2, item.fromCustomId);
                    carriedOn->step();
                }
            }
            transaction.commit();
        } catch (const std::exception &error) {
            const QString message = tr("The batch was sent, but couldn't be noted in the library: %1").arg(QString::fromUtf8(error.what()));
            if (announce)
                emit self->submitFailed(SummaryFailure::Rejected, message);
            else
                emit self->checkProblem(message);
            done();
            return;
        }
        if (announce)
            emit self->submitted(int(items.size()));
        emit self->changed();
        done();
    });
}

void SummaryBatches::check()
{
    if (m_busy || m_key.isEmpty())
        return;
    QStringList waiting;
    try {
        auto select = m_db.prepare("SELECT id FROM summary_batch WHERE status = 'waiting' ORDER BY created_at");
        while (select->step())
            waiting << select->textAt(0);
    } catch (const std::exception &error) {
        emit checkProblem(QString::fromUtf8(error.what()));
        return;
    }
    m_busy = true;
    checkNext(waiting);
}

void SummaryBatches::finishCheck()
{
    m_busy = false;
    emit checked();
}

void SummaryBatches::checkNext(QStringList waiting)
{
    if (waiting.isEmpty()) {
        // Then whatever paused, including any noted before Papertrail last closed.
        continuePaused();
        return;
    }
    const QString id = waiting.takeFirst();
    QPointer<SummaryBatches> self(this);
    const auto stop = [self](const QString &message) {
        emit self->checkProblem(message);
        self->finishCheck();
    };
    m_transport.send(request(QStringLiteral("/v1/messages/batches/") + id, "GET"), [self, id, waiting, stop](const TransportReply &reply) {
        if (!self)
            return;
        if (reply.error.isEmpty() && reply.status == 404) {
            // Anthropic keeps results for 29 days. After that, there's nothing to collect.
            self->collect(id, QByteArray());
            self->checkNext(waiting);
            return;
        }
        if (!reply.error.isEmpty() || reply.status != 200) {
            stop(failureFor(reply).second);
            return;
        }
        const BatchInfo info = parseBatch(reply.body);
        if (!info.valid) {
            stop(tr("Anthropic's reply about a batch couldn't be understood."));
            return;
        }
        if (info.status != QLatin1StringView("ended")) {
            self->checkNext(waiting);
            return;
        }
        // The results' own address, built here rather than taken from the reply, so the key only goes to Anthropic.
        HttpRequest results = self->request(QStringLiteral("/v1/messages/batches/%1/results").arg(id), "GET");
        results.maxReplyBytes = kLargestResultsBytes;
        results.timeoutMs = 10 * 60000;
        self->m_transport.send(results, [self, id, waiting, stop](const TransportReply &answer) {
            if (!self)
                return;
            if (!answer.error.isEmpty() || answer.status != 200) {
                stop(failureFor(answer).second);
                return;
            }
            self->collect(id, answer.body);
            self->checkNext(waiting);
        });
    });
}

void SummaryBatches::collect(const QString &batchId, const QByteArray &results)
{
    QString model;
    QString effort;
    QHash<QString, Item> items;
    try {
        auto batch = m_db.prepare("SELECT model, effort FROM summary_batch WHERE id = ?");
        batch->bind(1, batchId);
        if (!batch->step())
            return;
        model = batch->textAt(0);
        effort = batch->textAt(1);
        auto select = m_db.prepare("SELECT custom_id, publication_id, request, content_so_far, text_so_far, sources, usage, continuations, "
                                   "sent_at FROM summary_request WHERE batch_id = ? AND state = 'waiting'");
        select->bind(1, batchId);
        while (select->step()) {
            Item item;
            item.customId = select->textAt(0);
            item.publicationId = select->int64At(1);
            item.request = select->textAt(2).toUtf8();
            item.contentSoFar = QJsonDocument::fromJson(select->textAt(3).toUtf8()).array();
            item.text = select->textAt(4);
            item.sources = QJsonDocument::fromJson(select->textAt(5).toUtf8()).array();
            item.usage = usageFromJson(QJsonDocument::fromJson(select->textAt(6).toUtf8()).object());
            item.continuations = select->intAt(7);
            item.sentAt = select->int64At(8);
            items.insert(item.customId, item);
        }
    } catch (const std::exception &error) {
        emit checkProblem(QString::fromUtf8(error.what()));
        return;
    }

    // What each comes to is noted all together, so what's come back is either all kept in the library or, if
    // Papertrail closes first, all collected again next time.
    QHash<QString, QString> state;   // By custom id.
    QHash<QString, QString> problem; // By custom id: why one failed.
    QHash<QString, Usage> spent;     // By custom id: what this reply cost.
    for (const BatchResult &result : parseBatchResults(results)) {
        if (!items.contains(result.customId) || state.contains(result.customId))
            continue; // Its publication has gone since, or it was dealt with already.
        Item &item = items[result.customId];
        if (result.type != QLatin1StringView("succeeded")) {
            state.insert(result.customId, QStringLiteral("failed"));
            problem.insert(result.customId, result.type == QLatin1StringView("expired")
                                                ? tr("Anthropic didn't get to it within a day.")
                                                : result.type == QLatin1StringView("canceled")
                                                ? tr("It was cancelled.")
                                                : result.error.isEmpty() ? tr("Anthropic couldn't summarise it.") : result.error);
            continue;
        }
        const ParsedResponse parsed = parseResponse(result.message);
        if (!parsed.valid) {
            state.insert(result.customId, QStringLiteral("failed"));
            problem.insert(result.customId, tr("Anthropic's reply couldn't be understood."));
            continue;
        }
        spent.insert(result.customId, parsed.usage);
        item.usage += parsed.usage;
        for (const QJsonValue &block : parsed.content)
            item.contentSoFar.append(block);
        item.text += parsed.text;
        for (const Source &source : parsed.sources) {
            const QJsonObject entry{{QStringLiteral("url"), source.url}, {QStringLiteral("title"), source.title}};
            if (!item.sources.contains(entry))
                item.sources.append(entry);
        }
        QString outcome = QStringLiteral("arrived");
        if (parsed.stopReason == QLatin1StringView("refusal")) {
            outcome = QStringLiteral("failed");
            problem.insert(result.customId, tr("Claude's safeguards declined this request."));
        } else if (parsed.stopReason == QLatin1StringView("pause_turn")) {
            if (item.continuations + 1 > kMostContinuations) {
                outcome = QStringLiteral("failed");
                problem.insert(result.customId, tr("It took too many steps without finishing, so it was stopped."));
            } else {
                outcome = QStringLiteral("to_continue"); // Sent on next, from where it paused.
            }
        } else if (item.text.trimmed().isEmpty()) {
            outcome = QStringLiteral("failed");
            problem.insert(result.customId, tr("Claude's reply had no text in it."));
        }
        state.insert(result.customId, outcome);
    }

    QList<qint64> arrived;
    int failedCount = 0;
    QString firstProblem;
    try {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        core::Transaction transaction(m_db);
        auto update = m_db.prepare(
            "UPDATE summary_request SET state = ?, problem = ?, content_so_far = ?, text_so_far = ?, sources = ?, usage = ?, "
            "cost_dollars = ?, arrived_at = ? WHERE batch_id = ? AND custom_id = ?");
        for (auto it = items.cbegin(); it != items.cend(); ++it) {
            const QString outcome = state.value(it.key(), QStringLiteral("failed"));
            QString why;
            if (outcome == QLatin1StringView("failed")) {
                why = problem.value(it.key(), results.isEmpty() ? tr("Anthropic no longer has the results.") : tr("It wasn't in the batch's results."));
                ++failedCount;
                if (firstProblem.isEmpty())
                    firstProblem = why;
            } else if (outcome == QLatin1StringView("arrived")) {
                arrived << it->publicationId;
            }
            if (spent.contains(it.key()))
                recordSpend(m_db, QStringLiteral("batch:%1:%2").arg(batchId, it.key()), it->publicationId, model, SpendKind::Batch,
                            spent.value(it.key()), kBatchPriceFactor, now);
            update->reset();
            update->bind(1, outcome);
            if (why.isEmpty())
                update->bindNull(2);
            else
                update->bind(2, why);
            update->bind(3, compact(it->contentSoFar));
            update->bind(4, it->text.isNull() ? QStringLiteral("") : it->text);
            update->bind(5, compact(it->sources));
            update->bind(6, compact(usageToJson(it->usage)));
            const std::optional<double> cost = costOf(model, it->usage);
            if (cost && (it->usage.inputTokens > 0 || it->usage.outputTokens > 0))
                update->bind(7, *cost * kBatchPriceFactor);
            else
                update->bindNull(7);
            if (outcome == QLatin1StringView("arrived"))
                update->bind(8, now);
            else
                update->bindNull(8);
            update->bind(9, batchId);
            update->bind(10, it.key());
            update->step();
        }
        auto done = m_db.prepare("UPDATE summary_batch SET status = 'collected' WHERE id = ?");
        done->bind(1, batchId);
        done->step();
        transaction.commit();
    } catch (const std::exception &error) {
        emit checkProblem(QString::fromUtf8(error.what()));
        return;
    }

    if (!arrived.isEmpty())
        emit summariesArrived(arrived);
    if (failedCount > 0)
        emit summariesFailed(failedCount, firstProblem);
    emit changed();
}

void SummaryBatches::continuePaused()
{
    QList<Group> groups;
    try {
        auto select = m_db.prepare(
            "SELECT r.batch_id, r.custom_id, r.publication_id, r.request, r.content_so_far, r.text_so_far, r.sources, r.usage, "
            "r.continuations, r.sent_at, b.model, b.effort FROM summary_request r JOIN summary_batch b ON b.id = r.batch_id "
            "WHERE r.state = 'to_continue' ORDER BY b.model, b.effort, r.sent_at");
        while (select->step()) {
            Item item;
            item.fromBatch = select->textAt(0);
            item.fromCustomId = select->textAt(1);
            item.publicationId = select->int64At(2);
            item.request = select->textAt(3).toUtf8();
            item.contentSoFar = QJsonDocument::fromJson(select->textAt(4).toUtf8()).array();
            item.text = select->textAt(5);
            item.sources = QJsonDocument::fromJson(select->textAt(6).toUtf8()).array();
            item.usage = usageFromJson(QJsonDocument::fromJson(select->textAt(7).toUtf8()).object());
            item.continuations = select->intAt(8) + 1;
            item.sentAt = select->int64At(9);
            item.customId = QStringLiteral("p%1-c%2").arg(item.publicationId).arg(item.continuations);
            const QString model = select->textAt(10);
            const QString effort = select->textAt(11);
            if (groups.isEmpty() || groups.last().model != model || groups.last().effort != effort)
                groups << Group{model, effort, {}};
            groups.last().items << item;
        }
    } catch (const std::exception &error) {
        emit checkProblem(QString::fromUtf8(error.what()));
        finishCheck();
        return;
    }
    sendGroups(groups);
}

void SummaryBatches::sendGroups(QList<Group> groups)
{
    if (groups.isEmpty()) {
        finishCheck();
        return;
    }
    const Group group = groups.takeFirst();
    QPointer<SummaryBatches> self(this);
    send(group.model, group.effort, group.items, false, [self, groups] {
        if (self)
            self->sendGroups(groups);
    });
}

void SummaryBatches::setState(const QString &batchId, const QString &customId, const QString &state)
{
    auto update = m_db.prepare("UPDATE summary_request SET state = ? WHERE batch_id = ? AND custom_id = ?");
    update->bind(1, state);
    update->bind(2, batchId);
    update->bind(3, customId);
    update->step();
}

void SummaryBatches::keep(const QString &batchId, const QString &customId)
{
    std::optional<BatchEntry> entry;
    {
        auto select = m_db.prepare(kEntryColumns + "AND r.batch_id = ? AND r.custom_id = ? AND r.state = 'arrived'");
        select->bind(1, batchId);
        select->bind(2, customId);
        if (select->step())
            entry = entryFrom(*select);
    }
    if (!entry)
        return; // Kept or discarded already.

    const SummaryText text = parseSummaryText(entry->text);
    core::StoredSummary stored;
    stored.publicationId = entry->publicationId;
    stored.model = entry->model;
    stored.effort = entry->effort;
    stored.basis = basisToText(text.basis);
    stored.identificationUnsure = text.identificationUnsure;
    stored.text = summaryAsText(text);
    for (const Source &source : std::as_const(entry->sources))
        stored.sources << qMakePair(source.url, source.title);
    stored.costDollars = entry->cost;
    stored.sentPaper = false;
    core::Transaction transaction(m_db);
    core::SummaryRepository(m_db).save(stored);
    setState(batchId, customId, QStringLiteral("kept"));
    transaction.commit();
    emit changed();
}

void SummaryBatches::discard(const QString &batchId, const QString &customId)
{
    setState(batchId, customId, QStringLiteral("discarded"));
    emit changed();
}

void SummaryBatches::dismiss(const QString &batchId, const QString &customId)
{
    setState(batchId, customId, QStringLiteral("dismissed"));
    emit changed();
}

} // namespace papertrail::summary
