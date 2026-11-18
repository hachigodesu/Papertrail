#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// Where the library mentions some words, for looking up a selection: publications whose details, notes or pages have
// them, and highlights and notes with them. Hidden publications are left out, as everywhere.
struct Mention {
    enum class Kind { Details, Page, Highlight };

    Kind kind = Kind::Details;
    qint64 publicationId = 0;
    QString title;
    QString authors;
    std::optional<int> year;
    bool thisDocument = false; // In the publication being read.

    // For a page: which file, which page (from 0), and the words around it.
    qint64 fileId = 0;
    int page = -1;
    QString snippet;

    // For a highlight or note.
    qint64 annotationId = 0;
};

struct Mentions {
    QList<Mention> found;
    qint64 publications = 0; // How many publications mention it, which can be more than are listed.
};

// `words` are looked for as one phrase. The full-text index must already be attached to `db` for pages to be found.
Mentions mentionsOf(Database &db, const QString &words, qint64 thisPublication, int maxPublications = 50,
                    int publicationsWithPages = 20, int maxHighlights = 30);

} // namespace papertrail::core
