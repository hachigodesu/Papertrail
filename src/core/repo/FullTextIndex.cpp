#include "core/repo/FullTextIndex.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace papertrail::core {

namespace {

const char *const kPendingFiles =
    " FROM file f WHERE f.sha256 IS NOT NULL AND f.sha256 <> '' AND f.status = 'ok' "
    "AND f.root_id NOT IN (SELECT root_id FROM removed_root) "
    "AND f.id IN (SELECT file_id FROM publication_file) "
    "AND NOT EXISTS (SELECT 1 FROM fulltext.document d WHERE d.sha256 = f.sha256 AND d.complete = 1)";

void deleteDocumentPages(Database &db, qint64 documentId)
{
    auto remove = db.prepare("DELETE FROM fulltext.page_text WHERE rowid >= ? AND rowid < ?");
    remove->bind(1, documentId * kFullTextPagesPerDocument);
    remove->bind(2, (documentId + 1) * kFullTextPagesPerDocument);
    remove->step();
}

} // namespace

QString fullTextPathFor(const QString &libraryPath)
{
    return QFileInfo(libraryPath).dir().filePath(QStringLiteral("fulltext.db"));
}

bool hasFullText(Database &db)
{
    auto select = db.prepare("SELECT 1 FROM pragma_database_list WHERE name = 'fulltext'");
    return select->step();
}

void attachFullText(Database &db, const QString &path)
{
    if (hasFullText(db))
        return;
    auto attach = db.prepare("ATTACH DATABASE ? AS fulltext");
    attach->bind(1, path);
    attach->step();
    attach.reset();

    db.exec("PRAGMA fulltext.journal_mode = WAL");
    db.exec("PRAGMA fulltext.synchronous = NORMAL");
    db.exec("CREATE TABLE IF NOT EXISTS fulltext.document ("
            "  id INTEGER PRIMARY KEY,"
            "  sha256 TEXT NOT NULL UNIQUE,"
            "  page_count INTEGER NOT NULL DEFAULT 0,"
            "  problem TEXT NOT NULL DEFAULT '',"
            "  complete INTEGER NOT NULL DEFAULT 0,"
            "  indexed_at INTEGER NOT NULL) STRICT");
    db.exec("CREATE VIRTUAL TABLE IF NOT EXISTS fulltext.page_text USING fts5("
            "  text, tokenize = \"unicode61 remove_diacritics 2\")");
    // How far recognising a scanned document's words has got, with which languages. See import::TextRecognition.
    db.exec("CREATE TABLE IF NOT EXISTS fulltext.recognition ("
            "  sha256 TEXT PRIMARY KEY,"
            "  languages TEXT NOT NULL,"
            "  next_page INTEGER NOT NULL DEFAULT 0,"
            "  page_count INTEGER NOT NULL DEFAULT 0,"
            "  recognised INTEGER NOT NULL DEFAULT 0,"
            "  done INTEGER NOT NULL DEFAULT 0,"
            "  problem TEXT NOT NULL DEFAULT '',"
            "  updated_at INTEGER NOT NULL) STRICT");
    // Documents whose every page is to be recognised, whatever text they have, since their own is poor.
    db.exec("CREATE TABLE IF NOT EXISTS fulltext.recognition_wanted ("
            "  sha256 TEXT PRIMARY KEY,"
            "  wanted_at INTEGER NOT NULL) STRICT");
}

void FullTextIndex::forget(const QString &sha256)
{
    Transaction transaction(m_db);
    auto find = m_db.prepare("SELECT id FROM fulltext.document WHERE sha256 = ?");
    find->bind(1, sha256);
    if (find->step()) {
        const qint64 id = find->int64At(0);
        deleteDocumentPages(m_db, id);
        auto remove = m_db.prepare("DELETE FROM fulltext.document WHERE id = ?");
        remove->bind(1, id);
        remove->step();
    }
    transaction.commit();
}

std::optional<FullTextIndex::Pending> FullTextIndex::nextPending(qint64 afterFileId)
{
    auto select = m_db.prepare(QByteArray("SELECT f.id, f.sha256") + kPendingFiles + " AND f.id > ? ORDER BY f.id LIMIT 1");
    select->bind(1, afterFileId);
    if (!select->step())
        return std::nullopt;
    return Pending{select->int64At(0), select->textAt(1)};
}

qint64 FullTextIndex::pendingCount()
{
    auto select = m_db.prepare(QByteArray("SELECT count(DISTINCT f.sha256)") + kPendingFiles);
    select->step();
    return select->int64At(0);
}

qint64 FullTextIndex::indexedCount()
{
    auto select = m_db.prepare("SELECT count(*) FROM fulltext.document WHERE complete = 1");
    select->step();
    return select->int64At(0);
}

