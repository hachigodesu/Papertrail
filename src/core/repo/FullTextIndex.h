#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {

class Database;

// Document text lives in a database of its own beside the library, so that the library stays small and
// quick to back up. All of it can be rebuilt from the files, so it's never exported. Text is kept by the
// file's content hash, which means a moved file or a second copy doesn't get indexed again.
QString fullTextPathFor(const QString &libraryPath);

// Makes the index available to this connection under the name "fulltext", creating it if need be.
void attachFullText(Database &db, const QString &path);
bool hasFullText(Database &db);

// Each page's row number is this times its document's id, plus the page. A search within one document can
// then go straight to its range of rows, however common the word is across the library.
constexpr qint64 kFullTextPagesPerDocument = 1000000;

struct ContentMatch {
    qint64 fileId = 0;
    QString format;
    int page = 0; // Counting from 0.
    QString snippet;
};

class FullTextIndex {
public:
    explicit FullTextIndex(Database &db) : m_db(db) {}

    struct Pending {
        qint64 fileId = 0;
        QString sha256;
    };
    // The next file after this one whose contents haven't been indexed.
    std::optional<Pending> nextPending(qint64 afterFileId);
    qint64 pendingCount();
    qint64 indexedCount();

    // Starts again from nothing for these contents, and returns the document's id.
    qint64 beginDocument(const QString &sha256);
    void addPages(qint64 documentId, int firstPage, const QStringList &texts);

    // Even with a problem, it's finished with: what could be read stays searchable, and it isn't tried again.
    void finishDocument(qint64 documentId, int pageCount, const QString &problem);

    // Forgets the text of contents that no file in the library has any more.
    int removeOrphans();
    // Forgets a document's text, so it's read again, such as after its scanned pages' words have been recognised.
    void forget(const QString &sha256);

    // Where these words are in a publication's files, or only the pages of an article. `match` is an FTS5
    // expression, as ParsedSearch::contentsMatch gives.
    QList<ContentMatch> matchesFor(qint64 publicationId, const QString &match, int limit = 50);

    QString problemFor(const QString &sha256);

private:
    Database &m_db;
};

// Joins words split over a line break, so that a search finds them. The file itself isn't touched.
QString tidiedPageText(const QString &text);

} // namespace papertrail::core
