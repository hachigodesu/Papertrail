#include "core/db/Database.h"
#include "core/db/Schema.h"

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

private:
    QString path() const { return m_dir->filePath(QStringLiteral("test.db")); }

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
