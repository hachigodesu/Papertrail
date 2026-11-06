// End-to-end import tests, using the real worker process and real sample documents.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "import/ImportService.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <thread>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::import;

class ImportTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QDir(m_dir->path()).mkpath(QStringLiteral("docs"));
        QDir(m_dir->path()).mkpath(QStringLiteral("cache"));
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        m_files = std::make_unique<FileRepository>(*m_db);
        m_publications = std::make_unique<PublicationRepository>(*m_db);
        m_query = std::make_unique<LibraryQuery>(*m_db);
        m_service = makeService();
        m_root = m_files->addRoot(QStringLiteral("Docs"), docs());
    }

    void cleanup()
    {
        m_service.reset();
        m_query.reset();
        m_publications.reset();
        m_files.reset();
        m_db.reset();
    }

    void metadataComesFromInfoDictionary()
    {
        copySample("article.pdf");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("Fatigue of Steel Beams"));
        QCOMPARE(p.authors.size(), 2);
        QCOMPARE(p.authors.at(0), (PersonName{QStringLiteral("Smith"), QStringLiteral("Jane")}));
        QCOMPARE(p.authors.at(1).family, QStringLiteral("Haddad"));
        QCOMPARE(p.doi, QStringLiteral("10.1234/example.5678"));
        QCOMPARE(p.year, 2020);
        QCOMPARE(p.type, PublicationType::JournalArticle);
        QVERIFY(!p.needsReview);
        QVERIFY(p.extraJson.contains(QStringLiteral("fatigue")));
    }

    void metadataComesFromXmp()
    {
        copySample("xmp_article.pdf");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("Turbulent Flow in Example Channels"));
        QCOMPARE(p.authors.at(0).family, QStringLiteral("Tanaka"));
        QCOMPARE(p.doi, QStringLiteral("10.5555/turbulent.2019"));
        QCOMPARE(p.year, 2019);
    }

    void junkMetadataFallsBackToTheFirstPage()
    {
        copySample("junk_metadata.pdf");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("Neural Bridges in Practice"));
        QVERIFY(p.authors.isEmpty()); // "Administrator" isn't an author.
        QVERIFY(p.needsReview); // A guess from the page, so a person should check it.
        const auto history = m_publications->provenance(p.id, QStringLiteral("title"));
        QCOMPARE(history.first().source, QStringLiteral("first_page"));
    }

    // These four come from importing a real collection of journal articles.

    void referencesDontChangeWhatADocumentIs()
    {
        copySample("article_citing_things.pdf");
        const Publication p = importOne();
        QCOMPARE(p.type, PublicationType::JournalArticle); // Not a thesis, report or paper.
        QCOMPARE(p.doi, QStringLiteral("10.1234/own.doi")); // Its own, not one it cites.
        QVERIFY(p.isbn13.isNull()); // The ISBN belongs to a cited book.
        QVERIFY(p.arxivId.isNull());
    }

    void publisherCodesArentTitlesAndMastheadsArentEither()
    {
        copySample("publisher_code_title.pdf");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("Mechanisms of Example Disease in Adults")); // Not "Medicine".
        QVERIFY(p.needsReview);
    }

    void authorsInOneMetadataEntryAreSeparated()
    {
        copySample("publisher_code_title.pdf");
        const Publication p = importOne();
        QCOMPARE(p.authors.size(), 3);
        QCOMPARE(p.authors.at(0), (PersonName{QStringLiteral("Sample"), QStringLiteral("Nadia")}));
        QCOMPARE(p.authors.at(2).family, QStringLiteral("Testing"));
    }

    void htmlCodesInMetadataAreDecoded()
    {
        copySample("entity_title.pdf");
        QCOMPARE(importOne().title, QStringLiteral("Samples’ Challenges & Perspectives?"));
    }

    void titlesThatWrapAreKeptWhole()
    {
        copySample("wrapped_title.pdf");
        QCOMPARE(importOne().title, QStringLiteral("A Regional Sample Market for Example Goods and Services in Example Districts"));
    }

    void noMetadataFallsBackThroughToTheFileName()
    {
        copySample("garbage.pdf", "my_lost-paper.pdf");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("my lost paper"));
        QVERIFY(p.needsReview);
        QCOMPARE(m_publications->provenance(p.id, QStringLiteral("title")).first().source, QStringLiteral("file_name"));
    }

    void isbnIsFoundInsideABook()
    {
        copySample("book.pdf");
        const Publication p = importOne();
        QCOMPARE(p.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(p.type, PublicationType::Textbook);
        QCOMPARE(p.year, 2015);
        QCOMPARE(m_files->filesFor(p.id).first().pageCount, 120);
    }

    void eachKindOfDocumentIsClassified_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("type");
        QTest::newRow("thesis") << "thesis.pdf" << int(PublicationType::Thesis);
        QTest::newRow("dissertation") << "dissertation.pdf" << int(PublicationType::Dissertation);
        QTest::newRow("report") << "report.pdf" << int(PublicationType::TechnicalReport);
        QTest::newRow("standard") << "standard.pdf" << int(PublicationType::Standard);
        QTest::newRow("conference") << "conference.pdf" << int(PublicationType::ConferencePaper);
        QTest::newRow("preprint wording isn't either") << "preprint_stamp.pdf" << int(PublicationType::JournalArticle);
    }

    void eachKindOfDocumentIsClassified()
    {
        QFETCH(QString, file);
        QFETCH(int, type);
        copySample(file.toUtf8().constData());
        const Publication p = importOne();
        QCOMPARE(int(p.type), type);
        QVERIFY(!p.needsReview);
    }

    void epubMetadataIsRead()
    {
        copySample("book.epub");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("Principles of Sample Engineering"));
        QCOMPARE(p.authors.size(), 2);
        QCOMPARE(p.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(p.publisher, QStringLiteral("Example Press"));
        QCOMPARE(p.year, 2018);
        QCOMPARE(p.language, QStringLiteral("en"));
        QCOMPARE(p.abstract, QStringLiteral("A made-up textbook."));
    }

    void everyFormatGetsACover_data()
    {
        QTest::addColumn<QString>("file");
        for (const char *name : {"article.pdf", "book.epub", "sample.fb2", "sample.cbz", "sample.xps",
                                 "old_format.mobi", "scanned.djvu"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void everyFormatGetsACover()
    {
        QFETCH(QString, file);
        copySample(file.toUtf8().constData());
        const Publication p = importOne();
        const FileRecord record = m_files->filesFor(p.id).first();
        QVERIFY(!record.sha256.isEmpty());
        QCOMPARE(record.status, FileStatus::Ok);
        const QFileInfo cover(m_service->coverPath(record.sha256));
        QVERIFY2(cover.exists(), qPrintable(cover.filePath()));
        QVERIFY(cover.size() > 100);
        QFile image(cover.filePath());
        QVERIFY(image.open(QIODevice::ReadOnly));
        QVERIFY(image.read(3).startsWith("\xff\xd8\xff")); // A JPEG.
    }

    void textLayerIsDetected()
    {
        copySample("article.pdf");
        QCOMPARE(m_files->filesFor(importOne().id).first().hasTextLayer, true);
    }

    void scansAreFlaggedAsHavingNoText()
    {
        copySample("scanned_no_text.pdf");
        const FileRecord file = m_files->filesFor(importOne().id).first();
        QCOMPARE(file.hasTextLayer, false);
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY(QFile::exists(m_service->coverPath(file.sha256)));

        PublicationFilter filter;
        filter.noText = true;
        QCOMPARE(m_query->count(filter), qint64(1));
    }

    void djvuTextLayersAreDetected()
    {
        copySample("with_text.djvu");
        QCOMPARE(m_files->filesFor(importOne().id).first().hasTextLayer, true);
    }

    void djvuWithoutTextIsFlagged()
    {
        copySample("scanned.djvu");
        QCOMPARE(m_files->filesFor(importOne().id).first().hasTextLayer, false);
    }

    void kf8IsRecognisedAndNeverGivenToMuPdf()
    {
        copySample("kf8_header.azw3");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("New Kindle Book"));
        QCOMPARE(p.authors.at(0).family, QStringLiteral("Sample"));
        QCOMPARE(p.publisher, QStringLiteral("Example Press"));
        QCOMPARE(p.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(p.year, 2021);

        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.format, QStringLiteral("azw3"));
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY(!file.pageCount.has_value()); // Nothing was rendered: M5 will convert it first.
        QVERIFY(!QFile::exists(m_service->coverPath(file.sha256)));
    }

    void passwordProtectedFilesAreListedAndFlagged()
    {
        copySample("encrypted.pdf");
        const Publication p = importOne();
        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.status, FileStatus::Unreadable);
        QCOMPARE(file.problem, QStringLiteral("needs a password"));
        QCOMPARE(p.title, QStringLiteral("encrypted"));
        QVERIFY(p.needsReview);
    }

    void blankFilesCountAsUnreadable()
    {
        copySample("blank.pdf");
        const FileRecord file = m_files->filesFor(importOne().id).first();
        QCOMPARE(file.status, FileStatus::Unreadable);
        QVERIFY(file.problem.contains(QStringLiteral("no visible content")));
    }

    void badFilesDontStopTheImport()
    {
        for (const char *name : {"garbage.pdf", "empty.pdf", "truncated.pdf", "not_really.epub", "encrypted.pdf",
                                 "blank.pdf", "article.pdf", "book.epub"})
            copySample(name);

        m_service->scan(m_root);
        const ImportReport report = m_service->processQueue();

        QCOMPARE(m_service->pendingJobs(), qint64(0));
        QCOMPARE(m_query->count({}), qint64(8)); // Every file is listed, readable or not.
        QVERIFY(report.unreadable >= 4); // At least garbage, empty, not_really and encrypted, plus blank.

        // Bad files may be rejected cleanly or may really crash the worker: on Arch, MuPDF hits a
        // double free repairing truncated.pdf. Either way, the results must be the same.

        PublicationFilter filter;
        filter.searchText = QStringLiteral("title:fatigue");
        QCOMPARE(m_query->count(filter), qint64(1)); // The good ones still imported properly.
    }

    void emptyFilesAreNotDuplicatesOfEachOther()
    {
        copySample("empty.pdf", "first empty.pdf");
        copySample("empty.pdf", "second empty.pdf");
        m_service->scan(m_root);
        const ImportReport report = m_service->processQueue();
        QCOMPARE(report.duplicates, 0);
        QCOMPARE(m_query->count({}), qint64(2));
    }

    void resumingDoesNotReportWaitingFilesAsChanged()
    {
        copySample("article.pdf");
        copySample("thesis.pdf");
        const ScanReport first = m_service->scan(m_root);
        QCOMPARE(first.newFiles, 2);

        // Scanning again before anything has been hashed, as when an import was stopped early.
        const ScanReport second = m_service->scan(m_root);
        QCOMPARE(second.newFiles, 0);
        QCOMPARE(second.changed, 0);
        QCOMPARE(second.unchanged, 2);
        QCOMPARE(m_service->pendingJobs(), qint64(2)); // Still queued, and only once each.

        m_service->processQueue();
        QCOMPARE(m_query->count({}), qint64(2));
    }

    void unsupportedFilesAreIgnored()
    {
        copySample("article.pdf");
        copySample("ignored_notes.txt");
        const ScanReport scan = m_service->scan(m_root);
        QCOMPARE(scan.newFiles, 1);
        QCOMPARE(scan.unsupportedSkipped, 1);
    }

    void workerCrashIsContainedAndRetried()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        copySample("article.pdf", "a__crash__.pdf");
        copySample("book.epub", "z_after.epub");

        m_service->scan(m_root);
        const ImportReport report = m_service->processQueue();
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");

        QCOMPARE(m_service->pendingJobs(), qint64(0));
        QVERIFY(report.workerRestarts >= 1);
        QCOMPARE(report.unreadable, 1);
        QCOMPARE(m_query->count({}), qint64(2));

        // The file that crashed the worker is listed under its name, and marked.
        PublicationFilter filter;
        filter.unreadable = true;
        QCOMPARE(m_query->page(filter, {}, 0, 1).first().title, QStringLiteral("a crash"));

        // The file after it imported normally.
        filter = {};
        filter.searchText = QStringLiteral("title:principles");
        QCOMPARE(m_query->count(filter), qint64(1));
    }

    void hungWorkerIsStoppedAndRetried()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        ImportOptions options = serviceOptions();
        options.minHashTimeoutMs = 1500;
        options.inspectTimeoutMs = 1500;
        m_service = std::make_unique<ImportService>(*m_db, options);

        copySample("article.pdf", "a__hang__.pdf");
        copySample("book.epub", "z_after.epub");

        m_service->scan(m_root);
        const ImportReport report = m_service->processQueue();
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");

        QCOMPARE(m_service->pendingJobs(), qint64(0));
        QCOMPARE(report.unreadable, 1);
        QCOMPARE(m_query->count({}), qint64(2));
    }

    void duplicatesBecomeOnePublicationWithTwoLocations()
    {
        copySample("article.pdf", "first.pdf");
        QDir(docs()).mkpath(QStringLiteral("sub"));
        copySample("article.pdf", "sub/second copy.pdf");

        m_service->scan(m_root);
        const ImportReport report = m_service->processQueue();

        QCOMPARE(report.publicationsCreated, 1);
        QCOMPARE(report.duplicates, 1);
        QCOMPARE(m_query->count({}), qint64(1));
        const qint64 id = m_query->ids({}, {}).first();
        const QList<FileRecord> files = m_files->filesFor(id);
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).sha256, files.at(1).sha256);
        QCOMPARE(files.at(1).pageCount, files.at(0).pageCount);
    }

    void movedFilesKeepTheirPublicationAndEdits()
    {
        copySample("article.pdf", "old-place.pdf");
        const qint64 id = importOne().id;

        Publication edited = *m_publications->get(id);
        edited.title = QStringLiteral("My Corrected Title");
        m_publications->update(edited);

        QDir(docs()).mkpath(QStringLiteral("elsewhere"));
        QVERIFY(QFile::rename(docPath("old-place.pdf"), docPath("elsewhere/renamed.pdf")));

        const ScanReport scan = m_service->scan(m_root);
        QCOMPARE(scan.newFiles, 1);
        QCOMPARE(scan.nowMissing, 1);
        const ImportReport report = m_service->processQueue();

        QCOMPARE(report.moved, 1);
        QCOMPARE(report.publicationsCreated, 0);
        QCOMPARE(m_query->count({}), qint64(1));
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("My Corrected Title"));

        const QList<FileRecord> files = m_files->filesFor(id);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().relativePath, QStringLiteral("elsewhere/renamed.pdf"));
        QCOMPARE(files.first().status, FileStatus::Ok);
    }

    void changedFilesKeepTheirPublicationAndEdits()
    {
        copySample("article.pdf", "paper.pdf");
        const qint64 id = importOne().id;
        Publication edited = *m_publications->get(id);
        edited.title = QStringLiteral("My Corrected Title");
        m_publications->update(edited);
        const QString oldHash = m_files->filesFor(id).first().sha256;

        QFile::remove(docPath("paper.pdf"));
        copySample("book.pdf", "paper.pdf");

        QCOMPARE(m_service->scan(m_root).changed, 1);
        const ImportReport report = m_service->processQueue();

        QCOMPARE(report.updated, 1);
        QCOMPARE(report.publicationsCreated, 0);
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("My Corrected Title"));
        const FileRecord file = m_files->filesFor(id).first();
        QVERIFY(file.sha256 != oldHash);
        QCOMPARE(file.pageCount, 120); // The file's own facts do update.
    }

    void rescanFindsNothingNewAndDoesNoWork()
    {
        copySample("article.pdf");
        copySample("book.epub");
        m_service->scan(m_root);
        m_service->processQueue();

        const ScanReport again = m_service->scan(m_root);
        QCOMPARE(again.newFiles, 0);
        QCOMPARE(again.changed, 0);
        QCOMPARE(again.unchanged, 2);
        QCOMPARE(m_service->pendingJobs(), qint64(0));
        QCOMPARE(m_query->count({}), qint64(2));
    }

    void deletedFilesBecomeMissingAndCanComeBack()
    {
        copySample("article.pdf", "paper.pdf");
        const qint64 id = importOne().id;
        const QByteArray contents = readAll(docPath("paper.pdf"));
        const QDateTime modified = QFileInfo(docPath("paper.pdf")).lastModified();

        QFile::remove(docPath("paper.pdf"));
        QCOMPARE(m_service->scan(m_root).nowMissing, 1);
        QCOMPARE(m_files->filesFor(id).first().status, FileStatus::Missing);
        QVERIFY(m_publications->get(id).has_value()); // The record and its tags stay.

        PublicationFilter filter;
        filter.missingOrOffline = true;
        QCOMPARE(m_query->count(filter), qint64(1));

        QFile restored(docPath("paper.pdf"));
        QVERIFY(restored.open(QIODevice::WriteOnly));
        restored.write(contents);
        restored.flush(); // Otherwise the write lands on close, and changes the time again.
        restored.setFileTime(modified, QFileDevice::FileModificationTime);
        restored.close();

        QCOMPARE(m_service->scan(m_root).backAgain, 1);
        QCOMPARE(m_files->filesFor(id).first().status, FileStatus::Ok);
    }

    void unpluggedDriveMarksFilesOfflineNotMissing()
    {
        QTemporaryDir external;
        const QString path = external.filePath(QStringLiteral("drive"));
        QDir().mkpath(path);
        QVERIFY(QFile::copy(sample("article.pdf"), path + QStringLiteral("/paper.pdf")));
        const qint64 root = m_files->addRoot(QStringLiteral("Drive"), path);
        m_service->scan(root);
        m_service->processQueue();
        const qint64 id = m_query->ids({}, {}).first();

        QDir(path).removeRecursively(); // The drive has gone.
        const ScanReport scan = m_service->scan(root);
        QVERIFY(!scan.rootAvailable);
        QCOMPARE(scan.nowMissing, 0);
        QCOMPARE(m_files->filesFor(id).first().status, FileStatus::Offline);

        QDir().mkpath(path); // It's back.
        QVERIFY(QFile::copy(sample("article.pdf"), path + QStringLiteral("/paper.pdf")));
        m_service->scan(root);
        m_service->processQueue();
        QCOMPARE(m_query->count({}), qint64(1));
    }

    void importCarriesOnAfterStopping()
    {
        const QStringList names = {"article.pdf", "thesis.pdf", "report.pdf", "preprint.pdf", "book.epub"};
        for (const QString &name : names)
            copySample(name.toUtf8().constData());
        m_service->scan(m_root);

        // Stop after a couple of files, as if the app had been closed.
        int calls = 0;
        const ImportReport first = m_service->processQueue({}, [&] { return ++calls > 3; });
        QVERIFY(first.stoppedEarly);
        QVERIFY(m_service->pendingJobs() > 0);
        const qint64 afterFirst = m_query->count({});
        QVERIFY(afterFirst < names.size());

        // A completely new service and connection, as after restarting the app.
        m_service.reset();
        m_query.reset();
        m_publications.reset();
        m_files.reset();
        m_db.reset();

        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        m_files = std::make_unique<FileRepository>(*m_db);
        m_publications = std::make_unique<PublicationRepository>(*m_db);
        m_query = std::make_unique<LibraryQuery>(*m_db);
        m_service = makeService();

        const ImportReport second = m_service->processQueue();
        QVERIFY(!second.stoppedEarly);
        QCOMPARE(m_service->pendingJobs(), qint64(0));
        QCOMPARE(m_query->count({}), qint64(names.size())); // All there, and none doubled up.
    }

    void interruptedJobsAreRecovered()
    {
        copySample("article.pdf");
        m_service->scan(m_root);

        // As if the app died while a job was running.
        m_db->exec("UPDATE job SET status = 'running'");
        m_service = makeService();
        QCOMPARE(m_service->pendingJobs(), qint64(1));
        m_service->processQueue();
        QCOMPARE(m_query->count({}), qint64(1));
    }

    void rescanDoesNotDuplicateAFileThatKeepsFailing()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        copySample("article.pdf", "a__crash__.pdf");
        m_service->scan(m_root);
        m_service->processQueue();
        m_service->scan(m_root);
        m_service->processQueue();
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");
        QCOMPARE(m_query->count({}), qint64(1));
    }

    void nonAsciiAndSpacedPathsWork()
    {
        const QString folder = QStringLiteral("Bibliothèque 文献");
        QDir(docs()).mkpath(folder);
        const QString relative = folder + QStringLiteral("/Résumé de thèse (final).pdf");
        QVERIFY(QFile::copy(sample("article.pdf"), docPath(relative.toUtf8().constData())));

        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("Fatigue of Steel Beams")); // It really was opened and read.
        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.relativePath, relative);
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY(QFile::exists(m_service->coverPath(file.sha256)));
    }

    void nothingInTheLibraryFolderIsEverChanged()
    {
        // Every sample, including the bad ones.
        QDirIterator it(QStringLiteral(PAPERTRAIL_TEST_SAMPLES), QDir::Files);
        while (it.hasNext()) {
            it.next();
            QVERIFY(QFile::copy(it.filePath(), docs() + u'/' + it.fileName()));
        }

        const QMap<QString, QPair<QByteArray, QDateTime>> before = fingerprintFolder();
        QVERIFY(before.size() > 10);

        m_service->scan(m_root);
        m_service->processQueue();
        m_service->scan(m_root);
        m_service->processQueue();

        const QMap<QString, QPair<QByteArray, QDateTime>> after = fingerprintFolder();
        QCOMPARE(after.keys(), before.keys()); // Nothing added or removed.
        for (auto entry = before.constBegin(); entry != before.constEnd(); ++entry) {
            QCOMPARE(after.value(entry.key()).first, entry.value().first);
            QCOMPARE(after.value(entry.key()).second, entry.value().second);
        }
    }

    void anArxivStampIsNotEnoughToSortAPreprint()
    {
        copySample("preprint.pdf");
        m_service->scan(m_root);
        m_service->processQueue();
        const Publication p = *m_publications->get(m_query->ids({}, {}).first());
        QCOMPARE(p.type, PublicationType::Other);
        QVERIFY(p.needsReview); // It's a guess, and the arXiv number is still kept.
        QCOMPARE(p.arxivId, QStringLiteral("2101.00001v2"));
    }

    void automaticSortingIntoPreprintsIsUndone()
    {
        copySample("preprint_stamp.pdf"); // Moved by the check that used to run on existing libraries.
        copySample("preprint.pdf");       // Sorted when it was imported, by the earlier rule.
        copySample("article.pdf");        // Marked by the user.
        copySample("thesis.pdf");         // Never involved.
        m_service->scan(m_root);
        m_service->processQueue();

        qint64 moved = 0;
        qint64 sorted = 0;
        qint64 marked = 0;
        qint64 untouched = 0;
        for (const qint64 id : m_query->ids({}, {})) {
            Publication p = *m_publications->get(id);
            if (p.title == QStringLiteral("Measuring Example Flows")) {
                moved = id;
                p.type = PublicationType::Preprint;
                m_publications->updateWithProvenance(p, QStringLiteral("classifier"));
            } else if (p.title == QStringLiteral("Sampling Made Simple")) {
                sorted = id;
                p.type = PublicationType::Preprint;
                p.needsReview = false;
                m_publications->update(p);
            } else if (p.type == PublicationType::JournalArticle) {
                marked = id;
                p.type = PublicationType::Preprint;
                m_publications->updateWithProvenance(p, QStringLiteral("manual"));
            } else {
                untouched = id;
            }
        }
        QVERIFY(moved && sorted && marked && untouched);
        const PublicationType untouchedType = m_publications->get(untouched)->type;
        const QMap<QString, QPair<QByteArray, QDateTime>> before = fingerprintFolder();

        // A check left waiting from the old version isn't run.
        m_db->exec("INSERT INTO job (kind, payload, created_at, updated_at) "
                   "SELECT 'reclassify', json_object('file', id), 0, 0 FROM file");
        m_db->setUserVersion(3);
        migrateLibrary(*m_db);

        // The one with a recorded earlier type goes back at once.
        QCOMPARE(m_publications->get(moved)->type, PublicationType::JournalArticle);
        QCOMPARE(m_service->pendingJobs(), 1);

        const ImportReport report = m_service->processQueue();
        QCOMPARE(report.reclassified, 1);
        QCOMPARE(report.publicationsCreated, 0);
        QCOMPARE(m_publications->get(sorted)->type, PublicationType::Other);
        QVERIFY(m_publications->get(sorted)->needsReview);
        QCOMPARE(m_publications->get(marked)->type, PublicationType::Preprint);
        QCOMPARE(m_publications->get(untouched)->type, untouchedType);
        QCOMPARE(m_query->count([] { PublicationFilter f; f.tabTypes = typesForTab(LibraryTab::Preprints); return f; }()), 1);
        QCOMPARE(fingerprintFolder(), before);
    }

    void fileStillChangingIsLeftForLater()
    {
        copySample("article.pdf");
        QFile copied(docPath("article.pdf"));
        QVERIFY(copied.open(QIODevice::ReadWrite));
        QVERIFY(copied.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime));
        copied.close();

        ImportOptions options = serviceOptions();
        options.settleMs = 3600000;
        ImportService settling(*m_db, options);
        const ScanReport report = settling.scan(m_root);
        QCOMPARE(report.stillChanging, 1);
        QCOMPARE(report.newFiles, 0);
        QCOMPARE(settling.pendingJobs(), 0);

        // Once it's stopped changing, it goes in as normal.
        options.settleMs = 0;
        ImportService settled(*m_db, options);
        QCOMPARE(settled.scan(m_root).newFiles, 1);
    }

    void cancellingAScanDoesNotMarkFilesMissing()
    {
        copySample("article.pdf");
        m_service->scan(m_root);
        m_service->processQueue();
        QCOMPARE(m_query->flagCounts({}).missingOrOffline, 0);

        ImportOptions options = serviceOptions();
        options.cancelled = [] { return true; };
        ImportService cancelled(*m_db, options);
        const ScanReport report = cancelled.scan(m_root);
        QVERIFY(report.stoppedEarly);
        QCOMPARE(report.nowMissing, 0);
        QCOMPARE(m_query->flagCounts({}).missingOrOffline, 0);
    }

    void cancellingDoesNotCountAgainstAFile()
    {
        // Hangs until it's cancelled, far sooner than the timeout.
        const QString name = QStringLiteral("__hang__.pdf");
        QVERIFY(QFile::copy(sample("article.pdf"), docs() + u'/' + name));
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");

        std::atomic_bool stop = false;
        ImportOptions options = serviceOptions();
        options.cancelled = [&] { return stop.load(); };
        {
            ImportService service(*m_db, options);
            service.scan(m_root);
            QElapsedTimer timer;
            timer.start();
            std::thread stopper([&] {
                QThread::msleep(500);
                stop = true;
            });
            const ImportReport report = service.processQueue();
            stopper.join();
            QVERIFY(report.stoppedEarly);
            QCOMPARE(report.unreadable, 0);
            QVERIFY(timer.elapsed() < 30000);
            QCOMPARE(service.pendingJobs(), 1);
        }
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");

        auto attempts = m_db->prepare("SELECT attempts, status FROM job");
        QVERIFY(attempts->step());
        QCOMPARE(attempts->intAt(0), 0);
        QCOMPARE(attempts->textAt(1), QStringLiteral("pending"));
    }

    void anInterruptedJobIsNotHeldAgainstItsFile()
    {
        copySample("article.pdf");
        m_service->scan(m_root);
        m_db->exec("UPDATE job SET status = 'running', attempts = 1");
        m_service = makeService(); // As when starting up after the app was closed part way.
        auto attempts = m_db->prepare("SELECT attempts, status FROM job");
        QVERIFY(attempts->step());
        QCOMPARE(attempts->intAt(0), 0);
        QCOMPARE(attempts->textAt(1), QStringLiteral("pending"));
    }

    void aLibraryFolderInsideAnotherIsOnlyScannedOnce()
    {
        QDir(docs()).mkpath(QStringLiteral("inner"));
        QVERIFY(QFile::copy(sample("article.pdf"), docs() + QStringLiteral("/inner/article.pdf")));
        copySample("book.epub");

        const qint64 inner = m_files->addRoot(QStringLiteral("Inner"), docs() + QStringLiteral("/inner"));
        QCOMPARE(m_service->scan(m_root).newFiles, 1);
        QCOMPARE(m_service->scan(inner).newFiles, 1);
        QVERIFY(isPathInside(QStringLiteral("C:/a/b/c.pdf"), QStringLiteral("C:/a/b")));
        QVERIFY(isPathInside(QStringLiteral("C:/a/b"), QStringLiteral("C:/a/b")));
        QVERIFY(!isPathInside(QStringLiteral("C:/a/bc/d.pdf"), QStringLiteral("C:/a/b")));
        QVERIFY(isPathInside(QStringLiteral("P:/x.pdf"), QStringLiteral("P:/")));
    }

    void summariesCarryWhatTheLibraryViewShows()
    {
        copySample("article.pdf");
        m_service->scan(m_root);
        m_service->processQueue();
        const QList<PublicationSummary> rows = m_query->page({}, {}, 0, 10);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().format, QStringLiteral("pdf"));
        QCOMPARE(rows.first().fileStatus, QStringLiteral("ok"));
        QVERIFY(!rows.first().coverSha.isEmpty());
        QVERIFY(QFile::exists(coverPathFor(m_dir->filePath(QStringLiteral("cache")), rows.first().coverSha)));
    }

