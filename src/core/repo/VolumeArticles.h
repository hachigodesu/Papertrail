#pragma once

#include "core/model/Publication.h"

#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// One line of a document's table of contents.
struct ContentsEntry {
    QString title;
    int depth = 0;
    int page = -1; // Counting from 0. Below that, it doesn't lead anywhere in the document.
};

// An article to be made. Pages are positions in the file, counting from 1, and aren't the same thing as
// the page numbers printed on it, which is why they don't go in a citation's `pages` field.
struct ArticleDraft {
    QString title;
    int firstPage = 1;
    int lastPage = 1;
    QList<PersonName> authors;
    QString doi;
    bool likelyNotAnArticle = false; // Such as a cover or an index. Offered, but not chosen by default.
    bool alreadyAdded = false;
};

// Only these can have articles, and each has its own kind of article.
bool canHoldArticles(PublicationType type);
PublicationType articleTypeFor(PublicationType volumeType);

// The level most likely to list articles rather than sections, which is the one with the most entries.
int likelyArticleDepth(const QList<ContentsEntry> &entries);

// The entries at one level of the contents, each running until the next at that level or above.
QList<ArticleDraft> draftsFromContents(const QList<ContentsEntry> &entries, int depth, int pageCount);

// Notes which of these are already articles of this volume.
void markExistingArticles(Database &db, qint64 volumeId, QList<ArticleDraft> &drafts);

// Makes these into articles of the volume, carrying over what they share with it such as its journal.
// Those from a table of contents haven't been checked by anyone, so `needsReview` says so.
QList<qint64> createArticles(Database &db, qint64 volumeId, const QList<ArticleDraft> &drafts, bool needsReview);

} // namespace papertrail::core
