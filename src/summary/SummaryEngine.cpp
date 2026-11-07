#include "summary/SummaryEngine.h"

#include <QJsonArray>
#include <QPointer>
#include <QUrl>

namespace papertrail::summary {

using metadata::HttpRequest;
using metadata::TransportReply;

namespace {

constexpr int kMostContinuations = 5;
constexpr int kAnswerTimeoutMs = 10 * 60000; // Searching and thinking can take minutes before anything comes back.
constexpr qint64 kLargestAnswerBytes = 96 * 1024 * 1024; // A paper fetched from the web comes back in the reply.
const QByteArray kApiVersion = "2026-10-21";

} // namespace

SummaryEngine::SummaryEngine(metadata::Transport &transport, const QString &baseUrl, QObject *parent)
    : QObject(parent), m_transport(transport), m_baseUrl(baseUrl)
{
    qRegisterMetaType<SummaryOutcome>();
    qRegisterMetaType<Usage>();
    qRegisterMetaType<SummaryFailure>();
}

void SummaryEngine::generate(const QString &model, const QString &effort, const core::Publication &publication,
                             const std::optional<PaperContent> &paper)
{
    m_model = model;
    m_effort = effort;
    m_sentPaper = paper.has_value();
    m_request = buildRequest(model, effort, publication, paper);
    m_current = m_request;
    m_contentSoFar = QJsonArray();
    m_sources.clear();
    m_text.clear();
    m_usage = Usage();
    m_continuations = 0;
    m_busy = true;
    ++m_generation;
    send();
}

void SummaryEngine::cancel()
{
    ++m_generation;
    m_busy = false;
}

void SummaryEngine::send()
{
    HttpRequest request;
    request.url = QUrl(m_baseUrl + QStringLiteral("/v1/messages"));
    request.method = "POST";
    request.headers = {{"x-api-key", m_key.toUtf8()}, {"anthropic-version", kApiVersion}, {"content-type", "application/json"}};
    request.body = m_current;
    request.timeoutMs = kAnswerTimeoutMs;
    request.maxReplyBytes = kLargestAnswerBytes;
    const quint64 generation = m_generation;
    // The dialog can be closed while this is on its way, and the answer may come to nobody.
    QPointer<SummaryEngine> self(this);
    m_transport.send(request, [self, generation](const TransportReply &reply) {
        if (self && generation == self->m_generation)
            self->onReply(reply);
    });
}

void SummaryEngine::onReply(const TransportReply &reply)
{
    const auto fail = [&](SummaryFailure failure, const QString &message) {
        m_busy = false;
        emit failed(failure, message, m_usage);
    };

    if (!reply.error.isEmpty()) {
        fail(SummaryFailure::Network, tr("Couldn't reach Anthropic: %1").arg(reply.error));
        return;
    }
    if (reply.status != 200) {
        const ApiError error = parseError(reply.body);
        const QString message = error.message.isEmpty() ? tr("Anthropic replied with status %1.").arg(reply.status) : error.message;
        if (reply.status == 401 || reply.status == 403)
            fail(SummaryFailure::BadKey, tr("Anthropic didn't accept this API key: %1").arg(message));
        else if (reply.status == 429)
            fail(SummaryFailure::TooMany, tr("Too many requests for now. Try again shortly: %1").arg(message));
        else if (reply.status == 529 || reply.status >= 500)
            fail(SummaryFailure::Unavailable, tr("Anthropic is having problems right now. Try again shortly: %1").arg(message));
        else if (message.contains(QStringLiteral("credit balance"), Qt::CaseInsensitive))
            fail(SummaryFailure::NoCredit, tr("Your Anthropic account doesn't have the credit for this: %1").arg(message));
        else
            fail(SummaryFailure::Rejected, tr("Anthropic didn't accept the request: %1").arg(message));
        return;
    }

    const ParsedResponse parsed = parseResponse(reply.body);
    if (!parsed.valid) {
        fail(SummaryFailure::Empty, tr("Anthropic's reply couldn't be understood."));
        return;
    }
    m_usage += parsed.usage;
    for (const QJsonValue &block : parsed.content)
        m_contentSoFar.append(block);
    m_text += parsed.text;
    for (const Source &source : parsed.sources) {
        if (!m_sources.contains(source))
            m_sources << source;
    }

    if (parsed.stopReason == QLatin1StringView("refusal")) {
        fail(SummaryFailure::Refused, parsed.refusalCategory.isEmpty()
                 ? tr("Claude's safeguards declined this request.")
                 : tr("Claude's safeguards declined this request (%1).").arg(parsed.refusalCategory));
        return;
    }
    if (parsed.stopReason == QLatin1StringView("pause_turn")) {
        if (++m_continuations > kMostContinuations) {
            fail(SummaryFailure::Empty, tr("It took too many steps without finishing, so it was stopped."));
            return;
        }
        m_current = buildContinuation(m_request, m_contentSoFar);
        send();
        return;
    }
    if (m_text.trimmed().isEmpty()) {
        fail(SummaryFailure::Empty, tr("Claude's reply had no text in it."));
        return;
    }

    SummaryOutcome outcome;
    outcome.summary = parseSummaryText(m_text);
    outcome.sources = m_sources;
    outcome.usage = m_usage;
    outcome.cost = costOf(m_model, m_usage);
    outcome.model = m_model;
    outcome.effort = m_effort;
    outcome.sentPaper = m_sentPaper;
    outcome.cutOff = parsed.stopReason == QLatin1StringView("max_tokens");
    if (m_sentPaper && outcome.summary.basis == SummaryBasis::Unknown)
        outcome.summary.basis = SummaryBasis::FullText;
    m_busy = false;
    emit finished(outcome);
}

void SummaryEngine::fetchModels()
{
    HttpRequest request;
    request.url = QUrl(m_baseUrl + QStringLiteral("/v1/models?limit=100"));
    request.method = "GET";
    request.headers = {{"x-api-key", m_key.toUtf8()}, {"anthropic-version", kApiVersion}};
    request.timeoutMs = 30000;
    QPointer<SummaryEngine> self(this);
    m_transport.send(request, [self](const TransportReply &reply) {
        if (self && reply.error.isEmpty() && reply.status == 200)
            emit self->modelsReady(parseModelList(reply.body));
    });
}

} // namespace papertrail::summary