qint64 FullTextIndex::beginDocument(const QString &sha256)
{
    Transaction transaction(m_db);
    auto find = m_db.prepare("SELECT id FROM fulltext.document WHERE sha256 = ?");
    find->bind(1, sha256);
    qint64 id = 0;
    if (find->step()) {
        id = find->int64At(0);
        deleteDocumentPages(m_db, id); // What's left from being interrupted last time.
    } else {
        auto insert = m_db.prepare("INSERT INTO fulltext.document (sha256, indexed_at) VALUES (?, ?)");
        insert->bind(1, sha256);
        insert->bind(2, QDateTime::currentMSecsSinceEpoch());
        insert->step();
        id = m_db.lastInsertId();
    }
    transaction.commit();
    return id;
}

void FullTextIndex::addPages(qint64 documentId, int firstPage, const QStringList &texts)
{
    Transaction transaction(m_db);
    Statement &insert = m_db.cached("INSERT INTO fulltext.page_text (rowid, text) VALUES (?, ?)");
    for (qsizetype i = 0; i < texts.size(); ++i) {
        if (texts.at(i).trimmed().isEmpty())
            continue; // An empty page has nothing to find.
        insert.bind(1, documentId * kFullTextPagesPerDocument + firstPage + i);
        insert.bind(2, tidiedPageText(texts.at(i)));
        insert.step();
        insert.reset();
    }
    transaction.commit();
}

void FullTextIndex::finishDocument(qint64 documentId, int pageCount, const QString &problem)
{
    auto update = m_db.prepare(
        "UPDATE fulltext.document SET page_count = ?, problem = ?, complete = 1, indexed_at = ? WHERE id = ?");
    update->bind(1, pageCount);
    update->bind(2, problem.isNull() ? QStringLiteral("") : problem);
    update->bind(3, QDateTime::currentMSecsSinceEpoch());
    update->bind(4, documentId);
    update->step();
}

int FullTextIndex::removeOrphans()
{
    QList<qint64> orphans;
    {
        auto select = m_db.prepare(
            "SELECT d.id FROM fulltext.document d WHERE NOT EXISTS (SELECT 1 FROM file f WHERE f.sha256 = d.sha256)");
        while (select->step())
            orphans << select->int64At(0);
    }
    for (const qint64 id : std::as_const(orphans)) {
        Transaction transaction(m_db);
        deleteDocumentPages(m_db, id);
        auto remove = m_db.prepare("DELETE FROM fulltext.document WHERE id = ?");
        remove->bind(1, id);
        remove->step();
        transaction.commit();
    }
    return int(orphans.size());
}

QString FullTextIndex::problemFor(const QString &sha256)
{
    auto select = m_db.prepare("SELECT problem FROM fulltext.document WHERE sha256 = ?");
    select->bind(1, sha256);
    return select->step() ? select->textAt(0) : QString();
}

QList<ContentMatch> FullTextIndex::matchesFor(qint64 publicationId, const QString &match, int limit)
{
    QList<ContentMatch> matches;
    if (match.isEmpty() || !hasFullText(m_db))
        return matches;
    PublicationRepository publications(m_db);
    const std::optional<Publication> publication = publications.get(publicationId);
    if (!publication)
        return matches;

    // An article only looks at its own pages of its volume's file.
    const bool isArticle = publication->parentId.has_value();
    const int firstPage = isArticle && publication->pageStart ? *publication->pageStart - 1 : 0;
    const int lastPage = isArticle && publication->pageEnd ? *publication->pageEnd - 1 : int(kFullTextPagesPerDocument - 1);

    FileRepository files(m_db);
    QSet<QString> seen;
    auto findDocument = m_db.prepare("SELECT id FROM fulltext.document WHERE sha256 = ?");
    auto search = m_db.prepare(
        "SELECT rowid, snippet(page_text, 0, '', '', ' ... ', 24) FROM fulltext.page_text "
        "WHERE page_text MATCH ? AND rowid >= ? AND rowid <= ? ORDER BY rowid LIMIT ?");
    for (const FileRecord &file : files.filesFor(publication->parentId.value_or(publicationId))) {
        if (file.sha256.isEmpty() || seen.contains(file.sha256))
            continue;
        seen.insert(file.sha256);

        findDocument->reset();
        findDocument->bind(1, file.sha256);
        if (!findDocument->step())
            continue;
        const qint64 base = findDocument->int64At(0) * kFullTextPagesPerDocument;

        search->reset();
        search->bind(1, match);
        search->bind(2, base + firstPage);
        search->bind(3, base + lastPage);
        search->bind(4, limit - int(matches.size()));
        while (search->step()) {
            matches << ContentMatch{file.id, file.format, int(search->int64At(0) - base), search->textAt(1).simplified()};
        }
        if (matches.size() >= limit)
            break;
    }
    return matches;
}

QString tidiedPageText(const QString &text)
{
    // Only before a lower-case letter, so a hyphenated name at the end of a line stays as it is.
    static const QRegularExpression splitWord(QStringLiteral("(\\p{L})-[ \\t]*\\r?\\n[ \\t]*(\\p{Ll})"));
    QString tidied = text;
    tidied.replace(splitWord, QStringLiteral("\\1\\2"));
    return tidied;
}

} // namespace papertrail::core
