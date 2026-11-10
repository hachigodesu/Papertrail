#include "core/db/Backups.h"
#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/repo/PublicationRepository.h"

#include <QFile>

#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail::core;

class DatabaseTest : public QObject {
    Q_OBJECT

private slots:
    void init() { m_dir = std::make_unique<QTemporaryDir>(); }

    void opensWithExpectedSettings()
    {
        Database db(path());
        auto foreignKeys = db.prepare("PRAGMA foreign_keys");
        foreignKeys->step();
        QCOMPARE(foreignKeys->intAt(0), 1);

        auto journal = db.prepare("PRAGMA journal_mode");
        journal->step();
        QCOMPARE(journal->textAt(0), QStringLiteral("wal"));
    }

    void opensNonAsciiPath()
    {
        const QString file = m_dir->filePath(QStringLiteral("Bibliothèque 文献.db"));
        {
            Database db(file);
            db.exec("CREATE TABLE t (x INTEGER)");
        }
        QVERIFY(QFile::exists(file));
    }

    void fts5IsAvailable()
    {
        Database db(path());
        db.exec("CREATE VIRTUAL TABLE t USING fts5(x, tokenize = \"unicode61 remove_diacritics 2\")");
        db.exec("INSERT INTO t VALUES ('Müller')");
        auto select = db.prepare("SELECT count(*) FROM t WHERE t MATCH 'muller'");
        select->step();
        QCOMPARE(select->intAt(0), 1); // Accents are ignored when searching.
    }

    void textRoundTripsIncludingNullAndUnicode()
    {
        Database db(path());
        db.exec("CREATE TABLE t (x TEXT)");
        auto insert = db.prepare("INSERT INTO t VALUES (?)");
        insert->bind(1, QStringLiteral("Café 文献 🧪"));
        insert->step();
        insert->reset();
        insert->bind(1, QString());
        insert->step();

        auto select = db.prepare("SELECT x FROM t ORDER BY rowid");
        QVERIFY(select->step());
        QCOMPARE(select->textAt(0), QStringLiteral("Café 文献 🧪"));
        QVERIFY(select->step());
        QVERIFY(select->isNull(0));
        QVERIFY(select->textAt(0).isNull());
    }

    void badSqlThrows()
    {
        Database db(path());
        QVERIFY_THROWS_EXCEPTION(DbError, db.exec("NOT SQL"));
        QVERIFY_THROWS_EXCEPTION(DbError, db.prepare("SELECT * FROM missing_table"));
    }

    void transactionRollsBackUnlessCommitted()
    {
        Database db(path());
        db.exec("CREATE TABLE t (x INTEGER)");
        {
            Transaction transaction(db);
            db.exec("INSERT INTO t VALUES (1)");
        }
        QCOMPARE(rowCount(db), 0);
        QVERIFY(!db.inTransaction());

        {
            Transaction transaction(db);
            db.exec("INSERT INTO t VALUES (1)");
            transaction.commit();
        }
        QCOMPARE(rowCount(db), 1);
    }

    void nestedTransactionRollsBackOnlyItself()
    {
        Database db(path());
        db.exec("CREATE TABLE t (x INTEGER)");
        {
            Transaction outer(db);
            db.exec("INSERT INTO t VALUES (1)");
            {
                Transaction inner(db);
                db.exec("INSERT INTO t VALUES (2)");
            }
            {
                Transaction inner(db);
                db.exec("INSERT INTO t VALUES (3)");
                inner.commit();
            }
            outer.commit();
        }
        QCOMPARE(rowCount(db), 2);
    }

    void migrateCreatesSchemaAndIsRepeatable()
    {
        Database db(path());
        migrateLibrary(db);
        const int version = db.userVersion();
        QVERIFY(version >= 1);
        QCOMPARE(version, libraryMigrations().back().version);

        migrateLibrary(db); // Running again must change nothing and not fail.
        QCOMPARE(db.userVersion(), version);
    }