private:
    ImportOptions serviceOptions() const
    {
        ImportOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        return options;
    }

    std::unique_ptr<ImportService> makeService() { return std::make_unique<ImportService>(*m_db, serviceOptions()); }

    QString docs() const { return m_dir->filePath(QStringLiteral("docs")); }
    QString docPath(const char *name) const { return docs() + u'/' + QString::fromUtf8(name); }
    static QString sample(const char *name) { return QStringLiteral(PAPERTRAIL_TEST_SAMPLES) + u'/' + QString::fromUtf8(name); }

    void copySample(const char *name, const char *as = nullptr)
    {
        QVERIFY2(QFile::copy(sample(name), docPath(as ? as : name)), name);
    }

    static QByteArray readAll(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    // Imports whatever is in the folder and returns the only publication.
    Publication importOne()
    {
        m_service->scan(m_root);
        m_service->processQueue();
        const QList<qint64> ids = m_query->ids({}, {});
        if (ids.size() != 1) {
            qWarning("expected 1 publication, found %lld", qlonglong(ids.size()));
            return {};
        }
        return *m_publications->get(ids.first());
    }

    QMap<QString, QPair<QByteArray, QDateTime>> fingerprintFolder() const
    {
        QMap<QString, QPair<QByteArray, QDateTime>> result;
        QDirIterator it(docs(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QByteArray hash = QCryptographicHash::hash(readAll(path), QCryptographicHash::Sha256);
            result.insert(path, {hash, it.fileInfo().lastModified()});
        }
        return result;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<FileRepository> m_files;
    std::unique_ptr<PublicationRepository> m_publications;
    std::unique_ptr<LibraryQuery> m_query;
    std::unique_ptr<ImportService> m_service;
    qint64 m_root = 0;
};

QTEST_GUILESS_MAIN(ImportTest)
#include "test_import.moc"
