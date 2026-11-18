// A PDF copy of a book in another format, made through the worker and read back through it as the reader would.

#include "import/WorkerClient.h"
#include "ui/DocumentClient.h"
#include "ui/PdfCopyDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <iostream>
#include <string>

using namespace papertrail;

namespace {

constexpr int kTimeoutMs = 120000;
constexpr int kWaitMs = 30000;
constexpr const char *kFakeWorker = "PAPERTRAIL_TEST_FAKE_PROGRESS_WORKER";

QString sample(const QString &name)
{
    return QStringLiteral(PAPERTRAIL_TEST_SAMPLES "/") + name;
}

// Every file in a folder, by name, with its contents' hash.
QByteArray folderHash(const QString &folder)
{
    QStringList names;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        names << it.next();
    names.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &name : std::as_const(names)) {
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        hash.addData(QDir(folder).relativeFilePath(name).toUtf8());
        hash.addData(&file);
    }
    return hash.result();
}

// What a copy looks like, read back through the worker.
struct ReadBack {
    int pages = 0;
    QSizeF firstPage;
    QList<ui::ReaderOutlineItem> outline;
    QList<ui::ReaderLink> links; // On every page.
    QStringList text;            // Page by page.
};

ReadBack readBack(const QString &path)
{
    ReadBack back;
    ui::DocumentClient client(QStringLiteral(PAPERTRAIL_WORKER_PATH));
    QSignalSpy opened(&client, &ui::DocumentClient::opened);
    QSignalSpy sizes(&client, &ui::DocumentClient::sizesReady);
    QSignalSpy outline(&client, &ui::DocumentClient::outlineReady);
    QSignalSpy links(&client, &ui::DocumentClient::linksReady);
    client.open(path, {});
    if (!opened.wait(kWaitMs))
        return back;
    back.pages = opened.first().at(0).toInt();
    if (sizes.isEmpty() && !sizes.wait(kWaitMs))
        return back;
    const QList<QSizeF> measured = sizes.first().at(1).value<QList<QSizeF>>();
    if (!measured.isEmpty())
        back.firstPage = measured.first();
    client.requestOutline();
    if (outline.wait(kWaitMs))
        back.outline = outline.first().at(0).value<QList<ui::ReaderOutlineItem>>();
    for (int page = 0; page < back.pages; ++page)
        client.requestLinks(page);
    QDeadlineTimer deadline(kWaitMs);
    while (links.size() < back.pages && !deadline.hasExpired())
        links.wait(500);
    for (const QList<QVariant> &arguments : std::as_const(links))
        back.links << arguments.at(1).value<QList<ui::ReaderLink>>();

    import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
    back.text = worker.pageTexts(path, QString(), 0, back.pages, kTimeoutMs).pages;
    return back;
}

// Answers like a worker writing a copy: two lines of how it's getting on, a while apart, then the reply.
int fakeWorker()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QJsonValue id = request.value(QStringLiteral("id"));
        for (int done = 1; done <= 2; ++done) {
            const QJsonObject progress{{QStringLiteral("id"), id}, {QStringLiteral("progress"), done}, {QStringLiteral("of"), 2}};
            std::cout << QJsonDocument(progress).toJson(QJsonDocument::Compact).constData() << std::endl;
            QThread::msleep(400);
        }
        const QJsonObject reply{{QStringLiteral("id"), id}, {QStringLiteral("ok"), true}, {QStringLiteral("pages"), 7}};
        std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData() << std::endl;
    }
    return 0;
}

} // namespace

class PdfCopyTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_samplesBefore = folderHash(QStringLiteral(PAPERTRAIL_TEST_SAMPLES));
        QVERIFY(!m_samplesBefore.isEmpty());
    }

    void cleanupTestCase()
    {
        // Nothing here ever changes a book it copies.
        QCOMPARE(folderHash(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)), m_samplesBefore);
    }

    void eachKindOfBookIsCopied_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QSizeF>("page");     // Empty for the book's own.
        QTest::addColumn<QString>("word");    // Somewhere in its text.
        QTest::addColumn<int>("outline");     // Items at least.
        QTest::addColumn<int>("leastPages");
        QTest::newRow("EPUB at A4") << QStringLiteral("book.epub") << QSizeF(595, 842) << QStringLiteral("worked exercises") << 0 << 1;
        QTest::newRow("EPUB with contents, 6 by 9") << QStringLiteral("reader_nav.epub") << QSizeF(432, 648) << QStringLiteral("findmethird")
                                                     << 4 << 6;
        QTest::newRow("EPUB with an NCX, US Letter") << QStringLiteral("reader_ncx.epub") << QSizeF(612, 792)
                                                      << QStringLiteral("findmesecond") << 3 << 4;
        QTest::newRow("FB2") << QStringLiteral("sample.fb2") << QSizeF(420, 595) << QStringLiteral("made-up book") << 0 << 1;
        QTest::newRow("old Kindle") << QStringLiteral("old_format.mobi") << QSizeF(595, 842) << QStringLiteral("Kindle book") << 0 << 1;
        QTest::newRow("KF8 Kindle") << QStringLiteral("kf8_frankenstein.azw3") << QSizeF(595, 842) << QStringLiteral("Frankenstein") << 1
                                    << 50;
        // XPS measures in 96ths of an inch, so its page of 595 by 842 is smaller in points.
        QTest::newRow("XPS") << QStringLiteral("sample.xps") << QSizeF(446.25, 631.5) << QString() << 0 << 1;
        QTest::newRow("CBZ") << QStringLiteral("sample.cbz") << QSizeF() << QString() << 0 << 1;
        QTest::newRow("DjVu photo") << QStringLiteral("scanned.djvu") << QSizeF() << QString() << 0 << 1;
        QTest::newRow("DjVu with text") << QStringLiteral("with_text.djvu") << QSizeF() << QStringLiteral("hidden text layer") << 0 << 1;
        QTest::newRow("DjVu with contents") << QStringLiteral("reader_doc.djvu") << QSizeF(432, 576) << QStringLiteral("exact phrase") << 3
                                            << 3;
        QTest::newRow("bitonal DjVu") << QStringLiteral("bitonal.djvu") << QSizeF(288, 360) << QStringLiteral("pageword") << 1 << 1;
    }

    void eachKindOfBookIsCopied()
    {
        QFETCH(QString, name);
        QFETCH(QSizeF, page);
        QFETCH(QString, word);
        QFETCH(int, outline);
        QFETCH(int, leastPages);
        const bool djvu = name.endsWith(QLatin1StringView(".djvu"));

        import::WorkerClient::PdfCopySettings settings;
        if (!djvu && page.isValid()) {
            settings.pageWidth = page.width();
            settings.pageHeight = page.height();
        }
        const QString out = m_dir.filePath(QFileInfo(name).completeBaseName() + QStringLiteral(".pdf"));
        const QString converted = m_dir.filePath(QFileInfo(name).completeBaseName() + QStringLiteral("-converted.epub"));
        QList<QPair<int, int>> heard;
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto copy = worker.pdfCopy(sample(name), converted, out, settings, kTimeoutMs,
                                         [&](int done, int of) { heard << qMakePair(done, of); });
        QVERIFY2(copy.failure == import::WorkerFailure::None, qPrintable(copy.error));
        QVERIFY(QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QVERIFY(copy.pages >= leastPages);
        QCOMPARE(copy.unreadablePages, 0);
        QVERIFY(!heard.isEmpty());
        QCOMPARE(heard.last(), qMakePair(copy.pages, copy.pages));
        // Only what's used of a font is kept, which for one short page is far less than the whole of it.
        if (name == QLatin1StringView("book.epub"))
            QVERIFY2(QFileInfo(out).size() < 120 * 1024, qPrintable(QString::number(QFileInfo(out).size())));

        const ReadBack back = readBack(out);
        QCOMPARE(back.pages, copy.pages);
        QVERIFY(back.firstPage.isValid());
        if (page.isValid()) {
            QVERIFY2(qAbs(back.firstPage.width() - page.width()) < 1 && qAbs(back.firstPage.height() - page.height()) < 1,
                     qPrintable(QStringLiteral("%1 x %2").arg(back.firstPage.width()).arg(back.firstPage.height())));
        }
        if (!word.isEmpty())
            QVERIFY2(back.text.join(u' ').simplified().contains(word, Qt::CaseInsensitive), qPrintable(back.text.join(u'|').left(300)));
        QVERIFY2(back.outline.size() >= outline, qPrintable(QString::number(back.outline.size())));
        for (const ui::ReaderOutlineItem &item : back.outline)
            QVERIFY(item.page < back.pages);
    }

    void aPageSizeAndTypeSizeChangeTheLayout()
    {
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        import::WorkerClient::PdfCopySettings large;
        large.pageWidth = 420;
        large.pageHeight = 595;
        large.fontSize = 24;
        large.margin = 90;
        import::WorkerClient::PdfCopySettings small;
        small.fontSize = 9;
        small.margin = 36;
        const auto big = worker.pdfCopy(sample(QStringLiteral("reader_nav.epub")), QString(), m_dir.filePath(QStringLiteral("big.pdf")), large,
                                        kTimeoutMs);
        const auto compact = worker.pdfCopy(sample(QStringLiteral("reader_nav.epub")), QString(), m_dir.filePath(QStringLiteral("compact.pdf")),
                                            small, kTimeoutMs);
        QVERIFY2(big.failure == import::WorkerFailure::None, qPrintable(big.error));
        QVERIFY2(compact.failure == import::WorkerFailure::None, qPrintable(compact.error));
        QVERIFY2(big.pages > compact.pages * 2, qPrintable(QStringLiteral("%1 and %2").arg(big.pages).arg(compact.pages)));
    }

    void linksAndContentsAreKept()
    {
        const QString out = m_dir.filePath(QStringLiteral("links.pdf"));
        import::WorkerClient::PdfCopySettings settings;
        settings.pageWidth = 420;
        settings.pageHeight = 595;
        settings.fontSize = 18;
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto copy = worker.pdfCopy(sample(QStringLiteral("pdf_copy_links.epub")), QString(), out, settings, kTimeoutMs);
        QVERIFY2(copy.failure == import::WorkerFailure::None, qPrintable(copy.error));
        const ReadBack back = readBack(out);
        QVERIFY(back.pages >= 2);

        int target = -1;
        for (int page = 0; page < back.text.size(); ++page) {
            if (back.text.at(page).contains(QLatin1StringView("copyfindme")))
                target = page;
        }
        QVERIFY(target > 0);
        bool within = false;
        bool web = false;
        for (const ui::ReaderLink &link : back.links) {
            within = within || link.page == target;
            web = web || link.uri == QLatin1StringView("https://example.org/made-up-book");
            QVERIFY2(!link.uri.startsWith(QLatin1StringView("javascript"), Qt::CaseInsensitive), qPrintable(link.uri));
        }
        QVERIFY(within);
        QVERIFY(web);

        QCOMPARE(back.outline.size(), 3);
        QCOMPARE(back.outline.at(0).title, QStringLiteral("Linked Chapter One"));
        QCOMPARE(back.outline.at(0).depth, 0);
        QCOMPARE(back.outline.at(1).title, QStringLiteral("Linked Chapter Two"));
        QCOMPARE(back.outline.at(2).title, QStringLiteral("The Target Section"));
        QCOMPARE(back.outline.at(2).depth, 1);
        QCOMPARE(back.outline.at(2).page, target);
        QVERIFY(back.outline.at(1).page > back.outline.at(0).page);
    }

    void aDjvusWordsCanBeFoundInAnyScript()
    {
        const QString out = m_dir.filePath(QStringLiteral("greek.pdf"));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto copy = worker.pdfCopy(sample(QStringLiteral("bitonal.djvu")), QString(), out, {}, kTimeoutMs);
        QVERIFY2(copy.failure == import::WorkerFailure::None, qPrintable(copy.error));
        const QString text = readBack(out).text.join(u' ');
        QVERIFY2(text.contains(QStringLiteral("κύμα")), qPrintable(text));
        QVERIFY(text.contains(QLatin1StringView("Bitonal copied pageword")));
        // A bitonal page stays small.
        QVERIFY2(QFileInfo(out).size() < 150 * 1024, qPrintable(QString::number(QFileInfo(out).size())));

        // And without its words, there's nothing to find.
        import::WorkerClient::PdfCopySettings pictures;
        pictures.searchable = false;
        const QString plain = m_dir.filePath(QStringLiteral("plain.pdf"));
        QCOMPARE(worker.pdfCopy(sample(QStringLiteral("bitonal.djvu")), QString(), plain, pictures, kTimeoutMs).failure,
                 import::WorkerFailure::None);
        QVERIFY(readBack(plain).text.join(QString()).trimmed().isEmpty());

        // At a lower resolution it's smaller still.
        import::WorkerClient::PdfCopySettings coarse;
        coarse.dpi = 100;
        const QString small = m_dir.filePath(QStringLiteral("coarse.pdf"));
        QCOMPARE(worker.pdfCopy(sample(QStringLiteral("bitonal.djvu")), QString(), small, coarse, kTimeoutMs).failure,
                 import::WorkerFailure::None);
        QVERIFY(QFileInfo(small).size() < QFileInfo(out).size());
    }

    void recognisedWordsGoInADjvusCopy()
    {
        // As the worker keeps them, for the made-up photo page, which has no text of its own.
        const QString folder = m_dir.filePath(QStringLiteral("recognised"));
        QVERIFY(QDir().mkpath(folder));
        const QJsonObject kept{{QStringLiteral("version"), 1},
                               {QStringLiteral("words"), QJsonArray{QJsonArray{10, 20, 60, 12, QStringLiteral("recognisedword"), true}}}};
        QFile file(QDir(folder).filePath(QStringLiteral("p0.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(kept).toJson());
        file.close();

        const QString out = m_dir.filePath(QStringLiteral("recognised.pdf"));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const auto copy = worker.pdfCopy(sample(QStringLiteral("scanned.djvu")), QString(), out, {}, kTimeoutMs, {}, folder);
        QVERIFY2(copy.failure == import::WorkerFailure::None, qPrintable(copy.error));
        QVERIFY(readBack(out).text.join(u' ').contains(QLatin1StringView("recognisedword")));
    }

    void theOriginalIsNeverReplaced()
    {
        // A copy of the sample, so a mistake here can't harm it.
        const QString book = m_dir.filePath(QStringLiteral("Own Book.epub"));
        QVERIFY(QFile::copy(sample(QStringLiteral("book.epub")), book));
        const QByteArray before = folderHash(m_dir.filePath(QString()));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        QStringList names{book, QDir(m_dir.path()).filePath(QStringLiteral("./sub/../Own Book.epub"))};
#ifdef Q_OS_WIN
        names << book.toUpper() << QDir::toNativeSeparators(book);
#endif
        for (const QString &name : std::as_const(names)) {
            const auto copy = worker.pdfCopy(book, QString(), name, {}, kTimeoutMs);
            QCOMPARE(copy.failure, import::WorkerFailure::Rejected);
        }
        QCOMPARE(folderHash(m_dir.filePath(QString())), before);
        QVERIFY(!QFileInfo::exists(book + QStringLiteral(".part")));
        QFile::remove(book);
    }

    void aPdfIsntCopied()
    {
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const QString out = m_dir.filePath(QStringLiteral("article copy.pdf"));
        QCOMPARE(worker.pdfCopy(sample(QStringLiteral("article.pdf")), QString(), out, {}, kTimeoutMs).failure,
                 import::WorkerFailure::Rejected);
        QVERIFY(!QFileInfo::exists(out));
    }

    void cancellingLeavesNoPartFile()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        // The worker stops answering after the first page, as a very long book would seem to.
        const QString book = m_dir.filePath(QStringLiteral("long__hangpage1__.epub"));
        QVERIFY(QFile::copy(sample(QStringLiteral("reader_nav.epub")), book));
        const QString out = m_dir.filePath(QStringLiteral("stopped.pdf"));
        ui::PdfCopyJob job(QStringLiteral(PAPERTRAIL_WORKER_PATH), {book, QString(), QString()}, out, {});
        QSignalSpy progress(&job, &ui::PdfCopyJob::progress);
        QSignalSpy finished(&job, &ui::PdfCopyJob::finished);
        job.start();
        QVERIFY(progress.wait(kWaitMs));
        // As if the worker had started saving when it was stopped.
        QFile part(out + QStringLiteral(".part"));
        QVERIFY(part.open(QIODevice::WriteOnly));
        part.write("%PDF-1.7 half written");
        part.close();
        job.cancel();
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(finished.first().at(0).toBool(), false);
        QCOMPARE(finished.first().at(1).toBool(), true);
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QVERIFY(!QFileInfo::exists(out));
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");
    }

    void aCrashIsReportedAndTheNextCopyWorks()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        const QString book = m_dir.filePath(QStringLiteral("bad__crash__.epub"));
        QVERIFY(QFile::copy(sample(QStringLiteral("book.epub")), book));
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        const QString out = m_dir.filePath(QStringLiteral("crashed.pdf"));
        const auto crashed = worker.pdfCopy(book, QString(), out, {}, kTimeoutMs);
        QCOMPARE(crashed.failure, import::WorkerFailure::Crashed);
        QVERIFY(!QFileInfo::exists(out));
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");

        const auto good = worker.pdfCopy(sample(QStringLiteral("book.epub")), QString(), out, {}, kTimeoutMs);
        QVERIFY2(good.failure == import::WorkerFailure::None, qPrintable(good.error));
        QVERIFY(QFileInfo::exists(out));
    }

    void progressLinesArentTheReply()
    {
        // A fake worker that answers after 800 ms, saying how it's getting on every 400 ms, within a limit of 600 ms.
        qputenv(kFakeWorker, "1");
        import::WorkerClient worker(QCoreApplication::applicationFilePath());
        QList<QPair<int, int>> heard;
        const auto copy = worker.pdfCopy(QStringLiteral("made-up.epub"), QString(), m_dir.filePath(QStringLiteral("fake.pdf")), {}, 600,
                                         [&](int done, int of) { heard << qMakePair(done, of); });
        qunsetenv(kFakeWorker);
        QVERIFY2(copy.failure == import::WorkerFailure::None, qPrintable(copy.error));
        QCOMPARE(copy.pages, 7);
        QCOMPARE(heard, (QList<QPair<int, int>>{{1, 2}, {2, 2}}));

        // Without them, the same silence is too long.
        qputenv(kFakeWorker, "1");
        import::WorkerClient impatient(QCoreApplication::applicationFilePath());
        const auto hash = impatient.hash(QStringLiteral("made-up.epub"), 300);
        qunsetenv(kFakeWorker);
        QCOMPARE(hash.failure, import::WorkerFailure::TimedOut);
    }

    void theDialogOffersWhatSuitsTheBook()
    {
        ui::PdfCopyDialog reflowable(ui::PdfCopyKind::Reflowable, 14, QSizeF(520, 720));
        QVERIFY(reflowable.pageSizes());
        QVERIFY(!reflowable.qualities());
        QCOMPARE(reflowable.pageSizes()->count(), 5);
        QCOMPARE(reflowable.settings().pageWidth, 595.0);
        QCOMPARE(reflowable.settings().fontSize, 14.0);
        QCOMPARE(reflowable.settings().margin, 54.0);
        reflowable.pageSizes()->setCurrentIndex(2);
        QCOMPARE(reflowable.settings().pageWidth, 420.0);
        QCOMPARE(reflowable.settings().pageHeight, 595.0);
        reflowable.pageSizes()->setCurrentIndex(4);
        QCOMPARE(reflowable.settings().pageWidth, 520.0);
        reflowable.fontSize()->setValue(40);
        QCOMPARE(reflowable.settings().fontSize, 32.0);
        reflowable.margins()->setCurrentIndex(2);
        QVERIFY(reflowable.settings().margin > 54.0);

        ui::PdfCopyDialog djvu(ui::PdfCopyKind::Djvu, 12, QSizeF(520, 720));
        QVERIFY(!djvu.pageSizes());
        QCOMPARE(djvu.settings().dpi, 300);
        QVERIFY(djvu.settings().searchable);
        djvu.qualities()->setCurrentIndex(1);
        djvu.searchable()->setChecked(false);
        QCOMPARE(djvu.settings().dpi, 0);
        QVERIFY(!djvu.settings().searchable);

        QCOMPARE(ui::pdfCopyKindFor(QStringLiteral("azw3")), ui::PdfCopyKind::Reflowable);
        QCOMPARE(ui::pdfCopyKindFor(QStringLiteral("djvu")), ui::PdfCopyKind::Djvu);
        QCOMPARE(ui::pdfCopyKindFor(QStringLiteral("cbz")), ui::PdfCopyKind::Fixed);
        QVERIFY(!ui::pdfCopyKindFor(QStringLiteral("pdf")));
        QVERIFY(!ui::pdfCopyKindFor(QStringLiteral("zip")));
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_samplesBefore;
};

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsSet(kFakeWorker))
        return fakeWorker();
    QApplication app(argc, argv);
    PdfCopyTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_pdf_copy.moc"
