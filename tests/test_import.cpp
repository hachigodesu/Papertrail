// End-to-end import tests, using the real worker process and real sample documents.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "import/ImportService.h"
#include "import/JournalIssues.h"
#include "core/repo/VolumeArticles.h"

#include "KindleSamples.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
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

    void issueTitlesThatOnlyDescribeAnIssueAreNoticed_data()
    {
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("journal");
        QTest::addColumn<bool>("descriptive");
        QTest::newRow("information page") << "Made-Up Transactions on Examples Publication Information" << "" << true;
        QTest::newRow("cover") << "OCTOBER 2026 VOLUME 18 NUMBER 5 MUTE (ISSN 1234-5679)" << "" << true;
        QTest::newRow("numbers") << "Vol. 3, No. 2" << "" << true;
        QTest::newRow("contents") << "Table of Contents" << "" << true;
        QTest::newRow("the journal alone") << "Journal of Imaginary Findings" << "journal of imaginary findings" << true;
        QTest::newRow("empty") << "" << "" << true;
        QTest::newRow("a real title") << "A Special Issue on Pretend Crystals" << "Journal of Imaginary Findings" << false;
        QTest::newRow("journal, no other") << "Journal of Imaginary Findings" << "" << false;
    }

    void issueTitlesThatOnlyDescribeAnIssueAreNoticed()
    {
        QFETCH(QString, title);
        QFETCH(QString, journal);
        QFETCH(bool, descriptive);
        QCOMPARE(isDescriptiveIssueTitle(title, journal), descriptive);
    }

    void anIssueGetsATitleFromItsDetails()
    {
        Publication issue;
        issue.type = PublicationType::JournalIssue;
        QVERIFY(issueTitle(issue).isEmpty()); // Not without its journal.
        issue.containerTitle = QStringLiteral("Journal of Imaginary Findings");
        QCOMPARE(issueTitle(issue), QStringLiteral("Journal of Imaginary Findings"));
        issue.volume = QStringLiteral("7");
        issue.issue = QStringLiteral("11");
        issue.year = 2026;
        QCOMPARE(issueTitle(issue), QStringLiteral("Journal of Imaginary Findings, Vol. 7, No. 11 (2026)"));
        issue.month = 11;
        QCOMPARE(issueTitle(issue), QStringLiteral("Journal of Imaginary Findings, Vol. 7, No. 11 (November 2026)"));
    }

    void anIssueIsPutInOrder()
    {
        // Titled from its cover, with no journal yet.
        Publication cover;
        cover.type = PublicationType::JournalIssue;
        cover.title = QStringLiteral("OCTOBER 2026 VOLUME 18 NUMBER 5 MUTE (ISSN 1234-5679)");
        const qint64 id = m_publications->create(cover);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article Inside");
        const qint64 article = createArticles(*m_db, id, {draft}, false).first();
        QCOMPARE(m_publications->get(article)->containerTitle, cover.title); // It had nothing better then.

        // Its numbers and ISSN come from the cover's words, but without a journal, the title stays.
        QVERIFY(tidyJournalIssue(*m_db, id));
        Publication issue = *m_publications->get(id);
        QCOMPARE(issue.volume, QStringLiteral("18"));
        QCOMPARE(issue.issue, QStringLiteral("5"));
        QCOMPARE(issue.year.value(), 2026);
        QCOMPARE(issue.month.value(), 10);
        QCOMPARE(issue.issn, QStringLiteral("1234-5679"));
        QCOMPARE(issue.title, cover.title);
        // The article that took the cover's words as its journal's name has them taken away again.
        QVERIFY(m_publications->get(article)->containerTitle.isEmpty());
        QVERIFY(!tidyJournalIssue(*m_db, id)); // Nothing more to do.

        // An article's lookup brings the journal, and with it a proper title, for the issue and its articles.
        Publication lookedUp = *m_publications->get(article);
        lookedUp.containerTitle = QStringLiteral("Made-Up Letters");
        lookedUp.publisher = QStringLiteral("Example Society");
        m_publications->updateWithProvenance(lookedUp, QStringLiteral("crossref"));
        ArticleDraft another;
        another.title = QStringLiteral("Another Inside");
        const qint64 sibling = createArticles(*m_db, id, {another}, false).first();
        QVERIFY(tidyJournalIssue(*m_db, id, m_publications->get(article)));
        issue = *m_publications->get(id);
        QCOMPARE(issue.containerTitle, QStringLiteral("Made-Up Letters"));
        QCOMPARE(issue.publisher, QStringLiteral("Example Society"));
        QCOMPARE(issue.title, QStringLiteral("Made-Up Letters, Vol. 18, No. 5 (October 2026)"));
        QCOMPARE(m_publications->get(sibling)->containerTitle, QStringLiteral("Made-Up Letters"));
        // And it can be undone, like any other change.
        QVERIFY(!m_publications->provenance(id, QStringLiteral("title")).isEmpty());

        // A title typed by hand is never replaced.
        Publication named = issue;
        named.title = QStringLiteral("Table of Contents");
        m_publications->updateWithProvenance(named, QStringLiteral("manual"));
        QVERIFY(!tidyJournalIssue(*m_db, id));
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("Table of Contents"));

        // Every issue at once, as when a library opens.
        Publication information;
        information.type = PublicationType::JournalIssue;
        information.title = QStringLiteral("Journal of Imaginary Findings Publication Information");
        information.containerTitle = QStringLiteral("Journal of Imaginary Findings");
        information.volume = QStringLiteral("3");
        const qint64 other = m_publications->create(information);
        // One titled with its journal's name alone takes it as its journal, and its numbers in the title.
        Publication named2;
        named2.type = PublicationType::JournalIssue;
        named2.title = QStringLiteral("Annals of Examples");
        named2.volume = QStringLiteral("12");
        const qint64 annals = m_publications->create(named2);
        QCOMPARE(tidyJournalIssues(*m_db), 2);
        QCOMPARE(m_publications->get(other)->title, QStringLiteral("Journal of Imaginary Findings, Vol. 3"));
        QCOMPARE(m_publications->get(annals)->containerTitle, QStringLiteral("Annals of Examples"));
        QCOMPARE(m_publications->get(annals)->title, QStringLiteral("Annals of Examples, Vol. 12"));
    }

    void theWorkerExitsCleanlyWithADocumentOpen()
    {
        // A document kept open between requests once outlived what opened it, and crashed the worker as it closed.
        for (const QString &op : {QStringLiteral("pagetext"), QStringLiteral("cover")}) {
            QProcess worker;
            worker.start(QStringLiteral(PAPERTRAIL_WORKER_PATH), QStringList());
            QVERIFY(worker.waitForStarted());
            QJsonObject request{{QStringLiteral("id"), 1},
                                {QStringLiteral("op"), op},
                                {QStringLiteral("path"), QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(QStringLiteral("reader_doc.pdf"))},
                                {QStringLiteral("start"), 0},
                                {QStringLiteral("count"), 2},
                                {QStringLiteral("page"), 1},
                                {QStringLiteral("coverPath"), m_dir->filePath(QStringLiteral("cover.jpg"))}};
            worker.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
            QVERIFY(worker.waitForReadyRead(30000));
            QVERIFY(worker.readAll().contains("\"ok\":true"));
            worker.closeWriteChannel();
            QVERIFY(worker.waitForFinished(30000));
            QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
            QCOMPARE(worker.exitCode(), 0);
        }
        QVERIFY(QFile::exists(m_dir->filePath(QStringLiteral("cover.jpg"))));
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

    void aKindleFileThatCantBeConvertedIsStillListed()
    {
        // This sample only has a real header, with nothing behind it to convert.
        copySample("kf8_header.azw3");
        const Publication p = importOne();
        QCOMPARE(p.title, QStringLiteral("New Kindle Book"));
        QCOMPARE(p.authors.at(0).family, QStringLiteral("Sample"));
        QCOMPARE(p.publisher, QStringLiteral("Example Press"));
        QCOMPARE(p.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(p.year, 2021);

        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.format, QStringLiteral("azw3"));
        QCOMPARE(file.status, FileStatus::Unreadable);
        QVERIFY(!file.problem.isEmpty());
    }

    void aKf8BookIsConvertedAndReadInFull()
    {
        copySample("kf8_frankenstein.azw3", "Fr\u00E4nkenstein \u30D6\u30C3\u30AF.azw3");
        const QByteArray before = hashOf(docs() + QStringLiteral("/Fr\u00E4nkenstein \u30D6\u30C3\u30AF.azw3"));
        const Publication p = importOne();
        QVERIFY2(p.title.contains(QStringLiteral("Frankenstein")), qPrintable(p.title));

        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.format, QStringLiteral("azw3"));
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY(file.pageCount.value_or(0) > 50);
        QCOMPARE(file.hasTextLayer, true);
        const QString cache = m_dir->filePath(QStringLiteral("cache"));
        QVERIFY(QFile::exists(coverPathFor(cache, file.sha256)));
        QVERIFY(QFile::exists(convertedPathFor(cache, file.sha256)));

        // The original isn't touched, whatever's made from it.
        QCOMPARE(hashOf(docs() + QStringLiteral("/Fr\u00E4nkenstein \u30D6\u30C3\u30AF.azw3")), before);
    }

    void aJointKindleFileReadsItsKf8Part()
    {
        const QByteArray joint = kindle_samples::jointFile(kindle_samples::readAll(sample("old_format.mobi")),
                                                           kindle_samples::readAll(sample("kf8_frankenstein.azw3")));
        QVERIFY(kindle_samples::writeAll(docPath("joint.mobi"), joint));
        const Publication p = importOne();
        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY2(file.pageCount.value_or(0) > 50, qPrintable(QString::number(file.pageCount.value_or(0))));
        QVERIFY(QFile::exists(convertedPathFor(m_dir->filePath(QStringLiteral("cache")), file.sha256)));
    }

    void aBookLockedWithDrmSaysSo()
    {
        QVERIFY(kindle_samples::writeAll(docPath("locked.azw3"),
                                         kindle_samples::lockedFile(kindle_samples::readAll(sample("kf8_frankenstein.azw3")))));
        const Publication p = importOne();
        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.status, FileStatus::Unreadable);
        QVERIFY2(file.problem.contains(QStringLiteral("DRM")), qPrintable(file.problem));
        QVERIFY(!QFile::exists(convertedPathFor(m_dir->filePath(QStringLiteral("cache")), file.sha256)));
    }

    void everyFormatGetsItsFacts_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("hasText");
        QTest::newRow("PDF") << "article.pdf" << "pdf" << true;
        QTest::newRow("EPUB") << "book.epub" << "epub" << true;
        QTest::newRow("DjVu with text") << "with_text.djvu" << "djvu" << true;
        QTest::newRow("DjVu without") << "scanned.djvu" << "djvu" << false;
        QTest::newRow("MOBI") << "old_format.mobi" << "mobi" << true;
        QTest::newRow("FB2") << "sample.fb2" << "fb2" << true;
        QTest::newRow("CBZ") << "sample.cbz" << "cbz" << false;
        QTest::newRow("XPS") << "sample.xps" << "xps" << false;
    }

    void everyFormatGetsItsFacts()
    {
        QFETCH(QString, name);
        QFETCH(QString, format);
        QFETCH(bool, hasText);
        copySample(name.toUtf8().constData());
        const Publication p = importOne();
        const FileRecord file = m_files->filesFor(p.id).first();
        QCOMPARE(file.format, format);
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY(file.pageCount.value_or(0) >= 1);
        QCOMPARE(file.hasTextLayer, hasText);
        QVERIFY(QFile::exists(coverPathFor(m_dir->filePath(QStringLiteral("cache")), file.sha256)));
    }

    void otherNamesForFormatsWork_data()
    {
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("as");
        QTest::addColumn<QString>("format");
        QTest::newRow(".azw") << "old_format.mobi" << "old.azw" << "azw3";
        QTest::newRow(".oxps") << "sample.xps" << "sample.oxps" << "xps";
        QTest::newRow(".djv") << "with_text.djvu" << "sample.djv" << "djvu";
    }

    void otherNamesForFormatsWork()
    {
        QFETCH(QString, from);
        QFETCH(QString, as);
        QFETCH(QString, format);
        copySample(from.toUtf8().constData(), as.toUtf8().constData());
        const FileRecord file = m_files->filesFor(importOne().id).first();
        QCOMPARE(file.format, format);
        QCOMPARE(file.status, FileStatus::Ok);
        QVERIFY(file.pageCount.value_or(0) >= 1);
    }

    void kindleBooksAlreadyInALibraryGetTheirCovers()
    {
        copySample("kf8_frankenstein.azw3");
        const Publication p = importOne();
        const FileRecord imported = m_files->filesFor(p.id).first();
        const QString cover = coverPathFor(m_dir->filePath(QStringLiteral("cache")), imported.sha256);

        // As it would have been before these could be read.
        QVERIFY(QFile::remove(cover));
        m_files->setDocumentFacts(imported.id, std::nullopt, std::nullopt);
        Publication edited = *m_publications->get(p.id);
        edited.title = QStringLiteral("My Own Title");
        m_publications->update(edited);

        m_db->setUserVersion(5);
        migrateLibrary(*m_db);
        QCOMPARE(m_service->pendingJobs(), 1);
        const ImportReport report = m_service->processQueue();
        QCOMPARE(report.publicationsCreated, 0);

        const FileRecord file = m_files->filesFor(p.id).first();
        QVERIFY(file.pageCount.value_or(0) > 50);
        QVERIFY(QFile::exists(cover));
        QCOMPARE(m_publications->get(p.id)->title, QStringLiteral("My Own Title"));
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

    void addingARemovedFolderAgainBringsItAllBack()
    {
        copySample("article.pdf");
        const qint64 id = importOne().id;
        Publication edited = *m_publications->get(id);
        edited.title = QStringLiteral("My Corrected Title");
        m_publications->update(edited);

        m_files->hideRoot(m_root);
        QVERIFY(m_files->roots().isEmpty());
        QCOMPARE(m_files->hiddenCount(), qint64(1));

        m_files->restoreRoot(m_root);
        const ScanReport scan = m_service->scan(m_root);
        QCOMPARE(scan.newFiles, 0);
        const ImportReport report = m_service->processQueue();
        QCOMPARE(report.publicationsCreated, 0);
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("My Corrected Title"));
        QCOMPARE(m_files->hiddenCount(), qint64(0));
    }

    void aRemovedFolderMovedElsewhereIsRecognisedByItsContents()
    {
        copySample("article.pdf");
        const qint64 id = importOne().id;
        Publication edited = *m_publications->get(id);
        edited.title = QStringLiteral("My Corrected Title");
        m_publications->update(edited);
        m_files->hideRoot(m_root);

        const QString elsewhere = m_dir->filePath(QStringLiteral("elsewhere"));
        QDir().mkpath(elsewhere);
        QVERIFY(QFile::copy(docPath("article.pdf"), QDir(elsewhere).filePath(QStringLiteral("renamed.pdf"))));
        const qint64 newRoot = m_files->addRoot(QStringLiteral("Elsewhere"), elsewhere);
        m_service->scan(newRoot);
        const ImportReport report = m_service->processQueue();

        QCOMPARE(report.publicationsCreated, 0);
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("My Corrected Title"));
        QCOMPARE(m_files->hiddenCount(), qint64(0));
        const QList<FileRecord> files = m_files->filesFor(id);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().rootId, newRoot);
    }

    void aRemovedFolderDoesntGetScannedThroughWaitingWork()
    {
        copySample("article.pdf");
        QCOMPARE(m_service->scan(m_root).newFiles, 1);
        QCOMPARE(m_service->pendingJobs(), 1);
        m_files->hideRoot(m_root);
        QCOMPARE(m_service->pendingJobs(), 0);
        QCOMPARE(m_service->processQueue().publicationsCreated, 0);
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
        QCOMPARE(m_service->pendingJobs(), 2); // Going through it, and looking for other formats.

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

    void theSameNameInTwoFormatsIsOnePublication_data()
    {
        QTest::addColumn<bool>("pdfFirst");
        QTest::newRow("PDF, then EPUB") << true;
        QTest::newRow("EPUB, then PDF") << false;
    }

    void theSameNameInTwoFormatsIsOnePublication()
    {
        QFETCH(bool, pdfFirst);
        const auto importFile = [&](const char *sampleName, const char *as) {
            copySample(sampleName, as);
            m_service->scan(m_root);
            return m_service->processQueue();
        };
        ImportReport second;
        if (pdfFirst) {
            importFile("article.pdf", "Same Book.pdf");
            second = importFile("book.epub", "Same Book.epub");
        } else {
            importFile("book.epub", "Same Book.epub");
            second = importFile("article.pdf", "Same Book.pdf");
        }
        QCOMPARE(second.formatsJoined, 1);
        QCOMPARE(second.publicationsCreated, 0);
        QCOMPARE(m_query->count({}), 1);

        const qint64 id = m_query->ids({}, {}).first();
        QCOMPARE(m_files->filesFor(id).size(), 2);
        const QList<PublicationSummary> rows = m_query->page({}, {}, 0, 10);
        QCOMPARE(rows.first().formats.size(), 2);
        QVERIFY(rows.first().formats.contains(QStringLiteral("pdf")));
        QVERIFY(rows.first().formats.contains(QStringLiteral("epub")));

        // It can be filtered by either format, and it's counted once.
        PublicationFilter filter;
        filter.formats = {QStringLiteral("epub")};
        QCOMPARE(m_query->count(filter), 1);
        filter.formats = {QStringLiteral("pdf")};
        QCOMPARE(m_query->count(filter), 1);
    }

    void aSecondFormatFillsInWhatTheFirstLacked()
    {
        copySample("no_metadata.pdf", "A Book.pdf"); // Its title is only a guess.
        m_service->scan(m_root);
        m_service->processQueue();
        const qint64 id = m_query->ids({}, {}).first();
        const Publication before = *m_publications->get(id);
        QVERIFY(before.needsReview);

        copySample("book.epub", "A Book.epub"); // This one states its title and more.
        m_service->scan(m_root);
        m_service->processQueue();
        QCOMPARE(m_query->count({}), 1);
        const Publication after = *m_publications->get(id);
        QVERIFY(after.title != before.title);
        QVERIFY(!after.authors.isEmpty());
    }

    void aTitleChosenByHandIsNotReplacedByASecondFormat()
    {
        copySample("no_metadata.pdf", "A Book.pdf");
        m_service->scan(m_root);
        m_service->processQueue();
        const qint64 id = m_query->ids({}, {}).first();
        Publication p = *m_publications->get(id);
        p.title = QStringLiteral("My Own Title");
        m_publications->updateWithProvenance(p, QStringLiteral("manual"));

        copySample("book.epub", "A Book.epub");
        m_service->scan(m_root);
        m_service->processQueue();
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("My Own Title"));
    }

    void theSameFormatTwiceStaysApart()
    {
        // Two PDFs with one name can't be in one folder, and with different contents in two folders
        // they may be different editions. They're only ever joined if they're byte-for-byte the same.
        QDir(docs()).mkpath(QStringLiteral("a"));
        QDir(docs()).mkpath(QStringLiteral("b"));
        copySample("article.pdf", "a/Same.pdf");
        copySample("thesis.pdf", "b/Same.pdf");
        m_service->scan(m_root);
        m_service->processQueue();
        QCOMPARE(m_query->count({}), 2);
    }

    void theSameNameInDifferentFoldersStaysApart()
    {
        QDir(docs()).mkpath(QStringLiteral("a"));
        QDir(docs()).mkpath(QStringLiteral("b"));
        copySample("article.pdf", "a/notes.pdf");
        copySample("book.epub", "b/notes.epub");
        m_service->scan(m_root);
        m_service->processQueue();
        QCOMPARE(m_query->count({}), 2);
    }

    void existingLibrariesAreLookedThroughOnce_data()
    {
        QTest::addColumn<QString>("titleA");
        QTest::addColumn<QString>("titleB");
        QTest::addColumn<QString>("authorB");
        QTest::addColumn<int>("yearB");
        QTest::addColumn<QString>("isbnB");
        QTest::addColumn<QString>("formatB");
        QTest::addColumn<int>("publications");
        QTest::addColumn<bool>("marked");

        const QString title = QStringLiteral("Structural Analysis of Beams");
        const QString isbn = QStringLiteral("9780306406157");
        const QString epub = QStringLiteral("epub");
        const QString hooke = QStringLiteral("Hooke");
        QTest::newRow("same ISBN and title") << title << title << hooke << 2001 << isbn << epub << 1 << false;
        QTest::newRow("same ISBN, and a subtitle on one")
            << title << title + QStringLiteral(": A Primer") << hooke << 2001 << isbn << epub << 1 << false;
        QTest::newRow("same ISBN but another title: a chapter of the book")
            << title << QStringLiteral("Chapter Nine of a Book") << hooke << 2001 << isbn << epub << 2 << false;
        QTest::newRow("same ISBN and title, but also the same format")
            << title << title << hooke << 2001 << isbn << QStringLiteral("pdf") << 2 << false;
        QTest::newRow("no ISBN, same title, author and year")
            << title << title << hooke << 2001 << QString() << epub << 1 << true;
        QTest::newRow("no ISBN, and another author")
            << title << title << QStringLiteral("Euler") << 2001 << QString() << epub << 2 << false;
        QTest::newRow("no ISBN, and another year: perhaps another edition")
            << title << title << hooke << 2015 << QString() << epub << 2 << false;
        QTest::newRow("no ISBN, and a short common title")
            << QStringLiteral("Notes") << QStringLiteral("Notes") << hooke << 2001 << QString() << epub << 2 << false;
    }

    void existingLibrariesAreLookedThroughOnce()
    {
        QFETCH(QString, titleA);
        QFETCH(QString, titleB);
        QFETCH(QString, authorB);
        QFETCH(int, yearB);
        QFETCH(QString, isbnB);
        QFETCH(QString, formatB);
        QFETCH(int, publications);
        QFETCH(bool, marked);

        const auto add = [&](const QString &name, const QString &title, const QString &author, int year,
                             const QString &isbn, const QString &format) {
            Publication p;
            p.title = title;
            p.year = year;
            p.isbn13 = isbn;
            p.authors = {{author, QString()}};
            const qint64 id = m_publications->create(p);
            FileRecord file;
            file.rootId = m_root;
            file.relativePath = name;
            file.size = 100;
            file.modifiedAt = 1;
            file.sha256 = QString(name).leftJustified(64, u'0');
            file.format = format;
            m_files->upsert(file);
            m_files->link(id, file.id, true);
            return id;
        };
        add(QStringLiteral("first.pdf"), titleA, QStringLiteral("Hooke"), 2001, isbnB, QStringLiteral("pdf"));
        add(QStringLiteral("second.") + formatB, titleB, authorB, yearB, isbnB, formatB);

        m_db->setUserVersion(4);
        migrateLibrary(*m_db);
        QCOMPARE(m_service->pendingJobs(), 1);
        const ImportReport report = m_service->processQueue();
        QCOMPARE(m_query->count({}), publications);
        QCOMPARE(report.formatsJoined, publications == 1 ? 1 : 0);
        if (publications == 1) {
            const qint64 id = m_query->ids({}, {}).first();
            QCOMPARE(m_files->filesFor(id).size(), 2);
            QCOMPARE(m_publications->get(id)->needsReview, marked);
        }
        QCOMPARE(m_service->pendingJobs(), 0);
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
    static QByteArray hashOf(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        return hash.result();
    }

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
