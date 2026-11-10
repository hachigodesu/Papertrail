#include "core/repo/LibraryCheck.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"

#include <QDateTime>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace papertrail::core {

namespace {

// In the library: not kept only for a removed folder, and in a folder still indexed.
const char *const kFileInLibrary = "f.root_id NOT IN (SELECT root_id FROM removed_root)";
const char *const kShownPublication = "p.id NOT IN (SELECT publication_id FROM hidden_publication)";

QList<CheckedFile> filesWithStatus(Database &db, const char *status)
{
    QList<CheckedFile> found;
    auto select = db.prepare(QByteArray(
        "SELECT f.id, p.id, p.title, f.relative_path, r.name, f.problem, "
        "(SELECT count(*) FROM publication_file other WHERE other.publication_id = p.id AND other.file_id <> f.id) "
        "FROM file f JOIN library_root r ON r.id = f.root_id "
        "JOIN publication_file pf ON pf.file_id = f.id JOIN publication p ON p.id = pf.publication_id "
        "WHERE f.status = ? AND ") + kFileInLibrary + " AND " + kShownPublication + " ORDER BY r.name, f.relative_path");
    select->bind(1, status);
    while (select->step()) {
        CheckedFile file;
        file.fileId = select->int64At(0);
        file.publicationId = select->int64At(1);
        file.title = select->textAt(2);
        file.path = select->textAt(3);
        file.folder = select->textAt(4);
        file.problem = select->textAt(5);
        file.otherFiles = select->int64At(6) > 0;
        found << file;
    }
    return found;
}

QList<SharedIdentifier> sharedIdentifiers(Database &db)
{
    struct Kind {
        const char *column;
        const char *label;
    };
    static const Kind kinds[] = {{"doi", "DOI"}, {"isbn13", "ISBN"}, {"isbn10", "ISBN"}, {"arxiv_id", "arXiv"}, {"pmid", "PubMed"}};

    // Pairs said to be different, and which publications are articles inside which.
    QSet<QPair<qint64, qint64>> distinct;
    {
        auto select = db.prepare("SELECT first_id, second_id FROM distinct_publications");
        while (select->step())
            distinct.insert({select->int64At(0), select->int64At(1)});
    }
    const auto pairOf = [](qint64 a, qint64 b) { return a < b ? QPair<qint64, qint64>{a, b} : QPair<qint64, qint64>{b, a}; };

    QList<SharedIdentifier> found;
    QSet<QList<qint64>> seen; // The same publications sharing a DOI and an ISBN are listed once.
    for (const Kind &kind : kinds) {
        auto groups = db.prepare(QByteArray("SELECT p.") + kind.column + " FROM publication p WHERE p." + kind.column
                                 + " IS NOT NULL AND p." + kind.column + " <> '' AND " + kShownPublication + " GROUP BY p."
                                 + kind.column + " HAVING count(*) > 1");
        while (groups->step()) {
            const QString value = groups->textAt(0);
            auto members = db.prepare(QByteArray(
                "SELECT p.id, p.title, p.parent_id, EXISTS (SELECT 1 FROM publication_file pf WHERE pf.publication_id = p.id) "
                "FROM publication p WHERE p.") + kind.column + " = ? AND " + kShownPublication + " ORDER BY p.id");
            members->bind(1, value);
            struct Member {
                qint64 id;
                QString title;
                qint64 parent;
                bool hasFile;
            };
            QList<Member> list;
            while (members->step())
                list << Member{members->int64At(0), members->textAt(1), members->isNull(2) ? 0 : members->int64At(2), members->int64At(3) != 0};
            // An issue and an article inside it may share an identifier, and pairs said to differ don't count.
            bool undecided = false;
            for (qsizetype i = 0; i < list.size() && !undecided; ++i) {
                for (qsizetype j = i + 1; j < list.size() && !undecided; ++j) {
                    const Member &a = list.at(i);
                    const Member &b = list.at(j);
                    const bool related = a.parent == b.id || b.parent == a.id;
                    undecided = !related && !distinct.contains(pairOf(a.id, b.id));
                }
            }
            if (!undecided)
                continue;
            // The one to keep first: one with a file, then the oldest.
            std::stable_sort(list.begin(), list.end(), [](const Member &a, const Member &b) { return a.hasFile && !b.hasFile; });
            SharedIdentifier shared;
            shared.kind = QString::fromLatin1(kind.label);
            shared.value = value;
            for (const Member &member : std::as_const(list)) {
                shared.publicationIds << member.id;
                shared.titles << member.title;
            }
            QList<qint64> key = shared.publicationIds;
            std::sort(key.begin(), key.end());
            if (seen.contains(key))
                continue;
            seen.insert(key);
            found << shared;
        }
    }
    return found;
}

} // namespace

