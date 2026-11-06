#include "core/repo/FileRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include <stdexcept>

namespace papertrail::core {

namespace {

QString toDbString(FileStatus status)
{
    switch (status) {
    case FileStatus::Ok: return QStringLiteral("ok");
    case FileStatus::Missing: return QStringLiteral("missing");
    case FileStatus::Offline: return QStringLiteral("offline");
    case FileStatus::Unreadable: return QStringLiteral("unreadable");
    }
    return QStringLiteral("ok");
}

FileStatus fileStatusFromDb(const QString &value)
{
    if (value == QStringLiteral("missing"))
        return FileStatus::Missing;
    if (value == QStringLiteral("offline"))
        return FileStatus::Offline;
    if (value == QStringLiteral("unreadable"))
        return FileStatus::Unreadable;
    return FileStatus::Ok;
}

const char *const kFileColumns =
    "f.id, f.root_id, f.relative_path, f.size, f.modified_at, f.sha256, f.format, "
    "f.page_count, f.has_text_layer, f.status, f.problem";

FileRecord readFile(const Statement &select)
{
    FileRecord file;
    file.id = select.int64At(0);
    file.rootId = select.int64At(1);
    file.relativePath = select.textAt(2);
    file.size = select.int64At(3);
    file.modifiedAt = select.int64At(4);
    file.sha256 = select.textAt(5);
    file.format = select.textAt(6);
    file.pageCount = select.optIntAt(7);
    if (!select.isNull(8))
        file.hasTextLayer = select.intAt(8) != 0;
    file.status = fileStatusFromDb(select.textAt(9));
    file.problem = select.textAt(10);
    return file;
}

QByteArray fileSelect(const char *rest)
{
    return QByteArray("SELECT ") + kFileColumns + " FROM file f " + rest;
}

} // namespace

qint64 FileRepository::addRoot(const QString &name, const QString &path)
{
    auto insert = m_db.prepare(
        "INSERT INTO library_root (uuid, name, path, created_at) VALUES (?, ?, ?, ?)");
    insert->bind(1, QUuid::createUuid().toString(QUuid::WithoutBraces));
    insert->bind(2, name);
    insert->bind(3, QDir::fromNativeSeparators(path));
    insert->bind(4, QDateTime::currentMSecsSinceEpoch());
    insert->step();
    return m_db.lastInsertId();
}

QList<LibraryRoot> FileRepository::roots()
{
    QList<LibraryRoot> roots;
    auto select = m_db.prepare("SELECT id, uuid, name, path FROM library_root ORDER BY name");
    while (select->step())
        roots << LibraryRoot{select->int64At(0), select->textAt(1), select->textAt(2), select->textAt(3)};
    return roots;
}

void FileRepository::repointRoot(qint64 rootId, const QString &newPath)
{
    auto update = m_db.prepare("UPDATE library_root SET path = ? WHERE id = ?");
    update->bind(1, QDir::fromNativeSeparators(newPath));
    update->bind(2, rootId);
    update->step();
}

void FileRepository::setRootOnline(qint64 rootId, bool online)
{
    auto update = m_db.prepare("UPDATE file SET status = ?1 WHERE root_id = ?2 AND status = ?3");
    update->bind(1, online ? QStringLiteral("ok") : QStringLiteral("offline"));
    update->bind(2, rootId);
    update->bind(3, online ? QStringLiteral("offline") : QStringLiteral("ok"));
    update->step();
}

void FileRepository::removeRoot(qint64 rootId)
{
    auto remove = m_db.prepare("DELETE FROM library_root WHERE id = ?");
    remove->bind(1, rootId);
    remove->step();
}

qint64 FileRepository::upsert(FileRecord &file)
{
    file.relativePath = QDir::fromNativeSeparators(file.relativePath);
    Statement &upsert = m_db.cached(
        "INSERT INTO file (root_id, relative_path, file_name, size, modified_at, sha256, format, "
        "page_count, has_text_layer, status, problem) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (root_id, relative_path) DO UPDATE SET "
        "size = excluded.size, modified_at = excluded.modified_at, sha256 = excluded.sha256, "
        "format = excluded.format, page_count = excluded.page_count, "
        "has_text_layer = excluded.has_text_layer, status = excluded.status, problem = excluded.problem "
        "RETURNING id");
    upsert.bind(1, file.rootId);
    upsert.bind(2, file.relativePath);
    upsert.bind(3, QFileInfo(file.relativePath).fileName());
    upsert.bind(4, file.size);
    upsert.bind(5, file.modifiedAt);
    upsert.bind(6, file.sha256);
    upsert.bind(7, file.format);
    upsert.bind(8, file.pageCount);
    if (file.hasTextLayer)
        upsert.bind(9, *file.hasTextLayer ? 1 : 0);
    else
        upsert.bindNull(9);
    upsert.bind(10, toDbString(file.status));
    upsert.bind(11, file.problem);
    upsert.step();
    file.id = upsert.int64At(0);
    upsert.reset();
    return file.id;
}

std::optional<FileRecord> FileRepository::get(qint64 id)
{
    auto select = m_db.prepare(fileSelect("WHERE f.id = ?"));
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return readFile(*select);
}

QList<FileRecord> FileRepository::withHash(const QString &sha256)
{
    QList<FileRecord> files;
    auto select = m_db.prepare(fileSelect("WHERE f.sha256 = ?"));
    select->bind(1, sha256);
    while (select->step())
        files << readFile(*select);
    return files;
}

void FileRepository::setHash(qint64 fileId, const QString &sha256)
{
    auto update = m_db.prepare("UPDATE file SET sha256 = ? WHERE id = ?");
    update->bind(1, sha256);
    update->bind(2, fileId);
    update->step();
}

void FileRepository::setStatus(qint64 fileId, FileStatus status, const QString &problem)
{
    auto update = m_db.prepare("UPDATE file SET status = ?, problem = ? WHERE id = ?");
    update->bind(1, toDbString(status));
    update->bind(2, problem);
    update->bind(3, fileId);
    update->step();
}

void FileRepository::setDocumentFacts(qint64 fileId, std::optional<int> pageCount, std::optional<bool> hasTextLayer)
{
    auto update = m_db.prepare("UPDATE file SET page_count = ?, has_text_layer = ? WHERE id = ?");
    update->bind(1, pageCount);
    if (hasTextLayer)
        update->bind(2, *hasTextLayer ? 1 : 0);
    else
        update->bindNull(2);
    update->bind(3, fileId);
    update->step();
}

QList<FileRecord> FileRepository::filesInRoot(qint64 rootId)
{
    QList<FileRecord> files;
    auto select = m_db.prepare(fileSelect("WHERE f.root_id = ?"));
    select->bind(1, rootId);
    while (select->step())
        files << readFile(*select);
    return files;
}

std::optional<LibraryRoot> FileRepository::root(qint64 rootId)
{
    auto select = m_db.prepare("SELECT id, uuid, name, path FROM library_root WHERE id = ?");
    select->bind(1, rootId);
    if (!select->step())
        return std::nullopt;
    return LibraryRoot{select->int64At(0), select->textAt(1), select->textAt(2), select->textAt(3)};
}

void FileRepository::replaceFile(qint64 oldFileId, qint64 newFileId)
{
    Transaction transaction(m_db);
    auto move = m_db.prepare(
        "INSERT OR IGNORE INTO publication_file (publication_id, file_id, is_primary) "
        "SELECT publication_id, ?2, is_primary FROM publication_file WHERE file_id = ?1");
    move->bind(1, oldFileId);
    move->bind(2, newFileId);
    move->step();

    auto remove = m_db.prepare("DELETE FROM file WHERE id = ?");
    remove->bind(1, oldFileId);
    remove->step();
    transaction.commit();
}

QList<qint64> FileRepository::publicationsFor(qint64 fileId)
{
    QList<qint64> ids;
    auto select = m_db.prepare("SELECT publication_id FROM publication_file WHERE file_id = ?");
    select->bind(1, fileId);
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

void FileRepository::link(qint64 publicationId, qint64 fileId, bool primary)
{
    Transaction transaction(m_db);
    if (primary) {
        Statement &clear = m_db.cached(
            "UPDATE publication_file SET is_primary = 0 WHERE publication_id = ?");
        clear.bind(1, publicationId);
        clear.step();
        clear.reset();
    }
    Statement &insert = m_db.cached(
        "INSERT INTO publication_file (publication_id, file_id, is_primary) VALUES (?, ?, ?) "
        "ON CONFLICT (publication_id, file_id) DO UPDATE SET is_primary = excluded.is_primary");
    insert.bind(1, publicationId);
    insert.bind(2, fileId);
    insert.bind(3, primary ? 1 : 0);
    insert.step();
    insert.reset();
    transaction.commit();
}

void FileRepository::unlink(qint64 publicationId, qint64 fileId)
{
    auto remove = m_db.prepare("DELETE FROM publication_file WHERE publication_id = ? AND file_id = ?");
    remove->bind(1, publicationId);
    remove->bind(2, fileId);
    remove->step();
}

QList<FileRecord> FileRepository::filesFor(qint64 publicationId)
{
    QList<FileRecord> files;
    auto select = m_db.prepare(fileSelect(
        "JOIN publication_file pf ON pf.file_id = f.id "
        "WHERE pf.publication_id = ? ORDER BY pf.is_primary DESC, f.id"));
    select->bind(1, publicationId);
    while (select->step())
        files << readFile(*select);
    return files;
}

QString FileRepository::absolutePath(qint64 fileId)
{
    auto select = m_db.prepare(
        "SELECT r.path, f.relative_path FROM file f JOIN library_root r ON r.id = f.root_id "
        "WHERE f.id = ?");
    select->bind(1, fileId);
    if (!select->step())
        throw std::invalid_argument("file doesn't exist");
    return QDir(select->textAt(0)).filePath(select->textAt(1));
}

} // namespace papertrail::core
