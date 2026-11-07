#pragma once

#include "core/model/Publication.h"

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::summary {

// What each model costs, in US dollars per million tokens. Anthropic's list of models doesn't include prices.
struct ModelInfo {
    QString id;
    QString label;
    double inputPerMillion = 0;
    double outputPerMillion = 0;
    // Some models cost more for a long prompt.
    qint64 longPromptAbove = 0;
    double longInputPerMillion = 0;
    double longOutputPerMillion = 0;
};

const QList<ModelInfo> &knownModels();
std::optional<ModelInfo> modelInfo(const QString &id);
QString pricesCheckedOn();
constexpr double kDollarsPerWebSearch = 0.01;
QString defaultModel();
QStringList effortLevels();
QString defaultEffort();
QString effortLabel(const QString &effort);

// What identifies a publication, which is everything that's sent unless the user chooses to send the paper.
struct IdentifyingDetail {
    QString label;
    QString value;
};
QList<IdentifyingDetail> identifyingDetails(const core::Publication &publication);

// The paper itself, for the last resort. A PDF goes as it is, and anything else as its text.
struct PaperContent {
    QByteArray pdfBase64;
    QString text;
    bool isPdf() const { return !pdfBase64.isEmpty(); }
};

// The Messages API's request. With no paper, Claude is given web search and web fetch to find it with.
QByteArray buildRequest(const QString &model, const QString &effort, const core::Publication &publication,
                        const std::optional<PaperContent> &paper);

// The same request, carrying on from where the server paused, with everything it has returned so far.
QByteArray buildContinuation(const QByteArray &request, const QJsonArray &contentSoFar);

struct Usage {
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    qint64 cacheReadTokens = 0;
    qint64 cacheWriteTokens = 0;
    qint64 webSearches = 0;

    Usage &operator+=(const Usage &other);
};

// In dollars, or nothing if this model's prices aren't known.
std::optional<double> costOf(const QString &model, const Usage &usage);

struct Source {
    QString url;
    QString title;
    bool operator==(const Source &) const = default;
};

struct ParsedResponse {
    bool valid = false;
    QString stopReason;
    QString refusalCategory;
    QJsonArray content; // Exactly as it came, to send back if the server has paused.
    QString text;
    Usage usage;
    QList<Source> sources; // Only what was read or cited, and not everything a search turned up.
};
ParsedResponse parseResponse(const QByteArray &body);

// What an error reply says, for showing as plain text.
struct ApiError {
    QString type;
    QString message;
};
ApiError parseError(const QByteArray &body);

// Model ids from the list of models.
QStringList parseModelList(const QByteArray &body);

enum class SummaryBasis { FullText, AbstractOnly, OtherSources, NotFound, Unknown };
QString basisToText(SummaryBasis basis);
SummaryBasis basisFromText(const QString &text);

// The sections of a summary, as far as the reply follows the format it was asked for. If it doesn't,
// `structured` is false and the whole reply is in `raw`, so that nothing is lost.
struct SummaryText {
    SummaryBasis basis = SummaryBasis::Unknown;
    bool identificationUnsure = false;
    QString aim;
    QString methods;
    QString results;
    QString limitations;
    QString conclusion;
    QString raw;
    bool structured = false;
};
SummaryText parseSummaryText(const QString &text);
QString summaryAsText(const SummaryText &summary);

// What it may cost, which can only be a range: the length of the answer and how much the web tools read can't
// be known beforehand. Worked out on this computer, so nothing is sent to get it.
struct CostRange {
    double low = 0;
    double high = 0;
};
std::optional<CostRange> estimateFromDetails(const QString &model, const QString &effort, const core::Publication &publication);
std::optional<CostRange> estimateFromPaper(const QString &model, const QString &effort, int pages, bool isPdf, qint64 textCharacters = 0);
QString formatDollars(double dollars);
QString formatRange(const CostRange &range);

// Anthropic's limits on a PDF. Base64 makes it a third larger, and the limit is on the whole request.
constexpr qint64 kLargestRequestBytes = 32 * 1024 * 1024;
constexpr int kMostPdfPages = 600;
constexpr qint64 kMostTextCharacters = 2500000; // Comfortably inside the context window of every model offered.

} // namespace papertrail::summary