LibraryCheckResult checkLibrary(Database &db)
{
    LibraryCheckResult result;
    result.missing = filesWithStatus(db, "missing");
    result.unreadable = filesWithStatus(db, "unreadable");

    {
        auto select = db.prepare(QByteArray("SELECT DISTINCT r.name FROM file f JOIN library_root r ON r.id = f.root_id "
                                            "WHERE f.status = 'offline' AND ") + kFileInLibrary + " ORDER BY r.name");
        while (select->step())
            result.offlineFolders << select->textAt(0);
    }

    {
        // Those still waiting are hidden until their files come, so they're found by what they wait for instead.
        const QList<qint64> waiting = FileRepository(db).waitingPublications();
        auto details = db.prepare(
            "SELECT p.title, (SELECT e.file_name FROM expected_file e WHERE e.publication_id = p.id ORDER BY e.is_primary DESC LIMIT 1), "
            "EXISTS (SELECT 1 FROM file_proposal fp WHERE fp.publication_id = p.id) FROM publication p "
            "WHERE p.id = ? AND p.parent_id IS NULL");
        for (const qint64 id : waiting) {
            details->reset();
            details->bind(1, id);
            if (!details->step())
                continue; // An article waits with its issue, and is listed with it.
            result.waiting << WaitingForFile{id, details->textAt(0), details->textAt(1), details->int64At(2) != 0};
        }
    }

    {
        auto select = db.prepare(QByteArray(
            "SELECT f.id, f.relative_path, r.name FROM file f JOIN library_root r ON r.id = f.root_id "
            "WHERE f.status = 'ok' AND f.sha256 IS NOT NULL AND f.sha256 <> '' AND ") + kFileInLibrary + " "
            "AND NOT EXISTS (SELECT 1 FROM publication_file pf WHERE pf.file_id = f.id) "
            // Still being read, or kept out because its publication was removed.
            "AND NOT EXISTS (SELECT 1 FROM job j WHERE j.status IN ('pending', 'running') AND j.kind <> 'lookup' "
            "AND json_extract(j.payload, '$.file') = f.id) "
            "AND f.sha256 NOT IN (SELECT sha256 FROM removed_content WHERE sha256 IS NOT NULL) "
            "AND f.id NOT IN (SELECT file_id FROM removed_content WHERE file_id IS NOT NULL) "
            "ORDER BY r.name, f.relative_path");
        while (select->step()) {
            CheckedFile file;
            file.fileId = select->int64At(0);
            file.path = select->textAt(1);
            file.folder = select->textAt(2);
            result.unattached << file;
        }
    }

    result.shared = sharedIdentifiers(db);
    return result;
}

void markDistinct(Database &db, const QList<qint64> &publicationIds)
{
    Transaction transaction(db);
    auto insert = db.prepare("INSERT OR IGNORE INTO distinct_publications (first_id, second_id, created_at) VALUES (?, ?, ?)");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (qsizetype i = 0; i < publicationIds.size(); ++i) {
        for (qsizetype j = i + 1; j < publicationIds.size(); ++j) {
            const qint64 a = publicationIds.at(i);
            const qint64 b = publicationIds.at(j);
            if (a == b)
                continue;
            insert->reset();
            insert->bind(1, qMin(a, b));
            insert->bind(2, qMax(a, b));
            insert->bind(3, now);
            insert->step();
        }
    }
    transaction.commit();
}

