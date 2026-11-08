#pragma once

#include "core/model/Publication.h"
#include "core/repo/NoticeRepository.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

// What each online source is asked, and what its answers mean. Everything here is plain functions over
// text, so it can be tested against recorded answers with no network. Answers come from outside, so
// nothing in them is trusted to be well formed.
namespace papertrail::metadata {

// What a source said about a publication.
struct FoundRecord {
    core::Publication publication;
    std::optional<core::PublicationType> type; // Unset if the source's type says nothing useful.
    QString provider;
    QString matchedBy;
    double score = 1.0;

    // The source calls this a preprint. Only ever shown to the user: it's theirs to mark.
    bool preprintHint = false;

    QJsonObject toJson() const;
    static FoundRecord fromJson(const QJsonObject &json);
};

extern const QString kCrossref;
extern const QString kOpenAlex;
extern const QString kArxiv;
extern const QString kOpenLibrary;

// Where each source is, so that tests can point them at a local server.
struct ProviderUrls {
    QString crossref = QStringLiteral("https://api.crossref.org");
    QString openAlex = QStringLiteral("https://api.openalex.org");
    QString arxiv = QStringLiteral("https://export.arxiv.org");
    QString openLibrary = QStringLiteral("https://openlibrary.org");
    QString anthropic = QStringLiteral("https://api.anthropic.com"); // Article summaries, and not lookups.
};

QUrl crossrefWorkUrl(const ProviderUrls &urls, const QString &doi);

// Asks Crossref for the notices, such as retractions, that update any of these works. A DOI with a comma can't be
// asked about this way, so callers leave those out.
QUrl crossrefNoticesUrl(const ProviderUrls &urls, const QStringList &dois);
// The notices in Crossref's answer, by the DOI of the work each is about, in lower case. Anything that isn't one is
// skipped, and a reply that can't be read gives nothing.
QHash<QString, QList<core::Notice>> parseCrossrefNotices(const QByteArray &body, bool *readable = nullptr);
QUrl crossrefSearchUrl(const ProviderUrls &urls, const QString &title);
QUrl openAlexWorkUrl(const ProviderUrls &urls, const QString &doi);
QUrl openAlexSearchUrl(const ProviderUrls &urls, const QString &title);
QUrl arxivUrl(const ProviderUrls &urls, const QString &arxivId);
QUrl openLibraryUrl(const ProviderUrls &urls, const QString &isbn);

std::optional<FoundRecord> parseCrossrefWork(const QByteArray &body);
QList<FoundRecord> parseCrossrefSearch(const QByteArray &body);
std::optional<FoundRecord> parseOpenAlexWork(const QByteArray &body);
QList<FoundRecord> parseOpenAlexSearch(const QByteArray &body);
std::optional<FoundRecord> parseArxiv(const QByteArray &body);
std::optional<FoundRecord> parseOpenLibrary(const QByteArray &body);

} // namespace papertrail::metadata
