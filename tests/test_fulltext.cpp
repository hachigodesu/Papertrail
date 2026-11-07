// Tests of searching inside documents.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/query/SearchParser.h"
#include "core/repo/FileRepository.h"
#include "core/repo/FullTextIndex.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/VolumeArticles.h"
#include "import/FullTextIndexer.h"
#include "import/ImportService.h"
#include "ui/DetailsPanel.h"
#include "ui/ImportThread.h"
#include "ui/IndexThread.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/PageView.h"
#include "ui/PublicationListModel.h"
#include "ui/ReaderTab.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::ui;

namespace {

constexpr int kLongWaitMs = 60000;

QString sample(const QString &name)
{
    return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name);
}

QByteArray folderHash(const QString &folder)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QDir dir(folder);
    for (const QString &name : dir.entryList(QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::ReadOnly))
            hash.addData(&file);
    }
    return hash.result();
}

} // namespace

class FullTextTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_root = 0;
        m_dir = std::make_unique<QTemporaryDir>();
        m_docs = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(m_docs);
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        attachFullText(*m_db, fullTextPathFor(m_dir->filePath(QStringLiteral("library.db"))));
        m_publications = std::make_unique<PublicationRepository>(*m_db);
        m_files = std::make_unique<FileRepository>(*m_db);
        m_index = std::make_unique<FullTextIndex>(*m_db);
        m_query = std::make_unique<LibraryQuery>(*m_db);
    }

    void cleanup()
    {
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
        m_unexpectedBoxes.clear();
        m_window.reset();
        m_session.reset();
        m_query.reset();
        m_index.reset();
        m_files.reset();
        m_publications.reset();
        m_db.reset();
        m_dir.reset();
    }

    // ---- What's searched for ----

    void onlyPlainWordsAreLookedForInsideDocuments()
    {
        const ParsedSearch free = parseSearch(QStringLiteral("fatigue \"steel beam\" author:knuth year:2010 doi:10.1/x"));
        QVERIFY(free.contentsMatch.contains(QStringLiteral("\"fatigue\"*")));
        QVERIFY(free.contentsMatch.contains(QStringLiteral("\"steel beam\"")));
        QVERIFY(!free.contentsMatch.contains(QStringLiteral("knuth")));
        QVERIFY(free.fieldMatch.contains(QStringLiteral("knuth")));
        QVERIFY(free.ftsMatch.contains(QStringLiteral("knuth")));
        QVERIFY(free.ftsMatch.contains(QStringLiteral("fatigue")));

        // What a reader searches for: a phrase if there is one, or else the longest word.
        QCOMPARE(free.contentsText, QStringLiteral("steel beam"));
        QCOMPARE(parseSearch(QStringLiteral("a longest word")).contentsText, QStringLiteral("longest"));
        QCOMPARE(parseSearch(QStringLiteral("author:knuth")).contentsText, QString());
        QCOMPARE(parseSearch(QStringLiteral("author:knuth")).contentsMatch, QString());
    }

    void wordsSplitOverALineAreJoined()
    {
        QCOMPARE(tidiedPageText(QStringLiteral("a fatigue crack propa-\ngation study")), QStringLiteral("a fatigue crack propagation study"));
        QCOMPARE(tidiedPageText(QStringLiteral("propa-  \r\n  gation")), QStringLiteral("propagation"));
        // A capital after it is more likely to be a name or a new sentence.
        QCOMPARE(tidiedPageText(QStringLiteral("Smith-\nJones")), QStringLiteral("Smith-\nJones"));
        QCOMPARE(tidiedPageText(QStringLiteral("well-known")), QStringLiteral("well-known"));
    }

    // ---- The index ----

    void attachingTwiceChangesNothing()
    {
        attachFullText(*m_db, fullTextPathFor(m_dir->filePath(QStringLiteral("library.db"))));
        QVERIFY(hasFullText(*m_db));
        QCOMPARE(m_index->indexedCount(), qint64(0));
    }

    void aLibraryWithoutTheIndexStillWorks()
    {
        QTemporaryDir other;
        Database db(other.filePath(QStringLiteral("other.db")));
        migrateLibrary(db);
        LibraryQuery query(db);
        PublicationFilter filter;
        filter.searchText = QStringLiteral("anything");
        filter.searchContents = true;
        QCOMPARE(query.count(filter), qint64(0));
        QVERIFY(!hasFullText(db));
    }

    void contentsAreOnlySearchedWhenAskedFor()
    {
        const qint64 article = addIndexed(QStringLiteral("Title Without It"), {QStringLiteral("the secretword is here")});
        addIndexed(QStringLiteral("Unrelated"), {QStringLiteral("nothing of interest")});

        QCOMPARE(m_query->count(search(QStringLiteral("secretword"), false)), qint64(0));
        QCOMPARE(m_query->count(search(QStringLiteral("secretword"), true)), qint64(1));
        QCOMPARE(m_query->ids(search(QStringLiteral("secretword"), true), {}), QList<qint64>{article});
    }

    void metadataStillMatchesWhenSearchingContents()
    {
        const qint64 byTitle = addIndexed(QStringLiteral("About Fatigue"), {QStringLiteral("no mention")});
        const qint64 byText = addIndexed(QStringLiteral("Something Else"), {QStringLiteral("all about fatigue here")});
        addIndexed(QStringLiteral("Neither"), {QStringLiteral("no mention")});

        const QList<qint64> ids = m_query->ids(search(QStringLiteral("fatigue"), true), {});
        QCOMPARE(ids.size(), 2);
        QVERIFY(ids.contains(byTitle));
        QVERIFY(ids.contains(byText));
    }

    void bestMatchOrderStillGivesEverything()
    {
        addIndexed(QStringLiteral("About Fatigue"), {QStringLiteral("no mention")});
        addIndexed(QStringLiteral("Something Else"), {QStringLiteral("all about fatigue here")});
        const QList<PublicationSummary> page = m_query->page(search(QStringLiteral("fatigue"), true), {SortField::Relevance, true}, 0, 10);
        QCOMPARE(page.size(), 2);
    }

    void phrasesAndWordsAreFoundOnOnePage()
    {
        const qint64 together = addIndexed(QStringLiteral("One"), {QStringLiteral("a steel beam here")});
        addIndexed(QStringLiteral("Two"), {QStringLiteral("a beam made of steel")});
        const qint64 splitOverPages = addIndexed(QStringLiteral("Three"), {QStringLiteral("steel"), QStringLiteral("beam")});

        QCOMPARE(m_query->ids(search(QStringLiteral("\"steel beam\""), true), {}), QList<qint64>{together});
        const QList<qint64> words = m_query->ids(search(QStringLiteral("steel beam"), true), {});
        QCOMPARE(words.size(), 2);
        QVERIFY(!words.contains(splitOverPages)); // Each page is looked at on its own.
    }

    void diacriticsDontMatter()
    {
        const qint64 id = addIndexed(QStringLiteral("One"), {QString::fromUtf8("une th\xc3\xa9orie nouvelle")});
        QCOMPARE(m_query->ids(search(QStringLiteral("theorie"), true), {}), QList<qint64>{id});
    }

    void aFieldSearchNarrowsWhatsFoundInside()
    {
        const qint64 byKnuth = addIndexed(QStringLiteral("One"), {QStringLiteral("some fatigue here")}, QStringLiteral("Knuth"));
        addIndexed(QStringLiteral("Two"), {QStringLiteral("some fatigue here")}, QStringLiteral("Other"));

        QCOMPARE(m_query->ids(search(QStringLiteral("fatigue author:knuth"), true), {}), QList<qint64>{byKnuth});
        // With nothing to look for inside, there's only the ordinary search.
        QCOMPARE(m_query->ids(search(QStringLiteral("author:knuth"), true), {}), QList<qint64>{byKnuth});
    }

    void otherFiltersStillApply()
    {
        const qint64 article = addIndexed(QStringLiteral("One"), {QStringLiteral("some fatigue here")});
        addIndexed(QStringLiteral("Two"), {QStringLiteral("some fatigue here")}, QString(), PublicationType::Thesis);
        PublicationFilter filter = search(QStringLiteral("fatigue"), true);
        filter.types = {PublicationType::Other};
        QCOMPARE(m_query->ids(filter, {}), QList<qint64>{article});
        QCOMPARE(m_query->count(filter), qint64(1));
    }

    void aNewSearchSeesWhatsBeenIndexedSince()
    {
        QCOMPARE(m_query->count(search(QStringLiteral("laterword"), true)), qint64(0));
        addIndexed(QStringLiteral("One"), {QStringLiteral("the laterword now")});
        QCOMPARE(m_query->count(search(QStringLiteral("laterword"), true)), qint64(1));
    }

    void aHiddenPublicationIsntFound()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("Gone"), m_dir->filePath(QStringLiteral("gone")));
        addIndexed(QStringLiteral("One"), {QStringLiteral("the hiddenword")}, QString(), PublicationType::Other, root);
        QCOMPARE(m_query->count(search(QStringLiteral("hiddenword"), true)), qint64(1));
        m_files->hideRoot(root);
        QCOMPARE(m_query->count(search(QStringLiteral("hiddenword"), true)), qint64(0));
    }

    void articlesAreFoundByTheirOwnPages()
    {
        Publication volumeRecord;
        volumeRecord.type = PublicationType::JournalIssue;
        volumeRecord.title = QStringLiteral("A Volume");
        const qint64 volume = m_publications->create(volumeRecord);
        linkFile(volume, QStringLiteral("volume"), {QStringLiteral("first page"), QStringLiteral("the fatigue page"), QStringLiteral("last page")});

        ArticleDraft first;
        first.title = QStringLiteral("Opening Article");
        first.firstPage = 1;
        first.lastPage = 1;
        ArticleDraft second;
        second.title = QStringLiteral("Middle Article");
        second.firstPage = 2;
        second.lastPage = 2;
        const QList<qint64> articles = createArticles(*m_db, volume, {first, second}, false);

        const QList<qint64> ids = m_query->ids(search(QStringLiteral("fatigue"), true), {});
        QCOMPARE(ids.size(), 2);
        QVERIFY(ids.contains(volume));
        QVERIFY(ids.contains(articles.at(1)));

        // And its matches are only its own.
        QCOMPARE(m_index->matchesFor(articles.at(1), parseSearch(QStringLiteral("page")).contentsMatch).size(), 1);
        QCOMPARE(m_index->matchesFor(articles.at(1), parseSearch(QStringLiteral("page")).contentsMatch).first().page, 1);
        QCOMPARE(m_index->matchesFor(volume, parseSearch(QStringLiteral("page")).contentsMatch).size(), 3);
    }

    void matchesSayWhichPageAndShowTheWords()
    {
        const qint64 id = addIndexed(QStringLiteral("One"), {QStringLiteral("nothing"), QStringLiteral("before the\nfatigue   after"), QStringLiteral("fatigue again")});
        const QList<ContentMatch> matches = m_index->matchesFor(id, parseSearch(QStringLiteral("fatigue")).contentsMatch);
        QCOMPARE(matches.size(), 2);
        QCOMPARE(matches.at(0).page, 1);
        QCOMPARE(matches.at(1).page, 2);
        QCOMPARE(matches.at(0).snippet, QStringLiteral("before the fatigue after"));
        QVERIFY(m_index->matchesFor(id, QString()).isEmpty());
        QCOMPARE(m_index->matchesFor(id, parseSearch(QStringLiteral("fatigue")).contentsMatch, 1).size(), 1);
    }

    void searchTextCantBreakOutOfItsQuotes_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("quote") << QStringLiteral("\"unfinished");
        QTest::newRow("operators") << QStringLiteral("NOT AND OR");
        QTest::newRow("column") << QStringLiteral("text:fatigue");
        QTest::newRow("syntax") << QStringLiteral("* ( ) ^ : -");
        QTest::newRow("sql") << QStringLiteral("'; DROP TABLE page_text; --");
    }

    void searchTextCantBreakOutOfItsQuotes()
    {
        QFETCH(QString, text);
        addIndexed(QStringLiteral("One"), {QStringLiteral("some words")});
        m_query->count(search(text, true));
        QCOMPARE(m_index->indexedCount(), qint64(1));
    }

    void theSameContentsAreOnlyIndexedOnce()
    {
        const qint64 first = addIndexed(QStringLiteral("One"), {QStringLiteral("shared words")}, QString(), PublicationType::Other, 0, QStringLiteral("samehash"));
        QVERIFY(!m_index->nextPending(0).has_value());

        Publication other;
        other.title = QStringLiteral("Two");
        const qint64 second = m_publications->create(other);
        FileRecord copy;
        copy.rootId = rootForTests();
        copy.relativePath = QStringLiteral("copy.pdf");
        copy.format = QStringLiteral("pdf");
        copy.sha256 = QStringLiteral("samehash");
        m_files->link(second, m_files->upsert(copy), true);

        QVERIFY(!m_index->nextPending(0).has_value());
        const QList<qint64> ids = m_query->ids(search(QStringLiteral("shared"), true), {});
        QCOMPARE(ids.size(), 2);
        QVERIFY(ids.contains(first));
        QVERIFY(ids.contains(second));
    }

    void whatsWaitingLeavesOutWhatCantBeRead()
    {
        const qint64 removedRoot = m_files->addRoot(QStringLiteral("Gone"), m_dir->filePath(QStringLiteral("gone")));
        const auto fileIn = [&](qint64 root, const QString &name, const QString &sha, FileStatus status) {
            Publication p;
            p.title = name;
            const qint64 id = m_publications->create(p);
            FileRecord file;
            file.rootId = root;
            file.relativePath = name;
            file.format = QStringLiteral("pdf");
            file.sha256 = sha;
            file.status = status;
            m_files->link(id, m_files->upsert(file), true);
        };
        fileIn(rootForTests(), QStringLiteral("ready.pdf"), QStringLiteral("hash1"), FileStatus::Ok);
        fileIn(rootForTests(), QStringLiteral("nohash.pdf"), QString(), FileStatus::Ok);
        fileIn(rootForTests(), QStringLiteral("bad.pdf"), QStringLiteral("hash2"), FileStatus::Unreadable);
        fileIn(rootForTests(), QStringLiteral("gone.pdf"), QStringLiteral("hash3"), FileStatus::Missing);
        fileIn(removedRoot, QStringLiteral("removed.pdf"), QStringLiteral("hash4"), FileStatus::Ok);
        m_files->hideRoot(removedRoot);

        QCOMPARE(m_index->pendingCount(), qint64(1));
        QCOMPARE(m_index->nextPending(0)->sha256, QStringLiteral("hash1"));
    }

    void textNoFileHasAnyMoreIsForgotten()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("R"), m_dir->filePath(QStringLiteral("r")));
        addIndexed(QStringLiteral("One"), {QStringLiteral("forgettable words")}, QString(), PublicationType::Other, root);
        QCOMPARE(m_index->indexedCount(), qint64(1));
        QCOMPARE(m_index->removeOrphans(), 0);
        m_files->removeRoot(root);
        QCOMPARE(m_index->removeOrphans(), 1);
        QCOMPARE(m_index->indexedCount(), qint64(0));
        auto pages = m_db->prepare("SELECT count(*) FROM fulltext.page_text");
        pages->step();
        QCOMPARE(pages->int64At(0), qint64(0));
    }

    void anInterruptedDocumentStartsAgain()
    {
        const qint64 id = m_index->beginDocument(QStringLiteral("hash"));
        m_index->addPages(id, 0, {QStringLiteral("first attempt")});
        QCOMPARE(m_index->beginDocument(QStringLiteral("hash")), id);
        auto pages = m_db->prepare("SELECT count(*) FROM fulltext.page_text");
        pages->step();
        QCOMPARE(pages->int64At(0), qint64(0));
        QCOMPARE(m_index->indexedCount(), qint64(0));
    }

    // ---- Real files, through the real import and worker ----

    void everyFormatIsIndexed_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("word");
        QTest::addColumn<int>("page");
        QTest::newRow("pdf") << QStringLiteral("reader_doc.pdf") << QStringLiteral("findme") << 1;
        QTest::newRow("djvu") << QStringLiteral("reader_doc.djvu") << QStringLiteral("findme") << 0;
    }

    void everyFormatIsIndexed()
    {
        QFETCH(QString, file);
        QFETCH(QString, word);
        QFETCH(int, page);
        importSamples({file});
        const QByteArray before = folderHash(m_docs);
        const import::FullTextReport report = runIndexer();
        QCOMPARE(report.documents, 1);
        QCOMPARE(report.withProblems, 0);
        QCOMPARE(folderHash(m_docs), before); // Files are only read.

        const QList<qint64> ids = m_query->ids(search(word, true), {});
        QCOMPARE(ids.size(), 1);
        const QList<ContentMatch> matches = m_index->matchesFor(ids.first(), parseSearch(word).contentsMatch);
        QVERIFY(!matches.isEmpty());
        QCOMPARE(matches.first().page, page);
        QVERIFY(matches.first().snippet.contains(word, Qt::CaseInsensitive));
    }

    void reflowableAndKindleFilesAreIndexed()
    {
        importSamples({QStringLiteral("reader_nav.epub"), QStringLiteral("kf8_frankenstein.azw3"), QStringLiteral("sample.fb2"), QStringLiteral("sample.xps")});
        const import::FullTextReport report = runIndexer();
        QCOMPARE(report.documents, 4);
        QCOMPARE(report.withProblems, 0);
        QVERIFY(report.pages > 4);
        QVERIFY(m_query->count(search(QStringLiteral("Frankenstein"), true)) >= 1);
    }

    void aSecondRunHasNothingToDo()
    {
        importSamples({QStringLiteral("reader_doc.pdf")});
        QCOMPARE(runIndexer().documents, 1);
        QCOMPARE(runIndexer().documents, 0);
    }

    void scannedFilesAreMarkedAsHavingNoText()
    {
        importSamples({QStringLiteral("scanned_no_text.pdf"), QStringLiteral("article.pdf")});
        runIndexer();
        PublicationFilter noText;
        noText.noText = true;
        QCOMPARE(m_query->count(noText), qint64(1));
        QCOMPARE(m_index->indexedCount(), qint64(2));
    }

    void aBadFileDoesntStopTheOthers()
    {
        importSamples({QStringLiteral("truncated.pdf"), QStringLiteral("reader_doc.pdf")});
        // Arch's MuPDF crashes on the truncated file, others just read less of it. Either way, the rest go on.
        const import::FullTextReport report = runIndexer();
        QVERIFY(report.documents >= 1);
        QVERIFY(m_query->count(search(QStringLiteral("findme"), true)) >= 1);
    }

    void aCrashOrHangIsRecordedAndNotRetried()
    {
        QFile::copy(sample(QStringLiteral("reader_doc.pdf")), QDir(m_docs).filePath(QStringLiteral("__crash__.pdf")));
        QFile::copy(sample(QStringLiteral("reader_doc.pdf")), QDir(m_docs).filePath(QStringLiteral("__hang__.pdf")));
        QFile::copy(sample(QStringLiteral("article.pdf")), QDir(m_docs).filePath(QStringLiteral("fine.pdf")));
        const qint64 root = m_files->addRoot(QStringLiteral("Docs"), m_docs);
        addFileRecord(root, QStringLiteral("__crash__.pdf"), QStringLiteral("crashhash"));
        addFileRecord(root, QStringLiteral("__hang__.pdf"), QStringLiteral("hanghash"));
        addFileRecord(root, QStringLiteral("fine.pdf"), QStringLiteral("finehash"));

        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        import::FullTextOptions options = indexerOptions();
        options.timeoutMs = 3000;
        const import::FullTextReport report = import::FullTextIndexer(*m_db, options).run();
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");

        QCOMPARE(report.documents, 3);
        QCOMPARE(report.withProblems, 2);
        QVERIFY(!m_index->problemFor(QStringLiteral("crashhash")).isEmpty());
        QVERIFY(!m_index->problemFor(QStringLiteral("hanghash")).isEmpty());
        QVERIFY(m_index->problemFor(QStringLiteral("finehash")).isEmpty());
        QCOMPARE(m_index->pendingCount(), qint64(0));
    }

    void aMissingFileIsLeftForLater()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("Docs"), m_docs);
        addFileRecord(root, QStringLiteral("not-there.pdf"), QStringLiteral("somehash"));
        QCOMPARE(runIndexer().documents, 0);
        QCOMPARE(m_index->pendingCount(), qint64(1));
    }

    void stoppingLeavesTheRestWaiting()
    {
        importSamples({QStringLiteral("reader_doc.pdf"), QStringLiteral("article.pdf"), QStringLiteral("thesis.pdf")});
        import::FullTextOptions options = indexerOptions();
        int calls = 0;
        options.cancelled = [&calls] { return ++calls > 2; };
        const import::FullTextReport report = import::FullTextIndexer(*m_db, options).run();
        QVERIFY(report.stopped);
        QVERIFY(m_index->pendingCount() > 0);

        QCOMPARE(runIndexer().withProblems, 0);
        QCOMPARE(m_index->pendingCount(), qint64(0));
        QCOMPARE(m_index->indexedCount(), qint64(3));
    }

    // ---- The window ----

    void aPhraseSearchAcrossTheLibraryOpensTheRightPage()
    {
        QFile::copy(sample(QStringLiteral("reader_doc.pdf")), QDir(m_docs).filePath(QStringLiteral("reader_doc.pdf")));
        QFile::copy(sample(QStringLiteral("article.pdf")), QDir(m_docs).filePath(QStringLiteral("article.pdf")));
        openWindow();
        m_window->addFolder(m_docs);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), qint64(2), kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        // Indexing starts by itself once the import has finished.
        FullTextIndex index(m_session->database());
        QTRY_COMPARE_WITH_TIMEOUT(index.indexedCount(), qint64(2), kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->indexThread()->isBusy(), kLongWaitMs);

        // Off by default.
        QVERIFY(!m_window->searchInsideBox()->isChecked());
        m_window->searchBox()->setText(QStringLiteral("\"word findme\""));
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->rowCount(), 0, kLongWaitMs);

        m_window->searchInsideBox()->setChecked(true);
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->rowCount(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->model()->idAt(0).has_value(), kLongWaitMs);
        const qint64 id = m_window->model()->idAt(0).value();

        // The details say where it was found.
        DetailsPanel *details = m_window->details();
        details->showPublication(id);
        QVERIFY(details->matchesSection()->isVisibleTo(details));
        QListWidget *list = details->matchList();
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("2")));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("findme")));

        // Choosing it opens that page, with the words being searched for.
        emit list->itemActivated(list->item(0));
        auto *reader = qobject_cast<ReaderTab *>(m_window->documentTabs()->currentWidget());
        QVERIFY(reader != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->currentPage(), 1, kLongWaitMs);
        QCOMPARE(reader->searchBox()->text(), QStringLiteral("word findme"));

        // It's remembered for next time, and not asked for again in the details when turned off.
        m_window->documentTabs()->setCurrentIndex(0);
        m_window->searchInsideBox()->setChecked(false);
        QVERIFY(!details->matchesSection()->isVisibleTo(details));
        QCOMPARE(m_session->settings().value(QStringLiteral("library.search_contents")).toBool(), false);
    }

    void indexingCanBeTurnedOff()
    {
        QFile::copy(sample(QStringLiteral("article.pdf")), QDir(m_docs).filePath(QStringLiteral("article.pdf")));
        openWindow(false);
        m_window->addFolder(m_docs);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), qint64(1), kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        QTest::qWait(800);

        FullTextIndex index(m_session->database());
        QCOMPARE(index.indexedCount(), qint64(0));
        QVERIFY(!m_window->indexThread()->isBusy());

        m_window->indexContentsAction()->setChecked(true);
        QTRY_COMPARE_WITH_TIMEOUT(index.indexedCount(), qint64(1), kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->indexThread()->isBusy(), kLongWaitMs);
    }