bool areDistinct(Database &db, qint64 a, qint64 b)
{
    auto select = db.prepare("SELECT 1 FROM distinct_publications WHERE first_id = ? AND second_id = ?");
    select->bind(1, qMin(a, b));
    select->bind(2, qMax(a, b));
    return select->step();
}

void forgetFileLocation(Database &db, qint64 fileId)
{
    Transaction transaction(db);
    // Only where its publication has another file to keep it, so nothing's left with no file by mistake.
    auto check = db.prepare("SELECT count(*) FROM publication_file pf WHERE pf.file_id = ?1 AND NOT EXISTS "
                            "(SELECT 1 FROM publication_file other WHERE other.publication_id = pf.publication_id AND other.file_id <> ?1)");
    check->bind(1, fileId);
    check->step();
    if (check->int64At(0) > 0)
        return;
    check.reset();
    auto primaries = db.prepare("SELECT publication_id FROM publication_file WHERE file_id = ? AND is_primary = 1");
    primaries->bind(1, fileId);
    QList<qint64> needPrimary;
    while (primaries->step())
        needPrimary << primaries->int64At(0);
    primaries.reset();
    auto jobs = db.prepare("DELETE FROM job WHERE kind <> 'lookup' AND json_extract(payload, '$.file') = ?");
    jobs->bind(1, fileId);
    jobs->step();
    auto remove = db.prepare("DELETE FROM file WHERE id = ?");
    remove->bind(1, fileId);
    remove->step();
    // Another of its files becomes the one its cover comes from.
    auto primary = db.prepare("UPDATE publication_file SET is_primary = 1 WHERE publication_id = ?1 AND file_id = "
                              "(SELECT file_id FROM publication_file WHERE publication_id = ?1 ORDER BY file_id LIMIT 1)");
    for (const qint64 publication : std::as_const(needPrimary)) {
        primary->reset();
        primary->bind(1, publication);
        primary->step();
    }
    transaction.commit();
}

void readFileAgain(Database &db, qint64 fileId)
{
    Transaction transaction(db);
    // Its failed jobs are forgotten, and without a hash the next scan reads it from the start.
    auto jobs = db.prepare("DELETE FROM job WHERE kind <> 'lookup' AND status = 'failed' AND json_extract(payload, '$.file') = ?");
    jobs->bind(1, fileId);
    jobs->step();
    auto update = db.prepare("UPDATE file SET status = 'ok', problem = NULL, sha256 = NULL WHERE id = ? AND status IN ('ok', 'unreadable')");
    update->bind(1, fileId);
    update->step();
    transaction.commit();
}

void keepFileOut(Database &db, qint64 fileId, qint64 when)
{
    auto select = db.prepare("SELECT sha256, size, relative_path FROM file WHERE id = ?");
    select->bind(1, fileId);
    if (!select->step())
        return;
    const QString sha256 = select->textAt(0);
    const qint64 size = select->int64At(1);
    const QString name = select->textAt(2).section(u'/', -1);
    select.reset();
    // By contents where they're known, as RemovedContent keeps them, so the same file elsewhere stays out too.
    const bool hashed = !sha256.isEmpty() && size > 0;
    auto insert = db.prepare(hashed ? "INSERT INTO removed_content (sha256, title, file_name, removed_at) VALUES (?, ?, ?, ?) "
                                      "ON CONFLICT (sha256) DO NOTHING"
                                    : "INSERT INTO removed_content (file_id, title, file_name, removed_at) VALUES (?, ?, ?, ?) "
                                      "ON CONFLICT (file_id) DO NOTHING");
    if (hashed)
        insert->bind(1, sha256);
    else
        insert->bind(1, fileId);
    insert->bind(2, name);
    insert->bind(3, name);
    insert->bind(4, when);
    insert->step();
}

} // namespace papertrail::core
