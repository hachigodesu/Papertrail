#pragma once

#include "core/model/Publication.h"
#include "core/query/PublicationFilter.h"

#include <QList>
#include <QString>

#include <optional>
#include <utility>

namespace papertrail::core {

class Database;

enum class Facet {
    Type,
    Author,
    Journal,
    Publisher,
    Conference,
    Institution,
    Court,
    Jurisdiction,
    Language,
    Format,
    ReadStatus,
};

struct FacetValue {
    QString key;   // What to put back in the filter: a person id or the value itself.
    QString label; // What to show.
    qint64 count = 0;
};

struct CollectionCount {
    QString key;   // The collection's id, for the filter.
    QString label; // Its name.
    qint64 count = 0; // Publications in it or any collection inside it, each counted once.
    std::optional<qint64> parentId;
};

struct TagCount {
    qint64 tagId = 0;
    QString name;
    std::optional<qint64> parentId;
    QString colour;
    qint64 count = 0; // Publications with this tag or any tag beneath it, each counted once.
};

struct FlagCounts {
    qint64 needsReview = 0;
    qint64 noText = 0;
    qint64 noFile = 0;
    qint64 missingOrOffline = 0;
    qint64 unreadable = 0;
    qint64 retracted = 0;
};

// Read-only queries behind the library view.
class LibraryQuery {
public:
    explicit LibraryQuery(Database &db) : m_db(db) {}

    qint64 count(const PublicationFilter &filter);
    QList<PublicationSummary> page(const PublicationFilter &filter, const PublicationSort &sort,
                                   qint64 offset, int limit);

    // All matching ids in display order, for bulk actions.
    QList<qint64> ids(const PublicationFilter &filter, const PublicationSort &sort);

    // A facet's counts ignore its own selection, so choosing one author still shows the others.
    QList<FacetValue> facet(const PublicationFilter &filter, Facet facet, int limit = 50);
    QList<TagCount> tagCounts(const PublicationFilter &filter);

    // Every collection, including any with nothing matching, as its name sorts. Like a facet, this
    // ignores which collections are chosen.
    QList<CollectionCount> collectionCounts(const PublicationFilter &filter);
    FlagCounts flagCounts(const PublicationFilter &filter);
    std::optional<std::pair<int, int>> yearRange(const PublicationFilter &filter);

private:
    // The SQL condition for the filter's search text, or empty if it has none.
    QByteArray searchClause(const PublicationFilter &filter);
    void addContentMatches(const QString &contentsMatch, const QString &fieldMatch);
    void addNoteMatches(const QString &contentsMatch, const QString &fieldMatch);

    // Publications from removed folders are never part of the library view.
    bool anyHidden();

    Database &m_db;

    // A search's matches are worked out once and reused by the page, the total and the
    // sidebar, until the search text or the library changes.
    QString m_searchText;
    bool m_searchedContents = false;
    QByteArray m_searchStamp;
    qint64 m_searchCount = 0;
    bool m_matchesBeyondDetails = false; // Some of the search's matches came only from highlights and notes.
};

} // namespace papertrail::core
