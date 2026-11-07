#pragma once

#include "core/model/Publication.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::citation {

// What every style and export works from: a publication's details, with what an article or chapter takes from
// the issue, proceedings or book it's in already filled in.
struct CitationItem {
    // The kinds a citation distinguishes, which aren't quite the library's types.
    enum class Kind {
        Article,      // In a journal.
        Book,
        EditedBook,   // A book with editors and no authors.
        Chapter,      // In a book.
        Paper,        // In conference proceedings.
        Proceedings,
        Issue,        // A whole journal issue.
        Thesis,
        Report,
        Standard,
        Preprint,
        Document,     // Anything else.
    };

    Kind kind = Kind::Document;
    core::PublicationType type = core::PublicationType::Other;
    QString uuid;

    QString title; // With its subtitle, after a colon.
    QList<core::PersonName> authors;
    QList<core::PersonName> editors; // Of this work, or for a chapter or paper, of the book or proceedings it's in.

    std::optional<int> year;
    std::optional<int> month;
    std::optional<int> day;

    QString containerTitle; // The journal, book or proceedings.
    QString volume;
    QString issue;
    QString pages; // As printed, such as "43-70". Never the positions in a file.
    QString edition;
    QString series;
    QString publisher;
    QString place;

    QString doi;
    QString url;
    QString isbn;
    QString issn;
    QString arxivId;
    QString pmid;

    QString conferenceName;
    QString conferenceLocation;
    QString institution;
    QString department;
    QString degree;
    QString reportNumber;
    QString standardNumber;
    QString standardBody;

    QString abstract;
    QString language;
    QStringList keywords; // The publication's tags, as paths such as "Engineering > Structures".

    bool hasAuthors() const { return !authors.isEmpty(); }
    bool isOnline() const { return !doi.isEmpty() || !url.isEmpty() || !arxivId.isEmpty(); }

    // A web address for the item: its DOI's, its own, or its arXiv page's.
    QString link() const;
};

CitationItem::Kind kindFor(const core::Publication &publication);

// Whether a reference made from this would be missing something it needs, such as its authors, year or, for an
// article, its journal.
bool lacksDetails(const CitationItem &item);

// Builds an item from a publication alone. `holder` is the issue, proceedings or book an article is part of.
CitationItem citationItem(const core::Publication &publication, const std::optional<core::Publication> &holder = std::nullopt,
                          const QStringList &keywords = {});

// Builds the items for these publications from the library, in the same order. Any that no longer exist are
// left out.
QList<CitationItem> citationItems(core::Database &db, const QList<qint64> &ids);

} // namespace papertrail::citation
