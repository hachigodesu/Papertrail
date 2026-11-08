#include "core/repo/RemovedContent.h"

#include "core/db/Database.h"
#include "core/repo/PublicationRepository.h"

#include <QFileInfo>

namespace papertrail::core {

RemovedContent::RemovedContent(Database &db)
    : m_db(db)
{
}

void RemovedContent::remove(const QList<qint64> &publicationIds, qint64 when)
{
    Transaction transaction(m_db);
    PublicationRepository publications(m_db);
    auto files = m_db.prepare(
        "SELECT f.id, f.sha256, f.size, f.relative_path, p.title FROM publication p "
        "JOIN publication_file pf ON pf.publication_id = p.id JOIN file f ON f.id = pf.file_id WHERE p.id = ?");
    // By contents where they're known. An empty file's contents are every empty file's, so that's kept by itself.
    auto byContents = m_db.prepare(
        "INSERT INTO removed_content (sha256, title, file_name, removed_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (sha256) DO UPDATE SET title = excluded.title, file_name = excluded.file_name, removed_at = excluded.removed_at");
    auto byFile = m_db.prepare(
        "INSERT INTO removed_content (file_id, title, file_name, removed_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (file_id) DO UPDATE SET title = excluded.title, file_name = excluded.file_name, removed_at = excluded.removed_at");
    for (const qint64 id : publicationIds) {
        files->reset();
        files->bind(1, id);
        while (files->step()) {
            const QString sha256 = files->textAt(1);
            const bool hashed = !sha256.isEmpty() && files->int64At(2) > 0;
            Statement &insert = hashed ? *byContents : *byFile;
            insert.reset();
            if (hashed)
                insert.bind(1, sha256);
            else
                insert.bind(1, files->int64At(0));
            insert.bind(2, files->textAt(4));
            insert.bind(3, QFileInfo(files->textAt(3)).fileName());
            insert.bind(4, when);
            insert.step();
        }
        publications.remove(id); // Its articles, links, tags, notes and highlights go with it.
    }
    transaction.commit();
}

bool RemovedContent::covers(qint64 fileId, const QString &sha256, qint64 size)
{
    Statement &check = m_db.cached(
        "SELECT EXISTS (SELECT 1 FROM removed_content WHERE file_id = ?1 OR (?3 > 0 AND ?2 IS NOT NULL AND sha256 = ?2))");
    check.bind(1, fileId);
    check.bind(2, sha256.isEmpty() ? QString() : sha256);
    check.bind(3, size);
    check.step();
    const bool removed = check.intAt(0) != 0;
    check.reset();
    return removed;
}

QList<RemovedEntry> RemovedContent::entries()
{
    QList<RemovedEntry> found;
    auto select = m_db.prepare(
        "SELECT id, title, file_name, removed_at FROM removed_content ORDER BY removed_at DESC, title COLLATE NOCASE");
    while (select->step())
        found.append({select->int64At(0), select->textAt(1), select->textAt(2), select->int64At(3)});
    return found;
}

qint64 RemovedContent::count()
{
    auto select = m_db.prepare("SELECT count(*) FROM removed_content");
    select->step();
    return select->int64At(0);
}

int RemovedContent::bringBack(const QList<qint64> &entryIds)
{
    Transaction transaction(m_db);
    int files = 0;
    // Forgetting the files' contents has the next scan read them again, as new files are.
    auto forget = m_db.prepare(
        "UPDATE file SET sha256 = NULL WHERE id NOT IN (SELECT file_id FROM publication_file) AND ("
        "id = (SELECT file_id FROM removed_content WHERE id = ?1) OR "
        "sha256 = (SELECT sha256 FROM removed_content WHERE id = ?1))");
    auto drop = m_db.prepare("DELETE FROM removed_content WHERE id = ?");
    for (const qint64 id : entryIds) {
        forget->reset();
        forget->bind(1, id);
        forget->step();
        files += m_db.changes();
        drop->reset();
        drop->bind(1, id);
        drop->step();
    }
    transaction.commit();
    return files;
}

} // namespace papertrail::core
