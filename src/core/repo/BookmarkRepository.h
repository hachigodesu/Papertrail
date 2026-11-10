#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// A place in a document kept by the reader, with a name. Like a highlight, it belongs to a file's contents, so it
// survives the file moving, and it's kept only in Papertrail's database.
struct Bookmark {
    qint64 id = 0;
    QString uuid;
    qint64 publicationId = 0;
    QString contentHash;
    QString format;
    // Where it is: a page counting from 0, and how far down it, in points. A reflowable document's pages move with
    // its layout, so it also has a place that survives that, and the page is only where it was last time.
    int page = 0;
    double y = 0;
    QString place;
    QString label;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
};

class BookmarkRepository {
public:
    explicit BookmarkRepository(Database &db) : m_db(db) {}

    qint64 create(Bookmark &bookmark);
    std::optional<Bookmark> get(qint64 id);
    void rename(qint64 id, const QString &label);
    // Where it was found last, for a reflowable document.
    void setPage(qint64 id, int page);
    void remove(qint64 id);

    // Every bookmark on one file's contents, whichever publication it was made under, in page order.
    QList<Bookmark> forContents(const QString &contentHash);
    // Every bookmark made under this publication, in any format.
    QList<Bookmark> forPublication(qint64 publicationId);

private:
    QList<Bookmark> select(const char *where, const QString &value);
    Database &m_db;
};

} // namespace papertrail::core