    void migrationsApplyInOrderFromAnOlderVersion()
    {
        Database db(path());
        const std::vector<Migration> first = {{1, "CREATE TABLE a (x INTEGER)"}};
        migrate(db, first);
        QCOMPARE(db.userVersion(), 1);

        const std::vector<Migration> second = {
            {1, "CREATE TABLE a (x INTEGER)"},
            {2, "ALTER TABLE a ADD COLUMN y INTEGER"},
        };
        migrate(db, second);
        QCOMPARE(db.userVersion(), 2);
        db.exec("INSERT INTO a (x, y) VALUES (1, 2)");
    }

    void failedMigrationLeavesDatabaseUnchanged()
    {
        Database db(path());
        const std::vector<Migration> migrations = {
            {1, "CREATE TABLE good (x INTEGER)"},
            {2, "CREATE TABLE partial (x INTEGER); NOT VALID SQL"},
        };
        QVERIFY_THROWS_EXCEPTION(DbError, migrate(db, migrations));

        QCOMPARE(db.userVersion(), 1); // The first migration stays applied.
        db.exec("INSERT INTO good VALUES (1)");
        QVERIFY_THROWS_EXCEPTION(DbError, db.prepare("SELECT * FROM partial")); // The second is gone.
    }

    void refusesDatabaseFromNewerVersion()
    {
        Database db(path());
        db.setUserVersion(9999);
        QVERIFY_THROWS_EXCEPTION(DbError, migrateLibrary(db));
        QCOMPARE(db.userVersion(), 9999);
    }

    void foreignKeysAreEnforced()
    {
        Database db(path());
        migrateLibrary(db);
        QVERIFY_THROWS_EXCEPTION(DbError, db.exec("INSERT INTO publication_tag VALUES (999, 999)"));
    }

    void backupsAreMadeListedAndNamedForWhy()
    {
        Database db(path());
        migrateLibrary(db);
        addPublication(db, QStringLiteral("Kept"));
        const QDateTime morning(QDate(2026, 3, 4), QTime(9, 30, 0));
        QVERIFY(dailyBackupDue(path(), morning.date()));
        const QString daily = makeBackup(db, path(), BackupKind::Daily, morning);
        QCOMPARE(QFileInfo(daily).absolutePath(), backupFolder(path()));
        QCOMPARE(QFileInfo(daily).fileName(), QStringLiteral("library-daily-2026-03-04-093000.db"));
        QVERIFY(!dailyBackupDue(path(), morning.date()));
        QVERIFY(dailyBackupDue(path(), morning.date().addDays(1)));

        // Two in the same second don't overwrite each other.
        const QString first = makeBackup(db, path(), BackupKind::ByHand, morning.addSecs(60));
        const QString second = makeBackup(db, path(), BackupKind::ByHand, morning.addSecs(60));
        QVERIFY(first != second);

        // Anything else in the folder is left out.
        QFile other(QDir(backupFolder(path())).filePath(QStringLiteral("notes.db")));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();

        const QList<BackupInfo> found = backups(path());
        QCOMPARE(found.size(), 3);
        QCOMPARE(found.last().path, daily);
        QCOMPARE(found.last().kind, BackupKind::Daily);
        QCOMPARE(found.last().made, morning);
        QVERIFY(found.last().size > 0);
        QCOMPARE(found.first().kind, BackupKind::ByHand);

        // Each is a whole library on its own.
        Database copy(daily);
        QCOMPARE(publicationCount(copy), 1);
        QVERIFY(checkBackup(daily).usable);
    }

