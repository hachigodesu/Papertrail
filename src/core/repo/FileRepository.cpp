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

QHash<qint64, qint64> FileRepository::fileCountsByRoot()
{
    QHash<qint64, qint64> counts;
    auto select = m_db.prepare("SELECT root_id, count(*) FROM file GROUP BY root_id");
    while (select->step())
        counts.insert(select->int64At(0), select->int64At(1));
    return counts;
}

QList<LibraryRoot> FileRepository::roots()
{
    QList<LibraryRoot> roots;
    auto select = m_db.prepare(
        "SELECT id, uuid, name, path FROM library_root WHERE id NOT IN (SELECT root_id FROM removed_root) ORDER BY name");
    while (select->step())
        roots << LibraryRoot{select->int64At(0), select->textAt(1), select->textAt(2), select->textAt(3)};
    return roots;
}

QList<LibraryRoot> FileRepository::removedRoots()
{
    QList<LibraryRoot> roots;
    auto select = m_db.prepare(
        "SELECT id, uuid, name, path FROM library_root WHERE id IN (SELECT root_id FROM removed_root) ORDER BY name");
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

FileRepository::RemovalImpact FileRepository::removalImpact(const QList<qint64> &rootIds)
{
    if (rootIds.isEmpty())
        return {};
    QStringList numbers;
    for (const qint64 id : rootIds)
        numbers << QString::number(id); // Numbers from the library itself, so they can go in the query as they are.
    const QByteArray list = numbers.join(u',').toLatin1();
    auto select = m_db.prepare(
        "WITH here AS (SELECT DISTINCT pf.publication_id AS id FROM publication_file pf "
        "              JOIN file f ON f.id = pf.file_id WHERE f.root_id IN (" + list + ")) "
        "SELECT count(*), coalesce(sum(EXISTS (SELECT 1 FROM publication_file o JOIN file g ON g.id = o.file_id "
        "    WHERE o.publication_id = here.id AND g.root_id NOT IN (" + list + ") "
        "      AND g.root_id NOT IN (SELECT root_id FROM removed_root))), 0) "
        "FROM here");
    select->step();
    const qint64 all = select->int64At(0);
    const qint64 staying = select->int64At(1);
    return {all - staying, staying};
}

FileRepository::RemovalImpact FileRepository::removalImpact(qint64 rootId)
{
    auto select = m_db.prepare(
        "WITH here AS (SELECT DISTINCT pf.publication_id AS id FROM publication_file pf "
        "              JOIN file f ON f.id = pf.file_id WHERE f.root_id = ?1) "
        "SELECT count(*), coalesce(sum(EXISTS (SELECT 1 FROM publication_file o JOIN file g ON g.id = o.file_id "
        "    WHERE o.publication_id = here.id AND g.root_id <> ?1 "
        "      AND g.root_id NOT IN (SELECT root_id FROM removed_root))), 0) "
        "FROM here");
    select->bind(1, rootId);
    select->step();
    const qint64 all = select->int64At(0);
    const qint64 staying = select->int64At(1);
    return {all - staying, staying};
}

void FileRepository::hideRoot(qint64 rootId)
{
    Transaction transaction(m_db);

    // Work still waiting on these files would otherwise bring the folder back into the library.
    auto jobs = m_db.prepare(
        "DELETE FROM job WHERE CASE WHEN json_valid(payload) THEN json_extract(payload, '$.file') END "
        "IN (SELECT id FROM file WHERE root_id = ?)");
    jobs->bind(1, rootId);
    jobs->step();

    // A file that never became a publication has nothing to keep, and is simply found again next time.
    auto unused = m_db.prepare(
        "DELETE FROM file WHERE root_id = ? AND id NOT IN (SELECT file_id FROM publication_file)");
    unused->bind(1, rootId);
    unused->step();

    auto mark = m_db.prepare("INSERT OR REPLACE INTO removed_root (root_id, removed_at) VALUES (?, ?)");
    mark->bind(1, rootId);
    mark->bind(2, QDateTime::currentMSecsSinceEpoch());
    mark->step();

    // Publications that are still in the library through another folder just lose this location.
    auto elsewhere = m_db.prepare(
        "DELETE FROM file WHERE root_id = ?1 AND id IN ("
        "  SELECT pf.file_id FROM publication_file pf WHERE EXISTS ("
        "    SELECT 1 FROM publication_file o JOIN file g ON g.id = o.file_id "
        "    WHERE o.publication_id = pf.publication_id AND g.root_id NOT IN (SELECT root_id FROM removed_root)))");
    elsewhere->bind(1, rootId);
    elsewhere->step();
    m_db.exec("UPDATE publication_file SET is_primary = 1 WHERE (publication_id, file_id) IN ("
              "SELECT publication_id, min(file_id) FROM publication_file "
              "GROUP BY publication_id HAVING max(is_primary) = 0)");

    auto hide = m_db.prepare(
        "WITH here AS (SELECT DISTINCT pf.publication_id AS id FROM publication_file pf "
        "              JOIN file f ON f.id = pf.file_id WHERE f.root_id = ?) "
        "INSERT OR IGNORE INTO hidden_publication (publication_id) "
        "SELECT id FROM here UNION SELECT c.id FROM publication c JOIN here ON c.parent_id = here.id");
    hide->bind(1, rootId);
    hide->step();

    // Nothing that's been hidden should be sent to anyone.
    m_db.exec("DELETE FROM job WHERE kind = 'lookup' AND json_extract(payload, '$.publication') "
              "IN (SELECT publication_id FROM hidden_publication)");
    transaction.commit();
}

void FileRepository::restoreRoot(qint64 rootId)
{
    Transaction transaction(m_db);
    auto unmark = m_db.prepare("DELETE FROM removed_root WHERE root_id = ?");
    unmark->bind(1, rootId);
    unmark->step();

    auto reveal = m_db.prepare(
        "WITH here AS (SELECT DISTINCT pf.publication_id AS id FROM publication_file pf "
        "              JOIN file f ON f.id = pf.file_id WHERE f.root_id = ?) "
        "DELETE FROM hidden_publication WHERE publication_id IN ("
        "SELECT id FROM here UNION SELECT c.id FROM publication c JOIN here ON c.parent_id = here.id)");
    reveal->bind(1, rootId);
    reveal->step();
    transaction.commit();
}

namespace {

// Publications imported from another computer whose files haven't been found here yet.
const char *const kWaiting = "SELECT p.id FROM publication p WHERE coalesce(p.parent_id, p.id) IN (SELECT publication_id FROM expected_file) AND coalesce(p.parent_id, p.id) NOT IN (SELECT publication_id FROM publication_file)";

} // namespace

qint64 FileRepository::hiddenCount()
{
    auto select = m_db.prepare(QByteArray("SELECT count(*) FROM hidden_publication WHERE publication_id NOT IN (") + kWaiting + ")");
    select->step();
    return select->int64At(0);
}

QList<qint64> FileRepository::hiddenPublications()
{
    QList<qint64> ids;
    auto select = m_db.prepare(QByteArray("SELECT publication_id FROM hidden_publication WHERE publication_id NOT IN (") + kWaiting + ")");
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

QList<qint64> FileRepository::hiddenIn(qint64 rootId)
{
    auto select = m_db.prepare(
        "WITH here AS (SELECT DISTINCT pf.publication_id AS id FROM publication_file pf "
        "              JOIN file f ON f.id = pf.file_id WHERE f.root_id = ? "
        "                AND pf.publication_id IN (SELECT publication_id FROM hidden_publication)) "
        "SELECT id FROM here UNION SELECT c.id FROM publication c JOIN here ON c.parent_id = here.id");
    select->bind(1, rootId);
    QList<qint64> ids;
    while (select->step())
        ids << select->int64At(0);
    return ids;
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

void FileRepository::expectFile(qint64 publicationId, const ExpectedFile &file)
{
    Statement &insert = m_db.cached(
        "INSERT INTO expected_file (publication_id, sha256, size, format, file_name, is_primary) VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (publication_id, sha256) DO UPDATE SET is_primary = max(is_primary, excluded.is_primary)");
    insert.bind(1, publicationId);
    insert.bind(2, file.sha256.toLower());
    insert.bind(3, file.size);
    insert.bind(4, file.format);
    insert.bind(5, file.fileName);
    insert.bind(6, file.primary ? 1 : 0);
    insert.step();
    insert.reset();
}

std::optional<qint64> FileRepository::linkWaitingPublication(qint64 fileId)
{
    const std::optional<FileRecord> file = get(fileId);
    if (!file || file->sha256.isEmpty() || file->size == 0)
        return std::nullopt;
    Statement &removed = m_db.cached("SELECT 1 FROM removed_root WHERE root_id = ?");
    removed.bind(1, file->rootId);
    const bool inRemovedFolder = removed.step();
    removed.reset();
    if (inRemovedFolder)
        return std::nullopt;

    Statement &waiting = m_db.cached(
        "SELECT publication_id, is_primary FROM expected_file WHERE sha256 = ? ORDER BY publication_id LIMIT 1");
    waiting.bind(1, file->sha256.toLower());
    if (!waiting.step()) {
        waiting.reset();
        return std::nullopt;
    }
    const qint64 publicationId = waiting.int64At(0);
    const bool primary = waiting.intAt(1) != 0;
    waiting.reset();

    Transaction transaction(m_db);
    link(publicationId, fileId, primary);
    Statement &done = m_db.cached("DELETE FROM expected_file WHERE publication_id = ? AND sha256 = ?");
    done.bind(1, publicationId);
    done.bind(2, file->sha256.toLower());
    done.step();
    done.reset();
    // Possible files suggested for it are no longer needed.
    Statement &proposals = m_db.cached("DELETE FROM file_proposal WHERE publication_id = ?");
    proposals.bind(1, publicationId);
    proposals.step();
    proposals.reset();
    transaction.commit();
    return publicationId;
}

void FileRepository::hideWaitingPublications()
{
    m_db.exec((QByteArray("INSERT OR IGNORE INTO hidden_publication (publication_id) ") + kWaiting).constData());
}

qint64 FileRepository::waitingPublicationCount()
{
    auto count = m_db.prepare("SELECT count(DISTINCT publication_id) FROM expected_file "
                              "WHERE publication_id NOT IN (SELECT publication_id FROM publication_file)");
    count->step();
    return count->int64At(0);
}

QList<qint64> FileRepository::waitingPublications()
{
    QList<qint64> ids;
    auto select = m_db.prepare(kWaiting);
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
