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
    bool titleFromPage = false; // The contents only gave it a number, so its title was read from its first page.

    // An article made earlier from these contents, when they only gave it a number. Choosing this draft
    // gives that article its title, rather than making another.
    std::optional<qint64> renames;
};

// A contents entry that only numbers an article, such as "01", "12." or "Article 3", rather than naming it.
bool isPlaceholderTitle(const QString &title);

// Takes what's printed on a draft's first page. Its largest text becomes the title if the contents gave
// only a number, and a DOI found there fills an empty one. Returns whether anything changed.
bool adoptPageHeading(ArticleDraft &draft, const QString &pageTitle, const QString &doi);

// Only these can have articles, and each has its own kind of article.
bool canHoldArticles(PublicationType type);
PublicationType articleTypeFor(PublicationType volumeType);

// The level most likely to list articles rather than sections, which is the one with the most entries.
int likelyArticleDepth(const QList<ContentsEntry> &entries);

// The entries at one level of the contents, each running until the next at that level or above.
QList<ArticleDraft> draftsFromContents(const QList<ContentsEntry> &entries, int depth, int pageCount);

// Fills in a volume's numbers where they're empty, such as from its cover or a running header, and those of its
// articles too. Nothing already set is changed. Returns whether anything was.
bool fillVolumeNumbering(Database &db, qint64 volumeId, const QString &volume, const QString &issue,
                         std::optional<int> month, std::optional<int> year);

// Notes which of these are already articles of this volume. One made earlier with only a number for its
// title, starting on the same page, is offered for renaming instead.
void markExistingArticles(Database &db, qint64 volumeId, QList<ArticleDraft> &drafts);

// Makes these into articles of the volume, carrying over what they share with it such as its journal.
// Those from a table of contents haven't been checked by anyone, so `needsReview` says so. A draft that
// `renames` an article gives it its title, and its DOI if it has none, and nothing else about it changes.
QList<qint64> createArticles(Database &db, qint64 volumeId, const QList<ArticleDraft> &drafts, bool needsReview);

} // namespace papertrail::core