    void onlyTheNewestBackupsAreKept()
    {
        Database db(path());
        migrateLibrary(db);
        const QDateTime start(QDate(2026, 1, 1), QTime(12, 0, 0));
        for (int day = 0; day < 10; ++day)
            makeBackup(db, path(), BackupKind::Daily, start.addDays(day));
        for (int n = 0; n < 5; ++n) {
            makeBackup(db, path(), BackupKind::BeforeImport, start.addSecs(n));
            makeBackup(db, path(), BackupKind::ByHand, start.addSecs(n));
        }
        pruneBackups(path());
        QHash<int, QList<QDateTime>> kept;
        for (const BackupInfo &backup : backups(path()))
            kept[int(backup.kind)] << backup.made;
        QCOMPARE(kept.value(int(BackupKind::Daily)).size(), kDailyBackupsKept);
        QCOMPARE(kept.value(int(BackupKind::Daily)).last(), start.addDays(3)); // The oldest three went.
        QCOMPARE(kept.value(int(BackupKind::BeforeImport)).size(), kOtherBackupsKept);
        QCOMPARE(kept.value(int(BackupKind::ByHand)).size(), 5); // Never removed automatically.
    }

    void restoringReplacesTheLibraryAndCanBeUndone()
    {
        Database db(path());
        migrateLibrary(db);
        addPublication(db, QStringLiteral("Before"));
        const QString backup = makeBackup(db, path(), BackupKind::ByHand);
        addPublication(db, QStringLiteral("After"));
        QCOMPARE(publicationCount(db), 2);

        // Another connection, as the window's threads have, is reading the library too.
        Database other(path());
        QCOMPARE(publicationCount(other), 2);
        const QByteArray stamp = db.changeStamp();

        const QString before = restoreBackup(db, path(), backup);
        QCOMPARE(publicationCount(db), 1);
        QCOMPARE(publicationCount(other), 1);
        QVERIFY(db.changeStamp() != stamp); // So anything cached from before is looked at again.
        auto mode = db.prepare("PRAGMA journal_mode");
        QVERIFY(mode->step());
        QCOMPARE(mode->textAt(0), QStringLiteral("wal"));
        mode.reset();
        addPublication(db, QStringLiteral("Later")); // Still usable as ever.
        QCOMPARE(publicationCount(other), 2);

        // The library as it was is kept, and restoring it undoes the restore.
        QCOMPARE(backups(path()).first().kind, BackupKind::BeforeRestore);
        {
            Database kept(before);
            QCOMPARE(publicationCount(kept), 2);
        }
        restoreBackup(db, path(), before);
        QCOMPARE(publicationCount(db), 2);
    }

    void anOlderBackupIsBroughtUpToDate()
    {
        const QString older = m_dir->filePath(QStringLiteral("older.db"));
        {
            Database db(older);
            const std::vector<Migration> &all = libraryMigrations();
            migrate(db, std::vector<Migration>(all.begin(), all.end() - 1));
            QCOMPARE(db.userVersion(), all.back().version - 1);
        }
        QVERIFY(checkBackup(older).usable);
        Database db(path());
        migrateLibrary(db);
        restoreBackup(db, path(), older);
        QCOMPARE(db.userVersion(), libraryMigrations().back().version);
    }

    void unusableBackupsAreRefusedWithoutChangingTheLibrary()
    {
        Database db(path());
        migrateLibrary(db);
        addPublication(db, QStringLiteral("Mine"));

        const QString text = m_dir->filePath(QStringLiteral("text.db"));
        QFile file(text);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a database at all, only some text that is long enough to look like something");
        file.close();
        const QString notLibrary = m_dir->filePath(QStringLiteral("other.db"));
        {
            Database other(notLibrary);
            other.exec("CREATE TABLE t (x INTEGER)");
            other.setUserVersion(3);
        }
        const QString newer = m_dir->filePath(QStringLiteral("newer.db"));
        {
            Database other(newer);
            migrateLibrary(other);
            other.setUserVersion(9999);
        }
        for (const QString &bad : {text, notLibrary, newer, m_dir->filePath(QStringLiteral("missing.db"))}) {
            const BackupCheck check = checkBackup(bad);
            QVERIFY2(!check.usable, qPrintable(bad));
            QVERIFY(!check.problem.isEmpty());
            QVERIFY_THROWS_EXCEPTION(DbError, restoreBackup(db, path(), bad));
        }
        QCOMPARE(publicationCount(db), 1);
        QVERIFY(backups(path()).isEmpty()); // Nothing was backed up for a restore that didn't happen.
    }

