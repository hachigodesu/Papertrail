#include "core/repo/BookmarkRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QUuid>

namespace papertrail::core {

namespace {

const char *const kColumns = "SELECT id, uuid, publication_id, content_hash, format, page, y, place, label, created_at, updated_at "
                             "FROM bookmark ";

Bookmark readBookmark(Statement &select)
{
    Bookmark bookmark;
    bookmark.id = select.int64At(0);
    bookmark.uuid = select.textAt(1);
    bookmark.publicationId = select.int64At(2);
    bookmark.contentHash = select.textAt(3);
    bookmark.format = select.textAt(4);
    bookmark.page = select.intAt(5);
    bookmark.y = select.doubleAt(6);
    bookmark.place = select.textAt(7);
    bookmark.label = select.textAt(8);
    bookmark.createdAt = select.int64At(9);
    bookmark.updatedAt = select.int64At(10);
    return bookmark;
}

} // namespace

qint64 BookmarkRepository::create(Bookmark &bookmark)
{
    if (bookmark.uuid.isEmpty())
        bookmark.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (bookmark.createdAt == 0)
        bookmark.createdAt = now;
    bookmark.updatedAt = now;
    auto insert = m_db.prepare("INSERT INTO bookmark (uuid, publication_id, content_hash, format, page, y, place, label, "
                               "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert->bind(1, bookmark.uuid);
    insert->bind(2, bookmark.publicationId);
    if (bookmark.contentHash.isEmpty())
        insert->bindNull(3);
    else
        insert->bind(3, bookmark.contentHash);
    insert->bind(4, bookmark.format);
    insert->bind(5, bookmark.page);
    insert->bind(6, bookmark.y);
    if (bookmark.place.isEmpty())
        insert->bindNull(7);
    else
        insert->bind(7, bookmark.place);
    insert->bind(8, bookmark.label);
    insert->bind(9, bookmark.createdAt);
    insert->bind(10, bookmark.updatedAt);
    insert->step();
    bookmark.id = m_db.lastInsertId();
    return bookmark.id;
}

std::optional<Bookmark> BookmarkRepository::get(qint64 id)
{
    auto select = m_db.prepare(QByteArray(kColumns) + "WHERE id = ?");
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return readBookmark(*select);
}

void BookmarkRepository::rename(qint64 id, const QString &label)
{
    auto update = m_db.prepare("UPDATE bookmark SET label = ?, updated_at = ? WHERE id = ?");
    update->bind(1, label.simplified());
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, id);
    update->step();
}

void BookmarkRepository::setPage(qint64 id, int page)
{
    auto update = m_db.prepare("UPDATE bookmark SET page = ? WHERE id = ?");
    update->bind(1, page);
    update->bind(2, id);
    update->step();
}

void BookmarkRepository::remove(qint64 id)
{
    auto remove = m_db.prepare("DELETE FROM bookmark WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

QList<Bookmark> BookmarkRepository::forContents(const QString &contentHash)
{
    if (contentHash.isEmpty())
        return {};
    return select("WHERE content_hash = ? ORDER BY page, y, id", contentHash);
}

QList<Bookmark> BookmarkRepository::forPublication(qint64 publicationId)
{
    auto select = m_db.prepare(QByteArray(kColumns) + "WHERE publication_id = ? ORDER BY page, y, id");
    select->bind(1, publicationId);
    QList<Bookmark> bookmarks;
    while (select->step())
        bookmarks << readBookmark(*select);
    return bookmarks;
}

QList<Bookmark> BookmarkRepository::select(const char *where, const QString &value)
{
    auto select = m_db.prepare(QByteArray(kColumns) + where);
    select->bind(1, value);
    QList<Bookmark> bookmarks;
    while (select->step())
        bookmarks << readBookmark(*select);
    return bookmarks;
}

} // namespace papertrail::core
