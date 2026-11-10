#pragma once

#include "metadata/Providers.h"
#include "metadata/Transport.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::metadata {

// A paper that a publication cites, or that cites it, as OpenAlex describes it.
struct RelatedWork {
    QString openAlexId; // Such as "W2919115771".
    int citedByCount = 0;
    FoundRecord record;
    std::optional<qint64> inLibrary; // The publication here that it is, if there is one.
};

// What's kept of a publication's related papers. Nothing here asks OpenAlex: see RelatedFinder.
struct RelatedPapers {
    bool fetched = false;
    qint64 fetchedAt = 0;
    QString doi; // The DOI they were found for.
    int referenceTotal = 0; // How many it cites, as OpenAlex knows them, which may be more than are listed.
    int citedByTotal = 0;   // How many cite it. Only the most cited are listed.
    QList<RelatedWork> references;
    QList<RelatedWork> citedBy;
};

// What's been kept for this publication, with which of them are already in the library.
RelatedPapers relatedPapersFor(core::Database &db, qint64 publicationId);

// Adds a related paper to the library as a publication with no file, with OpenAlex's details. It's joined with its
// file if that turns up later. Returns its id.
qint64 addRelatedToLibrary(core::Database &db, const FoundRecord &record);

// OpenAlex's ids are only trusted in this form before they go into an address.
bool isOpenAlexWorkId(const QString &id);

QUrl openAlexRelatedUrl(const ProviderUrls &urls, const QString &doi);
QUrl openAlexWorksByIdUrl(const ProviderUrls &urls, const QStringList &ids);
QUrl openAlexCitingUrl(const ProviderUrls &urls, const QString &id);

// The work's own id and what it cites, from openAlexRelatedUrl's answer. Ids that aren't OpenAlex works are left out.
struct OpenAlexWorkLinks {
    QString id;
    int citedByCount = 0;
    QStringList references;
};
std::optional<OpenAlexWorkLinks> parseOpenAlexWorkLinks(const QByteArray &body);
// The works in a list's answer, and how many there are in all.
QList<RelatedWork> parseOpenAlexWorkList(const QByteArray &body, int *total = nullptr);

// Why related papers couldn't be found. Sent as an int, so it crosses threads without registering it.
enum class RelatedProblem { NoDoi, NotFound, RateLimited, Unreadable, Network, Offline };

// Asks OpenAlex for the papers a publication cites and the most cited of those that cite it, by its DOI and nothing
// else, and keeps what it says. It's only asked for by the user, one publication at a time, and only ever started when
// online lookups are allowed; it doesn't check that itself.
class RelatedFinder : public QObject {
    Q_OBJECT

public:
    RelatedFinder(core::Database &db, Transport &transport, QObject *parent = nullptr);

    void setProviderUrls(const ProviderUrls &urls) { m_urls = urls; }
    void setContactEmail(const QString &email) { m_email = email; }

    // Uses what's kept if it's recent and for the same DOI, unless `refresh`. A newer request replaces one under way.
    void find(qint64 publicationId, bool refresh);

    static constexpr int kKeepDays = 30;
    static constexpr int kMostReferences = 500; // More than any ordinary paper cites.

signals:
    void found(qint64 publicationId);
    // `problem` is a RelatedProblem. `detail` is what went wrong, for a network or database failure.
    void failed(qint64 publicationId, int problem, const QString &detail);

private:
    void ask(const QUrl &url, const std::function<void(const TransportReply &)> &then);
    void askForReferences(int from);
    void finish();
    void fail(RelatedProblem problem, const QString &detail = QString());
    void failFromReply(const TransportReply &reply);

    core::Database &m_db;
    Transport &m_transport;
    ProviderUrls m_urls;
    QString m_email;
    quint64 m_generation = 0;

    // The publication under way.
    qint64 m_publicationId = 0;
    QString m_doi;
    OpenAlexWorkLinks m_links;
    QList<RelatedWork> m_references;
    QList<RelatedWork> m_citedBy;
    int m_citedByTotal = 0;
};

} // namespace papertrail::metadata