    void caseLawIsAddedKeepingEverythingThatWasThere()
    {
        Database db(path());
        const std::vector<Migration> &all = libraryMigrations();
        // Up to 19, the last before case law.
        QCOMPARE(all.at(18).version, 19);
        migrate(db, std::vector<Migration>(all.begin(), all.begin() + 19));

        // A library as 1.0.2 left it: a publication in an issue, with a file, a tag, a note, a highlight and a summary.
        db.exec("INSERT INTO publication (id, uuid, type, title, notes, title_sort, created_at, updated_at) VALUES "
                "(1, 'u1', 'journal_volume', 'A Made-Up Issue', 'A made-up note', 'a made-up issue', 1, 1), "
                "(2, 'u2', 'journal_article', 'Bending Made-Up Beams', NULL, 'bending made-up beams', 1, 1)");
        db.exec("UPDATE publication SET parent_id = 1, page_start = 2, page_end = 5, doi = '10.1000/beams' WHERE id = 2");
        db.exec("INSERT INTO library_root (id, uuid, name, path, created_at) VALUES (1, 'r1', 'Docs', '/made/up', 1)");
        db.exec("INSERT INTO file (id, root_id, relative_path, file_name, size, modified_at, format, sha256) "
                "VALUES (1, 1, 'issue.pdf', 'issue.pdf', 10, 1, 'pdf', 'abc')");
        db.exec("INSERT INTO publication_file (publication_id, file_id, is_primary) VALUES (1, 1, 1)");
        db.exec("INSERT INTO tag (id, uuid, name, created_at) VALUES (1, 't1', 'Beams', 1)");
        db.exec("INSERT INTO publication_tag (publication_id, tag_id) VALUES (2, 1)");
        db.exec("INSERT INTO annotation (uuid, publication_id, content_hash, location_json, type, selected_text, created_at, "
                "updated_at) VALUES ('a1', 2, 'abc', '{\"parts\":[]}', 'highlight', 'made-up words', 1, 1)");
        const auto count = [&](const char *sql) {
            auto select = db.prepare(sql);
            select->step();
            return select->int64At(0);
        };
        const auto everything = [&] {
            return QList<qint64>{count("SELECT count(*) FROM publication"), count("SELECT count(*) FROM publication_file"),
                                 count("SELECT count(*) FROM publication_tag"), count("SELECT count(*) FROM annotation"),
                                 count("SELECT count(*) FROM publication_fts WHERE publication_fts MATCH 'beams'"),
                                 count("SELECT count(*) FROM publication_note_fts WHERE publication_note_fts MATCH 'note'")};
        };
        const QList<qint64> before = everything();
        const qint64 indexes = count("SELECT count(*) FROM sqlite_schema WHERE tbl_name = 'publication' AND type = 'index'");
        const qint64 triggers = count("SELECT count(*) FROM sqlite_schema WHERE tbl_name = 'publication' AND type = 'trigger'");

        migrateLibrary(db);
        QCOMPARE(db.userVersion(), all.back().version);
        QCOMPARE(everything(), before);
        // Later migrations add indexes of their own: for sorting by rating, and for PubMed ids.
        QCOMPARE(count("SELECT count(*) FROM sqlite_schema WHERE tbl_name = 'publication' AND type = 'index' "
                       "AND name NOT LIKE '%rating%' AND name <> 'publication_pmid'"),
                 indexes);
        QCOMPARE(count("SELECT count(*) FROM sqlite_schema WHERE tbl_name = 'publication' AND type = 'trigger'"), triggers);
        QCOMPARE(count("SELECT count(*) FROM sqlite_schema WHERE name = 'publication_new'"), qint64(0));
        // Foreign keys are back on, and everything still points where it did.
        QCOMPARE(count("PRAGMA foreign_keys"), qint64(1));
        auto dangling = db.prepare("PRAGMA foreign_key_check");
        QVERIFY(!dangling->step());
        const std::optional<Publication> article = PublicationRepository(db).get(2);
        QVERIFY(article);
        QCOMPARE(article->parentId.value_or(0), qint64(1));
        QCOMPARE(article->pageEnd.value_or(0), 5);
        QCOMPARE(article->doi, QStringLiteral("10.1000/beams"));
        // Another connection reads it as it is.
        {
            Database other(path());
            QCOMPARE(PublicationRepository(other).get(2)->title, QStringLiteral("Bending Made-Up Beams"));
        }

        // A case can be kept now, and searching finds it by its court and citations.
        Publication judgment;
        judgment.type = PublicationType::CaseLaw;
        judgment.title = QStringLiteral("Made-Up Party v Another Made-Up Party");
        judgment.court = QStringLiteral("Supreme Court of the United Kingdom");
        judgment.neutralCitation = QStringLiteral("[2020] UKSC 99");
        judgment.caseNumber = QStringLiteral("UKSC 2019/0001");
        judgment.caseCitationForm = QStringLiteral("uk");
        const qint64 id = PublicationRepository(db).create(judgment);
        const std::optional<Publication> kept = PublicationRepository(db).get(id);
        QCOMPARE(kept->type, PublicationType::CaseLaw);
        QCOMPARE(kept->neutralCitation, QStringLiteral("[2020] UKSC 99"));
        QCOMPARE(kept->caseCitationForm, QStringLiteral("uk"));
        QCOMPARE(count("SELECT count(*) FROM publication_fts WHERE publication_fts MATCH 'UKSC'"), qint64(1));
        QCOMPARE(count("SELECT count(*) FROM publication_fts WHERE publication_fts MATCH 'publisher:kingdom'"), qint64(1));

        // Running it again changes nothing, and keeps what's in the new columns.
        db.setUserVersion(19);
        migrateLibrary(db);
        QCOMPARE(PublicationRepository(db).get(id)->court, QStringLiteral("Supreme Court of the United Kingdom"));
        QCOMPARE(count("SELECT count(*) FROM publication"), before.first() + 1);

        // Deleting the issue still takes its article, and the article's highlight, with it.
        db.exec("DELETE FROM publication WHERE id = 1");
        QCOMPARE(count("SELECT count(*) FROM publication WHERE id = 2"), qint64(0));
        QCOMPARE(count("SELECT count(*) FROM annotation"), qint64(0));
    }

