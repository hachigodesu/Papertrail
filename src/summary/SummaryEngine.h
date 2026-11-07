#pragma once

#include "metadata/Transport.h"
#include "summary/SummaryRequest.h"

#include <QObject>
#include <QString>

#include <optional>

namespace papertrail::summary {

struct SummaryOutcome {
    SummaryText summary;
    QList<Source> sources;
    Usage usage;
    std::optional<double> cost; // Nothing if this model's prices aren't known.
    QString model;
    QString effort;
    bool sentPaper = false;
    bool cutOff = false; // It reached the longest answer allowed.
};

enum class SummaryFailure {
    BadKey,      // The key was refused.
    NoCredit,    // The account can't pay for this.
    TooMany,     // Too many requests for now.
    Unavailable, // Anthropic is having problems.
    Network,     // Couldn't reach Anthropic.
    Refused,     // Claude's safeguards declined this.
    Rejected,    // The request wasn't accepted, for some other reason.
    Empty,       // There was no usable answer.
};

// Asks Claude for one summary. Nothing here ever waits, and the window carries on throughout. The API key is only
// ever put in the header of a request to Anthropic, and that request doesn't follow redirects.
class SummaryEngine : public QObject {
    Q_OBJECT

public:
    SummaryEngine(metadata::Transport &transport, const QString &baseUrl, QObject *parent = nullptr);

    void setApiKey(const QString &key) { m_key = key; }

    void generate(const QString &model, const QString &effort, const core::Publication &publication,
                  const std::optional<PaperContent> &paper = std::nullopt);

    // Stops listening for the answer. A request already sent may still be charged for.
    void cancel();
    bool isBusy() const { return m_busy; }

    // Which models this key can use. Only the key is sent, and nothing about any publication.
    void fetchModels();

signals:
    void finished(const papertrail::summary::SummaryOutcome &outcome);
    void failed(papertrail::summary::SummaryFailure failure, const QString &message, const papertrail::summary::Usage &spentSoFar);
    void modelsReady(const QStringList &ids);

private:
    void send();
    void onReply(const metadata::TransportReply &reply);

    metadata::Transport &m_transport;
    QString m_baseUrl;
    QString m_key;
    QString m_model;
    QString m_effort;
    QByteArray m_request;
    QByteArray m_current;
    QJsonArray m_contentSoFar;
    QList<Source> m_sources;
    QString m_text;
    Usage m_usage;
    int m_continuations = 0;
    bool m_sentPaper = false;
    bool m_busy = false;
    quint64 m_generation = 0;
};

} // namespace papertrail::summary

Q_DECLARE_METATYPE(papertrail::summary::SummaryOutcome)
Q_DECLARE_METATYPE(papertrail::summary::Usage)
Q_DECLARE_METATYPE(papertrail::summary::SummaryFailure)