private:
    PublicationFilter search(const QString &text, bool contents)
    {
        PublicationFilter filter;
        filter.searchText = text;
        filter.searchContents = contents;
        return filter;
    }

    qint64 rootForTests()
    {
        if (m_root == 0 || !m_files->root(m_root))
            m_root = m_files->addRoot(QStringLiteral("Tests"), m_dir->filePath(QStringLiteral("tests")));
        return m_root;
    }

    void linkFile(qint64 publication, const QString &sha, const QStringList &pages, qint64 root = 0)
    {
        FileRecord file;
        file.rootId = root != 0 ? root : rootForTests();
        file.relativePath = QStringLiteral("file%1.pdf").arg(++m_fileNumber);
        file.format = QStringLiteral("pdf");
        file.sha256 = sha;
        m_files->link(publication, m_files->upsert(file), true);
        const qint64 document = m_index->beginDocument(sha);
        m_index->addPages(document, 0, pages);
        m_index->finishDocument(document, int(pages.size()), QString());
    }

    qint64 addIndexed(const QString &title, const QStringList &pages, const QString &author = QString(),
                      PublicationType type = PublicationType::Other, qint64 root = 0, const QString &sha = QString())
    {
        Publication p;
        p.title = title;
        p.type = type;
        if (!author.isEmpty())
            p.authors = {{author, QString()}};
        const qint64 id = m_publications->create(p);
        linkFile(id, sha.isEmpty() ? QStringLiteral("hash%1").arg(id) : sha, pages, root);
        return id;
    }

    void addFileRecord(qint64 root, const QString &name, const QString &sha)
    {
        Publication p;
        p.title = name;
        const qint64 id = m_publications->create(p);
        FileRecord file;
        file.rootId = root;
        file.relativePath = name;
        file.format = QStringLiteral("pdf");
        file.sha256 = sha;
        m_files->link(id, m_files->upsert(file), true);
    }

    import::FullTextOptions indexerOptions() const
    {
        import::FullTextOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        return options;
    }

    import::FullTextReport runIndexer()
    {
        return import::FullTextIndexer(*m_db, indexerOptions()).run();
    }

    void importSamples(const QStringList &names)
    {
        for (const QString &name : names)
            QFile::copy(sample(name), QDir(m_docs).filePath(name));
        const qint64 root = m_files->addRoot(QStringLiteral("Docs"), m_docs);
        import::ImportOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        import::ImportService service(*m_db, options);
        service.scan(root);
        service.processQueue();
    }

    void openWindow(bool indexing = true)
    {
        // The session has a connection of its own, in its own folder.
        m_boxWatcher = std::make_unique<QTimer>();
        connect(m_boxWatcher.get(), &QTimer::timeout, this, [this] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                m_unexpectedBoxes << box->text();
                box->close();
            }
        });
        m_boxWatcher->start(100);
        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("window/library.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("window/cache"));
        paths.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        paths.settleMs = 0;
        m_session = std::make_unique<LibrarySession>(paths);
        m_session->settings().setValue(QStringLiteral("library.rescan_on_startup"), false);
        m_session->settings().setValue(QStringLiteral("library.index_contents"), indexing);
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->show();
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_docs;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<PublicationRepository> m_publications;
    std::unique_ptr<FileRepository> m_files;
    std::unique_ptr<FullTextIndex> m_index;
    std::unique_ptr<LibraryQuery> m_query;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
    qint64 m_root = 0;
    int m_fileNumber = 0;
};

QTEST_MAIN(FullTextTest)
#include "test_fulltext.moc"