    void bookmarksRelatedPapersAndRatingSortsAreAddedAndCanBeAddedAgain()
    {
        Database db(path());
        const std::vector<Migration> &all = libraryMigrations();
        QCOMPARE(all.back().version, 22);
        migrate(db, std::vector<Migration>(all.begin(), all.end() - 2));
        db.exec("INSERT INTO publication (id, uuid, type, title, rating, title_sort, created_at, updated_at) VALUES "
                "(1, 'u1', 'book', 'A Made-Up Book', 4, 'a made-up book', 1, 1)");
        migrateLibrary(db);
        QCOMPARE(db.userVersion(), 22);
        const auto count = [&](const char *sql) {
            auto select = db.prepare(sql);
            select->step();
            return select->int64At(0);
        };
        db.exec("INSERT INTO bookmark (uuid, publication_id, content_hash, page, y, label, created_at, updated_at) "
                "VALUES ('b1', 1, 'abc', 3, 0.5, 'Made-up place', 1, 1)");
        db.exec("INSERT INTO related_fetch (publication_id, doi, fetched_at) VALUES (1, '10.1000/made-up', 1)");
        db.exec("INSERT INTO related_work (publication_id, relation, position, openalex_id, record) "
                "VALUES (1, 'reference', 0, 'W1', '{}')");
        for (const char *index : {"publication_rating_desc", "publication_rating_asc", "publication_type_rating_desc",
                                  "publication_type_rating_asc", "publication_rating", "reading_state_last_opened"}) {
            auto found = db.prepare("SELECT count(*) FROM sqlite_schema WHERE type = 'index' AND name = ?");
            found->bind(1, QString::fromLatin1(index));
            found->step();
            QVERIFY2(found->int64At(0) == 1, index);
        }

        // Running it again keeps what's there.
        db.setUserVersion(20);
        migrateLibrary(db);
        QCOMPARE(count("SELECT count(*) FROM bookmark"), qint64(1));
        QCOMPARE(count("SELECT count(*) FROM related_work"), qint64(1));

        // They go with their publication.
        db.exec("DELETE FROM publication WHERE id = 1");
        QCOMPARE(count("SELECT count(*) FROM bookmark"), qint64(0));
        QCOMPARE(count("SELECT count(*) FROM related_fetch"), qint64(0));
        QCOMPARE(count("SELECT count(*) FROM related_work"), qint64(0));
    }

