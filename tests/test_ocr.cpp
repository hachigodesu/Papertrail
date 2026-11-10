// Recognising the words on scanned pages: the word layer that answers text questions from them, the worker that
// recognises and keeps them, and the reader and full-text index that use them.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "core/repo/FullTextIndex.h"
#include "engine/WordLayer.h"
#include "import/FullTextIndexer.h"
#include "import/ImportService.h"
#include "import/TextRecognition.h"
#include "import/WorkerClient.h"
#include "ui/DocumentClient.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail;

namespace {

constexpr int kWaitMs = 180000;

QString sample(const QString &name)
{
    return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name);
}

QString tessdata()
{
    return QStringLiteral(PAPERTRAIL_TEST_TESSDATA);
}

engine::TextWord word(double x, double y, const QString &text, bool endsLine = false)
{
    return engine::TextWord{QRectF(x, y, 10.0 * text.size(), 12), text, endsLine};
}

// How many of these words were recognised, ignoring case. Recognition differs a little between Tesseract's versions,
// so tests ask for most of them rather than all.
int found(const QString &text, const QStringList &words)
{
    int count = 0;
    for (const QString &w : words)
        count += text.contains(w, Qt::CaseInsensitive) ? 1 : 0;
    return count;
}

const QStringList kScannedWords = {QStringLiteral("Bending"), QStringLiteral("beam"), QStringLiteral("load"),
                                   QStringLiteral("deflection"), QStringLiteral("length"), QStringLiteral("testing")};

} // namespace

class OcrTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // For the page that crashes. The app itself never sets this.
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
    }

    void init() { m_dir = std::make_unique<QTemporaryDir>(); }
    void cleanup() { m_dir.reset(); }

    // ---- The word layer

    void wordsAnswerLikeAPagesText()
    {
        const QList<engine::TextWord> words = {word(10, 10, QStringLiteral("Made-up"), false),
                                               word(90, 10, QStringLiteral("beams"), true),
                                               word(10, 30, QStringLiteral("bend"), false),
                                               word(60, 30, QStringLiteral("slowly."), true)};
        QCOMPARE(engine::words::pageText(words), QStringLiteral("Made-up beams\nbend slowly.\n"));

        // A phrase runs over a line break.
        QCOMPARE(engine::words::search(words, QStringLiteral("beams bend")).size(), 2);
        QCOMPARE(engine::words::search(words, QStringLiteral("BEND")).size(), 1);
        QVERIFY(engine::words::search(words, QStringLiteral("twist")).isEmpty());

        const QList<engine::TextLine> lines = engine::words::textLines(words);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.first().text, QStringLiteral("Made-up beams"));
        QCOMPARE(lines.first().chars.size(), lines.first().text.size());

        const engine::TextSelection selection = engine::words::selectText(words, QPointF(95, 12), QPointF(15, 32));
        QCOMPARE(selection.text, QStringLiteral("beams\nbend"));
        QCOMPARE(selection.rects.size(), 2);
    }

    // ---- Recognising, through the worker

    void aScannedPageIsRecognisedAndKeptButNotOneWithText()
    {
        requireOcr();
        const QString folder = m_dir->filePath(QStringLiteral("ocr"));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));

        // Before, the scan has no text.
        const auto before = worker.pageTexts(sample(QStringLiteral("scanned_text.pdf")), QString(), 0, 5, kWaitMs, folder);
        QCOMPARE(before.failure, import::WorkerFailure::None);
        QCOMPARE(before.pageCount, 2);
        QCOMPARE(found(before.pages.at(0), kScannedWords), 0);

        // Only the first page is a scan. The second has text of its own.
        const auto recognised = worker.recognise(sample(QStringLiteral("scanned_text.pdf")), QString(), folder, tessdata(),
                                                 QStringLiteral("eng"), 0, 5, false, kWaitMs);
        QVERIFY2(recognised.failure == import::WorkerFailure::None, qPrintable(recognised.error));
        QCOMPARE(recognised.pages, 1);
        QVERIFY(recognised.words > 10);
        QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("p0.json"))));
        QVERIFY(!QFile::exists(QDir(folder).filePath(QStringLiteral("p1.json"))));

        // Its text is the recognised words now, and said to be.
        const auto after = worker.pageTexts(sample(QStringLiteral("scanned_text.pdf")), QString(), 0, 5, kWaitMs, folder);
        QVERIFY2(found(after.pages.at(0), kScannedWords) >= 4, qPrintable(after.pages.at(0)));
        QCOMPARE(after.recognised, QList<int>{0});
        QVERIFY(after.pages.at(1).contains(QStringLiteral("own text"), Qt::CaseInsensitive));
        QVERIFY(!after.recognised.contains(1));

        // Once is enough, unless asked again.
        const auto again = worker.recognise(sample(QStringLiteral("scanned_text.pdf")), QString(), folder, tessdata(),
                                            QStringLiteral("eng"), 0, 5, false, kWaitMs);
        QCOMPARE(again.pages, 0);
        const auto forced = worker.recognise(sample(QStringLiteral("scanned_text.pdf")), QString(), folder, tessdata(),
                                             QStringLiteral("eng"), 0, 5, true, kWaitMs);
        QCOMPARE(forced.pages, 1);
    }

    // A document whose own text is poor is recognised on every page, and its recognised words are used from then on,
    // while what counts as the file's own text is still its own.
    void everyPageCanBeRecognisedInPlaceOfAFilesOwnText()
    {
        requireOcr();
        const QString folder = m_dir->filePath(QStringLiteral("ocr"));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const QString path = sample(QStringLiteral("article.pdf"));

        // Without asking for every page, a page with its own text is left alone.
        const auto skipped = worker.recognise(path, QString(), folder, tessdata(), QStringLiteral("eng"), 0, 5, false, kWaitMs);
        QVERIFY2(skipped.failure == import::WorkerFailure::None, qPrintable(skipped.error));
        QCOMPARE(skipped.pages, 0);
        const auto own = worker.pageTexts(path, QString(), 0, 5, kWaitMs, folder);
        QVERIFY(own.recognised.isEmpty());
        QVERIFY(own.ownLetters >= 40);

        const auto every = worker.recognise(path, QString(), folder, tessdata(), QStringLiteral("eng"), 0, 5, false, kWaitMs,
                                            true);
        QVERIFY2(every.failure == import::WorkerFailure::None, qPrintable(every.error));
        QCOMPARE(every.pages, every.pageCount);
        QVERIFY(every.words > 5);

        // Its text is what was recognised now, even in a worker started afresh, but its own letters are still counted.
        import::WorkerClient fresh(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto after = fresh.pageTexts(path, QString(), 0, 5, kWaitMs, folder);
        QCOMPARE(after.recognised.size(), after.pageCount);
        QCOMPARE(after.ownLetters, own.ownLetters);
        QVERIFY2(found(after.pages.at(0), {QStringLiteral("Fatigue"), QStringLiteral("Steel"), QStringLiteral("Beams"),
                                           QStringLiteral("made-up")})
                     >= 3,
                 qPrintable(after.pages.at(0)));
    }

    void aMissingLanguageSaysSo()
    {
        requireOcr();
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto result = worker.recognise(sample(QStringLiteral("scanned_text.pdf")), QString(),
                                             m_dir->filePath(QStringLiteral("ocr")), tessdata(), QStringLiteral("xxx"), 0, 5,
                                             false, kWaitMs);
        QCOMPARE(result.failure, import::WorkerFailure::Rejected);
        QVERIFY(result.error.contains(QStringLiteral("language")));
    }

    void aPageThatCrashesWhileBeingRecognisedIsSurvived()
    {
        requireOcr();
        const QString copy = m_dir->filePath(QStringLiteral("scan__crashpage0__.pdf"));
        QVERIFY(QFile::copy(sample(QStringLiteral("scanned_text.pdf")), copy));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto crashed = worker.recognise(copy, QString(), m_dir->filePath(QStringLiteral("ocr")), tessdata(),
                                              QStringLiteral("eng"), 0, 5, false, kWaitMs);
        QCOMPARE(crashed.failure, import::WorkerFailure::Crashed);
        // The worker carries on with the next request.
        const auto hashed = worker.hash(copy, kWaitMs);
        QCOMPARE(hashed.failure, import::WorkerFailure::None);
    }

    // ---- In the library

    void aScannedDocumentIsRecognisedInTheBackgroundAndFoundBySearching()
    {
        requireOcr();
        using namespace core;
        const QString docs = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(docs);
        QVERIFY(QFile::copy(sample(QStringLiteral("scanned_text.pdf")), QDir(docs).filePath(QStringLiteral("scan.pdf"))));
        const QString database = m_dir->filePath(QStringLiteral("library.db"));
        Database db(database);
        migrateLibrary(db);
        attachFullText(db, fullTextPathFor(database));
        const QString cache = m_dir->filePath(QStringLiteral("cache"));
        {
            FileRepository files(db);
            const qint64 root = files.addRoot(QStringLiteral("Docs"), docs);
            import::ImportOptions options;
            options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
            options.cacheDirectory = cache;
            import::ImportService service(db, options);
            service.scan(root);
            service.processQueue();
        }
        import::FullTextOptions indexing;
        indexing.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        indexing.cacheDirectory = cache;
        import::FullTextIndexer(db, indexing).run();
        auto noText = db.prepare("SELECT has_text_layer FROM file");
        QVERIFY(noText->step());
        QCOMPARE(noText->intAt(0), 0); // Only one page has text of its own, and too little to count.

        PublicationFilter filter;
        filter.searchText = QStringLiteral("deflection");
        filter.searchContents = true;
        QCOMPARE(LibraryQuery(db).count(filter), qint64(0));

        import::RecognitionOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = cache;
        options.tessdata = tessdata();
        options.languages = QStringLiteral("eng");
        import::TextRecognition recognition(db, options);
        QCOMPARE(recognition.waitingCount(), qint64(1));
        const import::RecognitionReport report = recognition.run();
        QVERIFY2(report.problem.isEmpty(), qPrintable(report.problem));
        QCOMPARE(report.documents, 1);
        QCOMPARE(report.pages, 1);
        QCOMPARE(recognition.waitingCount(), qint64(0));

        // Read into the index again, its words are found, but the file still has no text of its own.
        import::FullTextIndexer(db, indexing).run();
        QCOMPARE(LibraryQuery(db).count(filter), qint64(1));
        noText->reset();
        QVERIFY(noText->step());
        QCOMPARE(noText->intAt(0), 0);

        // Done once, unless the languages change.
        QCOMPARE(import::TextRecognition(db, options).run().documents, 0);
        options.languages = QStringLiteral("eng+osd");
        QCOMPARE(import::TextRecognition(db, options).waitingCount(), qint64(1));
    }

    void theScannedPagesOfAMixedDocumentAreRecognisedInTheBackground()
    {
        requireOcr();
        using namespace core;
        const QString docs = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(docs);
        QVERIFY(QFile::copy(sample(QStringLiteral("scanned_mixed.pdf")), QDir(docs).filePath(QStringLiteral("mixed.pdf"))));
        const QString database = m_dir->filePath(QStringLiteral("library.db"));
        Database db(database);
        migrateLibrary(db);
        attachFullText(db, fullTextPathFor(database));
        const QString cache = m_dir->filePath(QStringLiteral("cache"));
        {
            FileRepository files(db);
            const qint64 root = files.addRoot(QStringLiteral("Docs"), docs);
            import::ImportOptions options;
            options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
            options.cacheDirectory = cache;
            import::ImportService service(db, options);
            service.scan(root);
            service.processQueue();
        }
        import::FullTextOptions indexing;
        indexing.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        indexing.cacheDirectory = cache;
        import::FullTextIndexer(db, indexing).run();
        auto file = db.prepare("SELECT has_text_layer, sha256 FROM file");
        QVERIFY(file->step());
        QCOMPARE(file->intAt(0), 1); // Three pages of its own text.
        const QString sha256 = file->textAt(1);
        file.reset();

        PublicationFilter filter;
        filter.searchText = QStringLiteral("deflection");
        filter.searchContents = true;
        QCOMPARE(LibraryQuery(db).count(filter), qint64(0)); // Only on the scanned page.

        import::RecognitionOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = cache;
        options.tessdata = tessdata();
        options.languages = QStringLiteral("eng");
        import::TextRecognition recognition(db, options);
        const import::RecognitionReport report = recognition.run();
        QVERIFY2(report.problem.isEmpty(), qPrintable(report.problem));
        QCOMPARE(report.documents, 1);
        QCOMPARE(report.pages, 1);
        QCOMPARE(recognition.waitingCount(), qint64(0));

        // Only the scanned page was looked at, and only it has recognised words.
        auto noted = db.prepare("SELECT pages FROM fulltext.mixed_pages WHERE sha256 = ?");
        noted->bind(1, sha256);
        QVERIFY(noted->step());
        QCOMPARE(noted->textAt(0), QStringLiteral("3"));
        noted.reset();
        const QDir words(import::recognisedFolderFor(cache, sha256));
        QCOMPARE(words.entryList({QStringLiteral("p*.json")}, QDir::Files), QStringList{QStringLiteral("p3.json")});

        // Read into the index again, its words are found, and the file still has its own text.
        import::FullTextIndexer(db, indexing).run();
        QCOMPARE(LibraryQuery(db).count(filter), qint64(1));
        file = db.prepare("SELECT has_text_layer FROM file");
        QVERIFY(file->step());
        QCOMPARE(file->intAt(0), 1);
        file.reset();

        // Done once, unless the languages change.
        QCOMPARE(import::TextRecognition(db, options).run().documents, 0);
        options.languages = QStringLiteral("eng+osd");
        QCOMPARE(import::TextRecognition(db, options).waitingCount(), qint64(1));
    }

    void aDocumentIndexedBeforeIsLookedAtToo()
    {
        requireOcr();
        using namespace core;
        // As a library indexed by 1.3.0 is: read into the index, with nothing noted about its pages.
        const QString docs = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(docs);
        QVERIFY(QFile::copy(sample(QStringLiteral("scanned_mixed.pdf")), QDir(docs).filePath(QStringLiteral("mixed.pdf"))));
        QVERIFY(QFile::copy(sample(QStringLiteral("article.pdf")), QDir(docs).filePath(QStringLiteral("article.pdf"))));
        const QString database = m_dir->filePath(QStringLiteral("library.db"));
        Database db(database);
        migrateLibrary(db);
        attachFullText(db, fullTextPathFor(database));
        const QString cache = m_dir->filePath(QStringLiteral("cache"));
        {
            FileRepository files(db);
            const qint64 root = files.addRoot(QStringLiteral("Docs"), docs);
            import::ImportOptions options;
            options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
            options.cacheDirectory = cache;
            import::ImportService service(db, options);
            service.scan(root);
            service.processQueue();
        }
        import::FullTextOptions indexing;
        indexing.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        indexing.cacheDirectory = cache;
        import::FullTextIndexer(db, indexing).run();
        auto none = db.prepare("SELECT count(*) FROM fulltext.mixed_pages");
        QVERIFY(none->step());
        QCOMPARE(none->int64At(0), qint64(0));
        none.reset();

        import::RecognitionOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = cache;
        options.tessdata = tessdata();
        options.languages = QStringLiteral("eng");
        const import::RecognitionReport report = import::TextRecognition(db, options).run();
        QVERIFY2(report.problem.isEmpty(), qPrintable(report.problem));
        QCOMPARE(report.pages, 1);
        // Both were looked at once, and the article, all its own text, has nothing to recognise.
        auto noted = db.prepare("SELECT count(*), sum(pages = '') FROM fulltext.mixed_pages");
        QVERIFY(noted->step());
        QCOMPARE(noted->int64At(0), qint64(2));
        QCOMPARE(noted->int64At(1), qint64(1));
    }

    void aDocumentWithItsOwnTextCanBeRecognisedInFull()
    {
        requireOcr();
        using namespace core;
        const QString docs = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(docs);
        QVERIFY(QFile::copy(sample(QStringLiteral("article.pdf")), QDir(docs).filePath(QStringLiteral("article.pdf"))));
        const QString database = m_dir->filePath(QStringLiteral("library.db"));
        Database db(database);
        migrateLibrary(db);
        attachFullText(db, fullTextPathFor(database));
        const QString cache = m_dir->filePath(QStringLiteral("cache"));
        {
            FileRepository files(db);
            const qint64 root = files.addRoot(QStringLiteral("Docs"), docs);
            import::ImportOptions options;
            options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
            options.cacheDirectory = cache;
            import::ImportService service(db, options);
            service.scan(root);
            service.processQueue();
        }
        import::FullTextOptions indexing;
        indexing.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        indexing.cacheDirectory = cache;
        import::FullTextIndexer(db, indexing).run();
        auto file = db.prepare("SELECT sha256, has_text_layer FROM file");
        QVERIFY(file->step());
        const QString sha256 = file->textAt(0);
        QCOMPARE(file->intAt(1), 1);

        import::RecognitionOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = cache;
        options.tessdata = tessdata();
        options.languages = QStringLiteral("eng");
        QCOMPARE(import::TextRecognition(db, options).waitingCount(), qint64(0));

        QVERIFY(!import::TextRecognition::isWanted(db, sha256));
        import::TextRecognition::want(db, sha256);
        QVERIFY(import::TextRecognition::isWanted(db, sha256));
        import::TextRecognition recognition(db, options);
        QCOMPARE(recognition.waitingCount(), qint64(1));
        const import::RecognitionReport report = recognition.run();
        QVERIFY2(report.problem.isEmpty(), qPrintable(report.problem));
        QCOMPARE(report.documents, 1);
        QCOMPARE(report.pages, 1);
        QCOMPARE(recognition.waitingCount(), qint64(0));

        // Read again, it's still found, and the file still has text of its own.
        import::FullTextIndexer(db, indexing).run();
        PublicationFilter filter;
        filter.searchText = QStringLiteral("fatigue");
        filter.searchContents = true;
        QCOMPARE(LibraryQuery(db).count(filter), qint64(1));
        file->reset();
        QVERIFY(file->step());
        QCOMPARE(file->intAt(1), 1);

        // Asked again, it starts over, keeping the pages it has.
        import::TextRecognition::want(db, sha256);
        QCOMPARE(recognition.waitingCount(), qint64(1));
        QCOMPARE(recognition.run().pages, 0);
    }

    // ---- In the reader

    void theReaderRecognisesAScannedPageAndSearchesIt()
    {
        requireOcr();
        ui::DocumentClient client(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        QSignalSpy opened(&client, &ui::DocumentClient::opened);
        QSignalSpy recognised(&client, &ui::DocumentClient::pageRecognised);
        QSignalSpy hits(&client, &ui::DocumentClient::searchHits);
        QSignalSpy finished(&client, &ui::DocumentClient::searchFinished);
        client.setRecognition(m_dir->filePath(QStringLiteral("ocr")), tessdata(), QStringLiteral("eng"));
        client.open(sample(QStringLiteral("scanned_text.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.startSearch(QStringLiteral("deflection"), 0);
        QVERIFY(finished.wait(kWaitMs));
        QVERIFY(hits.isEmpty()); // Not yet.

        client.requestRecognition(1); // Has text of its own, so there's nothing to do.
        QVERIFY(recognised.wait(kWaitMs));
        QCOMPARE(recognised.last().at(1).toInt(), 0);
        client.requestRecognition(0);
        QTRY_COMPARE_WITH_TIMEOUT(recognised.size(), 2, kWaitMs);
        QVERIFY(recognised.last().at(1).toInt() > 10);

        // Searched like any text now, and found where it is on the page.
        finished.clear();
        client.startSearch(QStringLiteral("deflection"), 0);
        QVERIFY(finished.wait(kWaitMs));
        QVERIFY(!hits.isEmpty());
        QCOMPARE(hits.first().at(0).toInt(), 0);
        const QRectF hit = hits.first().at(1).value<QList<QRectF>>().first();
        QVERIFY(hit.top() > 50 && hit.top() < 400);
    }

    void theReaderRecognisesAPageWithTextWhenAskedForEveryPage()
    {
        requireOcr();
        const QString folder = m_dir->filePath(QStringLiteral("ocr"));
        ui::DocumentClient client(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        QSignalSpy opened(&client, &ui::DocumentClient::opened);
        QSignalSpy recognised(&client, &ui::DocumentClient::pageRecognised);
        client.setRecognition(folder, tessdata(), QStringLiteral("eng"));
        client.open(sample(QStringLiteral("article.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.requestRecognition(0);
        QVERIFY(recognised.wait(kWaitMs));
        QCOMPARE(recognised.last().at(1).toInt(), 0);
        client.requestRecognition(0, true);
        QTRY_COMPARE_WITH_TIMEOUT(recognised.size(), 2, kWaitMs);
        QVERIFY(recognised.last().at(1).toInt() > 5);
        QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("every-page"))));

        // Reading the words again changes nothing the reader can't carry on from.
        QSignalSpy hits(&client, &ui::DocumentClient::searchHits);
        QSignalSpy finished(&client, &ui::DocumentClient::searchFinished);
        client.rereadWords();
        client.startSearch(QStringLiteral("fatigue"), 0);
        QVERIFY(finished.wait(kWaitMs));
        QVERIFY(!hits.isEmpty());
    }

    // Writes the sample above, when asked: a page that's only a picture of made-up text, as a scan is, and a page with
    // a few words of its own, too few for the file to count as having text. Run once, with a real screen's fonts rather than the offscreen platform's.
    void writeScannedSampleWhenAsked()
    {
        const QString path = qEnvironmentVariable("PAPERTRAIL_WRITE_SCANNED_SAMPLE");
        if (path.isEmpty())
            QSKIP("Only when PAPERTRAIL_WRITE_SCANNED_SAMPLE is set");
        constexpr int kDpi = 200;
        QImage scan(int(8.27 * kDpi), int(11.69 * kDpi), QImage::Format_Grayscale8);
        scan.fill(255);
        {
            QPainter painter(&scan);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.setPen(Qt::black);
            QFont heading(QStringLiteral("Arial"));
            heading.setPixelSize(64);
            heading.setBold(true);
            painter.setFont(heading);
            painter.drawText(QPoint(160, 300), QStringLiteral("Bending Made-Up Beams"));
            QFont body(QStringLiteral("Arial"));
            body.setPixelSize(40);
            painter.setFont(body);
            const QStringList lines = {QStringLiteral("A made-up beam bends under a made-up load."),
                                       QStringLiteral("The deflection grows with the cube of its length."),
                                       QStringLiteral("Nothing here is real, and it is only for testing.")};
            for (int i = 0; i < lines.size(); ++i)
                painter.drawText(QPoint(160, 420 + i * 64), lines.at(i));
        }
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(kDpi);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setCreator(QStringLiteral("Papertrail tests"));
        QPainter pdf(&writer);
        pdf.drawImage(QRect(0, 0, writer.width(), writer.height()), scan);
        writer.newPage();
        QFont text(QStringLiteral("Arial"));
        text.setPointSize(12);
        pdf.setFont(text);
        pdf.drawText(QPoint(300, 400), QStringLiteral("Its own text, made up."));
        pdf.end();
    }

    // Writes scanned_mixed.pdf, when asked: three pages of made-up text of its own, then a page that's only a picture of
    // made-up text, as a scan bound into a paper would be. Run once, with a real screen's fonts.
    void writeMixedSampleWhenAsked()
    {
        const QString path = qEnvironmentVariable("PAPERTRAIL_WRITE_MIXED_SAMPLE");
        if (path.isEmpty())
            QSKIP("Only when PAPERTRAIL_WRITE_MIXED_SAMPLE is set");
        constexpr int kDpi = 200;
        QImage scan(int(8.27 * kDpi), int(11.69 * kDpi), QImage::Format_Grayscale8);
        scan.fill(255);
        {
            QPainter painter(&scan);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.setPen(Qt::black);
            QFont heading(QStringLiteral("Arial"));
            heading.setPixelSize(64);
            heading.setBold(true);
            painter.setFont(heading);
            painter.drawText(QPoint(160, 300), QStringLiteral("Appendix: Made-Up Beams"));
            QFont body(QStringLiteral("Arial"));
            body.setPixelSize(40);
            painter.setFont(body);
            const QStringList lines = {QStringLiteral("A made-up beam bends under a made-up load."),
                                       QStringLiteral("The deflection grows with the cube of its length."),
                                       QStringLiteral("Nothing here is real, and it is only for testing.")};
            for (int i = 0; i < lines.size(); ++i)
                painter.drawText(QPoint(160, 420 + i * 64), lines.at(i));
        }
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(kDpi);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setCreator(QStringLiteral("Papertrail tests"));
        QPainter pdf(&writer);
        QFont text(QStringLiteral("Arial"));
        text.setPointSize(12);
        pdf.setFont(text);
        const QStringList paragraphs = {
            QStringLiteral("This made-up paper is about steel, concrete and timber, and none of it is real."),
            QStringLiteral("Its pages have text of their own, as most papers do, so they are searched as they are."),
            QStringLiteral("Only its last page is a picture of words, which has to be recognised before it is searched."),
        };
        for (int page = 0; page < 3; ++page) {
            if (page > 0)
                writer.newPage();
            pdf.drawText(QPoint(300, 400), QStringLiteral("Page %1 of a made-up paper").arg(page + 1));
            for (int i = 0; i < paragraphs.size(); ++i)
                pdf.drawText(QPoint(300, 600 + i * 120), paragraphs.at(i));
        }
        writer.newPage();
        pdf.drawImage(QRect(0, 0, writer.width(), writer.height()), scan);
        pdf.end();
    }

private:
    static void requireOcr()
    {
#ifndef PAPERTRAIL_TEST_OCR
        QSKIP("This build can't recognise text");
#endif
    }

    std::unique_ptr<QTemporaryDir> m_dir;
};

QTEST_MAIN(OcrTest)
#include "test_ocr.moc"
