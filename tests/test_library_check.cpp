// Checking the library: what's found, what's left out, and what each fix does to Papertrail's records alone.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/repo/FileRepository.h"
#include "core/repo/LibraryCheck.h"
#include "core/repo/PublicationMerge.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/RemovedContent.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace papertrail::core;

class LibraryCheckTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        m_root = FileRepository(*m_db).addRoot(QStringLiteral("Papers"), m_dir->filePath(QStringLiteral("papers")));
    }

    void cleanup()
    {
        m_db.reset();
        m_dir.reset();
    }

    void aHealthyLibraryHasNothingToSay()
    {
        addWithFile(QStringLiteral("A Made-Up Paper"), QStringLiteral("a.pdf"), QStringLiteral("ok"), QStringLiteral("aaa"));
        QCOMPARE(checkLibrary(*m_db).problems(), qsizetype(0));
    }

    void missingAndUnreadableFilesAreFound()
    {
        const qint64 missing = addWithFile(QStringLiteral("A Missing Paper"), QStringLiteral("gone.pdf"), QStringLiteral("missing"), QStringLiteral("m1"));
        const qint64 unreadable = addWithFile(QStringLiteral("A Broken Paper"), QStringLiteral("broken.pdf"), QStringLiteral("unreadable"), QString());
        m_db->exec("UPDATE file SET problem = 'the file is damaged' WHERE relative_path = 'broken.pdf'");
        addWithFile(QStringLiteral("On A Drive Unplugged"), QStringLiteral("away.pdf"), QStringLiteral("offline"), QStringLiteral("o1"));

        const LibraryCheckResult result = checkLibrary(*m_db);
        QCOMPARE(result.missing.size(), qsizetype(1));
        QCOMPARE(result.missing.first().publicationId, missing);
        QCOMPARE(result.missing.first().title, QStringLiteral("A Missing Paper"));
        QCOMPARE(result.missing.first().path, QStringLiteral("gone.pdf"));
        QVERIFY(!result.missing.first().otherFiles);
        QCOMPARE(result.unreadable.size(), qsizetype(1));
        QCOMPARE(result.unreadable.first().publicationId, unreadable);
        QCOMPARE(result.unreadable.first().problem, QStringLiteral("the file is damaged"));
        // A folder that isn't there is said once, not as a problem with each file.
        QCOMPARE(result.offlineFolders, QStringList{QStringLiteral("Papers")});
        QCOMPARE(result.problems(), qsizetype(2));
    }

    void aMissingCopyOfAPublicationWithAnotherCanBeForgotten()
    {
        const qint64 id = addWithFile(QStringLiteral("A Paper In Two Places"), QStringLiteral("old/copy.pdf"), QStringLiteral("missing"), QStringLiteral("c1"));
        const qint64 kept = addFile(QStringLiteral("new/copy.pdf"), QStringLiteral("ok"), QStringLiteral("c1"));
        FileRepository(*m_db).link(id, kept, false);
        LibraryCheckResult result = checkLibrary(*m_db);
        QCOMPARE(result.missing.size(), qsizetype(1));
        QVERIFY(result.missing.first().otherFiles);

        forgetFileLocation(*m_db, result.missing.first().fileId);
        result = checkLibrary(*m_db);
        QVERIFY(result.missing.isEmpty());
        QCOMPARE(FileRepository(*m_db).filesFor(id).size(), qsizetype(1));
        // The one left is where its cover comes from.
        QCOMPARE(scalar("SELECT is_primary FROM publication_file"), qint64(1));

        // A publication's only file is never forgotten.
        const qint64 alone = addWithFile(QStringLiteral("Only Here"), QStringLiteral("only.pdf"), QStringLiteral("missing"), QStringLiteral("o9"));
        forgetFileLocation(*m_db, checkLibrary(*m_db).missing.first().fileId);
        QCOMPARE(FileRepository(*m_db).filesFor(alone).size(), qsizetype(1));
    }

    void anUnreadableFileIsReadAgainFromTheStart()
    {
        addWithFile(QStringLiteral("A Broken Paper"), QStringLiteral("broken.pdf"), QStringLiteral("unreadable"), QStringLiteral("b1"));
        const qint64 file = scalar("SELECT id FROM file");
        m_db->exec("INSERT INTO job (kind, payload, status, created_at, updated_at) VALUES ('inspect', '{\"file\":1}', 'failed', 1, 1)");
        readFileAgain(*m_db, file);
        QCOMPARE(text("SELECT status FROM file"), QStringLiteral("ok"));
        QVERIFY(text("SELECT sha256 FROM file").isNull()); // So the next scan reads it from the start.
        QCOMPARE(scalar("SELECT count(*) FROM job"), qint64(0));
    }

    void publicationsWaitingForFilesAreFound()
    {
        PublicationRepository publications(*m_db);
        Publication p;
        p.title = QStringLiteral("From Another Computer");
        const qint64 id = publications.create(p);
        FileRepository(*m_db).expectFile(id, {QStringLiteral("abc"), 100, QStringLiteral("pdf"), QStringLiteral("elsewhere.pdf"), true});
        FileRepository(*m_db).hideWaitingPublications();
        const qint64 suggestion = addFile(QStringLiteral("maybe.pdf"), QStringLiteral("ok"), QStringLiteral("other"));
        auto propose = m_db->prepare("INSERT INTO file_proposal (publication_id, file_id, reason, created_at) VALUES (?, ?, 'name_and_size', 1)");
        propose->bind(1, id);
        propose->bind(2, suggestion);
        propose->step();
        propose.reset();
        keepFileOut(*m_db, suggestion, 1); // Not also a file in no publication, for this test.

        const LibraryCheckResult result = checkLibrary(*m_db);
        QCOMPARE(result.waiting.size(), qsizetype(1));
        QCOMPARE(result.waiting.first().publicationId, id);
        QCOMPARE(result.waiting.first().fileName, QStringLiteral("elsewhere.pdf"));
        QVERIFY(result.waiting.first().suggested);
    }

    void filesInNoPublicationAreFoundUnlessKeptOutOrStillBeingRead()
    {
        const qint64 loose = addFile(QStringLiteral("loose.pdf"), QStringLiteral("ok"), QStringLiteral("l1"));
        const qint64 reading = addFile(QStringLiteral("reading.pdf"), QStringLiteral("ok"), QStringLiteral("r1"));
        m_db->exec(QStringLiteral("INSERT INTO job (kind, payload, created_at, updated_at) VALUES ('inspect', '{\"file\":%1}', 1, 1)")
                       .arg(reading).toUtf8().constData());
        const qint64 removed = addFile(QStringLiteral("removed.pdf"), QStringLiteral("ok"), QStringLiteral("x1"));
        m_db->exec("INSERT INTO removed_content (sha256, title, file_name, removed_at) VALUES ('x1', 'Gone', 'removed.pdf', 1)");
        addFile(QStringLiteral("unhashed.pdf"), QStringLiteral("ok"), QString());

        LibraryCheckResult result = checkLibrary(*m_db);
        QCOMPARE(result.unattached.size(), qsizetype(1));
        QCOMPARE(result.unattached.first().fileId, loose);
        Q_UNUSED(removed);

        // Kept out, as removing would, so the same contents elsewhere stay out too.
        keepFileOut(*m_db, loose, 5);
        QVERIFY(RemovedContent(*m_db).covers(loose, QStringLiteral("l1"), 10));
        QVERIFY(checkLibrary(*m_db).unattached.isEmpty());
    }

    void sharedIdentifiersAreFoundUntilSaidToDiffer()
    {
        PublicationRepository publications(*m_db);
        Publication a;
        a.title = QStringLiteral("A Made-Up Study");
        a.doi = QStringLiteral("10.1000/shared");
        a.isbn13 = QStringLiteral("9780306406157");
        const qint64 first = publications.create(a);
        Publication b = a;
        b.id = 0;
        b.uuid.clear();
        b.title = QStringLiteral("A Made-Up Study, Again");
        const qint64 second = publications.create(b);
        // The second has a file, so it's the one kept.
        const qint64 file = addFile(QStringLiteral("study.pdf"), QStringLiteral("ok"), QStringLiteral("s1"));
        FileRepository(*m_db).link(second, file, true);

        LibraryCheckResult result = checkLibrary(*m_db);
        QCOMPARE(result.shared.size(), qsizetype(1)); // Sharing a DOI and an ISBN is said once.
        QCOMPARE(result.shared.first().kind, QStringLiteral("DOI"));
        QCOMPARE(result.shared.first().publicationIds, (QList<qint64>{second, first}));

        markDistinct(*m_db, result.shared.first().publicationIds);
        QVERIFY(areDistinct(*m_db, first, second));
        QVERIFY(checkLibrary(*m_db).shared.isEmpty());

        // A third with the same DOI brings the group back, since it hasn't been said to differ.
        Publication c = a;
        c.id = 0;
        c.uuid.clear();
        c.title = QStringLiteral("A Third Copy");
        const qint64 third = publications.create(c);
        result = checkLibrary(*m_db);
        QCOMPARE(result.shared.size(), qsizetype(1));
        QCOMPARE(result.shared.first().publicationIds.size(), qsizetype(3));

        // Combining keeps what was said about the one that goes.
        mergePublications(*m_db, third, first, QStringLiteral("manual"));
        QVERIFY(areDistinct(*m_db, third, second));
        QCOMPARE(scalar("SELECT count(*) FROM distinct_publications"), qint64(1));
    }

    void anIssueAndItsArticleMayShareAnIdentifier()
    {
        PublicationRepository publications(*m_db);
        Publication issue;
        issue.type = PublicationType::JournalIssue;
        issue.title = QStringLiteral("A Made-Up Journal, Issue 3");
        issue.doi = QStringLiteral("10.1000/issue");
        const qint64 parent = publications.create(issue);
        Publication article;
        article.type = PublicationType::JournalArticle;
        article.title = QStringLiteral("An Article Inside");
        article.doi = QStringLiteral("10.1000/issue");
        article.parentId = parent;
        publications.create(article);
        QVERIFY(checkLibrary(*m_db).shared.isEmpty());
    }

    void publicationsKeptForARemovedFolderAreLeftOut()
    {
        addWithFile(QStringLiteral("In A Removed Folder"), QStringLiteral("gone.pdf"), QStringLiteral("missing"), QStringLiteral("g1"));
        addFile(QStringLiteral("loose.pdf"), QStringLiteral("ok"), QStringLiteral("l2"));
        FileRepository(*m_db).hideRoot(m_root);
        QCOMPARE(checkLibrary(*m_db).problems(), qsizetype(0));
    }

private:
    qint64 addFile(const QString &path, const QString &status, const QString &sha256)
    {
        auto insert = m_db->prepare("INSERT INTO file (root_id, relative_path, file_name, size, modified_at, sha256, format, status) "
                                    "VALUES (?, ?, ?, 10, 1, ?, 'pdf', ?)");
        insert->bind(1, m_root);
        insert->bind(2, path);
        insert->bind(3, path.section(u'/', -1));
        insert->bind(4, sha256);
        insert->bind(5, status);
        insert->step();
        return m_db->lastInsertId();
    }

    qint64 addWithFile(const QString &title, const QString &path, const QString &status, const QString &sha256)
    {
        Publication p;
        p.title = title;
        const qint64 id = PublicationRepository(*m_db).create(p);
        FileRepository(*m_db).link(id, addFile(path, status, sha256), true);
        return id;
    }

    qint64 scalar(const char *sql)
    {
        auto select = m_db->prepare(sql);
        select->step();
        return select->int64At(0);
    }

    QString text(const char *sql)
    {
        auto select = m_db->prepare(sql);
        select->step();
        return select->textAt(0);
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    qint64 m_root = 0;
};

QTEST_GUILESS_MAIN(LibraryCheckTest)
#include "test_library_check.moc"
