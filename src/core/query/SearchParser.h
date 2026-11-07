#pragma once

#include "core/model/Publication.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {

// The search box's text, split into what each part has to query.
//
// Supported syntax:
//   free words          each must match, and also matches as a prefix while typing
//   "exact phrase"      words next to each other
//   title: author: abstract: journal: conference: publisher: institution:   one field
//   tag:name            a tag whose name starts with this, plus everything beneath it
//   doi: isbn: issn: arxiv:   identifiers, in any common written form
//   type:thesis         a publication type
//   year:2010  year:>2010  year:>=2010  year:<2010  year:<=2010  year:2010..2020
//
// Anything else with a colon is treated as ordinary text, so it never causes an error.
struct ParsedSearch {
    QString ftsMatch; // An FTS5 expression, with all input safely quoted. Empty if none.

    // The same, split up for searching inside documents, which only have words and no fields.
    QString contentsMatch; // The plain words and phrases.
    QString fieldMatch;    // The rest, which only the metadata can answer.
    QString contentsText;  // What to look for in a reader: a phrase if there is one, or else the longest word.
    QStringList tagPrefixes;
    QStringList doiPrefixes;
    QStringList isbns;
    QStringList issns;
    QStringList arxivPrefixes;
    QList<PublicationType> types;
    std::optional<int> yearMin;
    std::optional<int> yearMax;
};

ParsedSearch parseSearch(const QString &text);

} // namespace papertrail::core
