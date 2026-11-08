#pragma once

#include "metadata/Transport.h"
#include "summary/SummaryEngine.h"
#include "summary/SummaryRequest.h"

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <functional>
#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::summary {

// Batches cost half as much as asking straight away, and come back within a day, usually within the hour.
constexpr double kBatchPriceFactor = 0.5;

// The Message Batches API's request: each item an ordinary Messages request, under an id of its own.
QByteArray buildBatch(const QList<QPair<QString, QByteArray>> &requests);

struct BatchInfo {
    bool valid = false;
    QString id;
    QString status; // "in_progress", "canceling" or "ended".
};
BatchInfo parseBatch(const QByteArray &body);

// One line of a batch's results.
struct BatchResult {
    QString customId;
    QString type;       // "succeeded", "errored", "canceled" or "expired".
    QByteArray message; // For one that succeeded: the Messages API's reply, as parseResponse reads it.
    QString error;      // For one that didn't.
};
QList<BatchResult> parseBatchResults(const QByteArray &jsonLines);

// A summary asked for in a batch, as the window lists it.
struct BatchEntry {
    enum class State { Waiting, Paused, Arrived, Kept, Discarded, Failed };
    QString batchId;
    QString customId;
    qint64 publicationId = 0;
    QString title;
    QString model;
    QString effort;
    State state = State::Waiting;
    QString problem; // Why one failed.
    std::optional<double> cost; // What it's cost so far, at half price. Nothing if the model's prices aren't known.
    qint64 sentAt = 0;          // When the user sent it, however many times it's been carried on since.
    qint64 arrivedAt = 0;
    int continuations = 0;
    QString text; // What came back, for one that's arrived.
    QList<Source> sources;
};

// Summaries asked for in batches, at half price. Everything about them is kept in the library, so closing Papertrail
// loses nothing: a batch still on its way is asked about next time, what's come back waits for the user to review
// it, and one that paused is noted before it's sent on. Only what identifies each publication is sent, as when
// asking straight away without the paper. The key only ever goes in a request to Anthropic.
class SummaryBatches : public QObject {
    Q_OBJECT

public:
    SummaryBatches(core::Database &db, metadata::Transport &transport, const QString &baseUrl, QObject *parent = nullptr);

    void setApiKey(const QString &key) { m_key = key; }

    // Sends one batch. Only call this once the user has confirmed what's sent and what it may cost.
    void submit(const QString &model, const QString &effort, const QList<core::Publication> &publications);

    // Sends on anything that paused, asks about each batch still waiting, and collects any that have finished.
    void check();
    bool isBusy() const { return m_busy; }

    // Keeping one makes it the publication's summary, replacing any it had. Throws if it can't be saved.
    void keep(const QString &batchId, const QString &customId);
    void discard(const QString &batchId, const QString &customId);
    // Takes a failed one off the list.
    void dismiss(const QString &batchId, const QString &customId);

    // Publications with a summary on its way, including any that paused and wait to be sent on.
    qint64 waitingCount();
    static qint64 waitingCount(core::Database &db);
    static qint64 arrivedCount(core::Database &db);
    // Everything still on its way, arrived to be reviewed, or failed and not yet taken off the list, newest first.
    static QList<BatchEntry> entries(core::Database &db);
    // The newest of those for one publication.
    static std::optional<BatchEntry> entryFor(core::Database &db, qint64 publicationId);

    static constexpr int kMostContinuations = 5;

signals:
    void submitted(int count);
    void submitFailed(papertrail::summary::SummaryFailure failure, const QString &message);
    // Summaries have come back and wait to be reviewed.
    void summariesArrived(const QList<qint64> &publicationIds);
    // Some couldn't be summarised. `problem` is the first one's reason.
    void summariesFailed(int count, const QString &problem);
    // Anthropic couldn't be asked. It's asked again next time.
    void checkProblem(const QString &message);
    void checked();
    // Anything listed has changed.
    void changed();

private:
    struct Item {
        QString customId;
        qint64 publicationId = 0;
        QByteArray request; // The first request, which a continuation carries on from.
        QJsonArray contentSoFar;
        QString text;
        QJsonArray sources;
        Usage usage;
        int continuations = 0;
        qint64 sentAt = 0;
        QString fromBatch; // For one carried on: where it paused.
        QString fromCustomId;
    };
    struct Group {
        QString model;
        QString effort;
        QList<Item> items;
    };
    // `done` is called once the reply has been dealt with, whatever it was.
    void send(const QString &model, const QString &effort, const QList<Item> &items, bool announce, std::function<void()> done);
    void checkNext(QStringList waiting);
    void collect(const QString &batchId, const QByteArray &results);
    void continuePaused();
    void sendGroups(QList<Group> groups);
    void finishCheck();
    void setState(const QString &batchId, const QString &customId, const QString &state);
    metadata::HttpRequest request(const QString &path, const QByteArray &method) const;

    core::Database &m_db;
    metadata::Transport &m_transport;
    QString m_baseUrl;
    QString m_key;
    bool m_busy = false;
};

} // namespace papertrail::summary