    void publicationsSaidToDifferAreKeptAndCanBeAddedAgain()
    {
        Database db(path());
        const std::vector<Migration> &all = libraryMigrations();
        migrate(db, std::vector<Migration>(all.begin(), all.end() - 1));
        for (int id = 1; id <= 3; ++id) {
            db.exec(QStringLiteral("INSERT INTO publication (id, uuid, type, title, title_sort, created_at, updated_at) VALUES "
                                   "(%1, 'u%1', 'book', 'A Made-Up Book', 'a made-up book', 1, 1)").arg(id).toUtf8().constData());
        }
        migrateLibrary(db);
        QCOMPARE(db.userVersion(), 22);
        const auto count = [&](const char *sql) {
            auto select = db.prepare(sql);
            select->step();
            return select->int64At(0);
        };
        db.exec("INSERT INTO distinct_publications (first_id, second_id, created_at) VALUES (1, 2, 5), (2, 3, 5)");
        QVERIFY_THROWS_EXCEPTION(DbError, db.exec("INSERT INTO distinct_publications (first_id, second_id, created_at) VALUES (3, 1, 5)"));

        // Running it again keeps what's there.
        db.setUserVersion(21);
        migrateLibrary(db);
        QCOMPARE(count("SELECT count(*) FROM distinct_publications"), qint64(2));
        QCOMPARE(count("SELECT count(*) FROM sqlite_schema WHERE name = 'publication_pmid'"), qint64(1));

        // A pair goes with either of them.
        db.exec("DELETE FROM publication WHERE id = 3");
        QCOMPARE(count("SELECT count(*) FROM distinct_publications"), qint64(1));
    }

    void anUpgradeIsBackedUpFirst()
    {
        Database db(path());
        QVERIFY(!backUpBeforeUpgrade(db, path())); // A new library has nothing to keep.
        const std::vector<Migration> &all = libraryMigrations();
        migrate(db, std::vector<Migration>(all.begin(), all.end() - 1));
        const std::optional<QString> made = backUpBeforeUpgrade(db, path());
        QVERIFY(made);
        QCOMPARE(backups(path()).first().kind, BackupKind::BeforeUpgrade);
        {
            Database copy(*made);
            QCOMPARE(copy.userVersion(), all.back().version - 1);
        }
        migrateLibrary(db);
        QVERIFY(!backUpBeforeUpgrade(db, path())); // Up to date.
    }

private:
    QString path() const { return m_dir->filePath(QStringLiteral("test.db")); }

    static void addPublication(Database &db, const QString &title)
    {
        Publication p;
        p.title = title;
        PublicationRepository(db).create(p);
    }

    static int publicationCount(Database &db)
    {
        auto select = db.prepare("SELECT count(*) FROM publication");
        select->step();
        return select->intAt(0);
    }

    static int rowCount(Database &db)
    {
        auto select = db.prepare("SELECT count(*) FROM t");
        select->step();
        return select->intAt(0);
    }

    std::unique_ptr<QTemporaryDir> m_dir;
};

QTEST_GUILESS_MAIN(DatabaseTest)
#include "test_database.moc"
