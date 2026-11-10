// Tests of the reader, using the real document process and real sample documents, with no screen.

#include "KindleSamples.h"

#include "core/repo/PublicationMerge.h"
#include "core/repo/BookmarkRepository.h"
#include "core/repo/ReadingStateRepository.h"
#include "ui/DetailsPanel.h"
#include "ui/DocumentClient.h"
#include "ui/ImportThread.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/PageView.h"
#include "ui/PublicationListModel.h"
#include "ui/ReaderPair.h"
#include "ui/ReaderTab.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QListWidget>
#include <QMenuBar>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::ui;

Q_DECLARE_METATYPE(papertrail::ui::DocumentClient::Failure)
Q_DECLARE_METATYPE(QList<papertrail::ui::ReaderOutlineItem>)
Q_DECLARE_METATYPE(QList<papertrail::ui::ReaderLink>)

namespace {

constexpr int kWaitMs = 60000;

QString sample(const QString &name)
{
    return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name);
}

QByteArray hashOf(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

} // namespace

class ReaderTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<DocumentClient::Failure>();
        qRegisterMetaType<QList<ReaderOutlineItem>>();
        qRegisterMetaType<QList<ReaderLink>>();
        qRegisterMetaType<QList<QSizeF>>();
        qRegisterMetaType<QList<QRectF>>();
    }

    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();

        // An error box would wait for a click forever, with no one here to click it. A test that
        // brings one up fails, and says what it said.
        m_unexpectedBoxes.clear();
        m_boxWatcher = std::make_unique<QTimer>();
        connect(m_boxWatcher.get(), &QTimer::timeout, this, [this] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                m_unexpectedBoxes << box->text();
                box->close();
            }
        });
        m_boxWatcher->start(100);
    }

    void cleanup()
    {
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
        m_window.reset();
        m_session.reset();
        m_dir.reset();
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");
    }

    // ---- The document process ----

    void opensAPdfAndMeasuresItsPages()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy sizes(&client, &DocumentClient::sizesReady);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));
        QCOMPARE(opened.first().at(0).toInt(), 6);
        QCOMPARE(opened.first().at(1).toBool(), false);

        QTRY_VERIFY_WITH_TIMEOUT(!sizes.isEmpty(), kWaitMs);
        const auto measured = sizes.first().at(1).value<QList<QSizeF>>();
        QCOMPARE(measured.size(), 6);
        QCOMPARE(measured.at(0), QSizeF(595, 842));
        QCOMPARE(measured.at(3), QSizeF(842, 595));
    }

    void rendersAPage()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));
        client.setWantedPages({{0, 2.0}});
        QVERIFY(rendered.wait(kWaitMs));
        const QImage image = rendered.first().at(2).value<QImage>();
        QCOMPARE(image.width(), 1190);
        QCOMPARE(image.height(), 1684);
        QCOMPARE(QColor(image.pixel(2, 2)), QColor(Qt::white));

        // There's something other than white where the heading is.
        bool foundInk = false;
        for (int y = 100; y < 160 && !foundInk; ++y) {
            for (int x = 100; x < 500 && !foundInk; ++x)
                foundInk = qGray(image.pixel(x, y)) < 128;
        }
        QVERIFY(foundInk);
    }

    void onlyTheNewestWantedPagesAreRendered()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.setWantedPages({{0, 1.0}, {1, 1.0}, {2, 1.0}}); // At most one of these can have started.
        client.setWantedPages({{5, 1.0}});
        QTRY_VERIFY_WITH_TIMEOUT(!rendered.isEmpty() && rendered.last().at(0).toInt() == 5, kWaitMs);
        QTest::qWait(200);
        QVERIFY(rendered.size() <= 2);
        for (const QList<QVariant> &result : rendered)
            QVERIFY(result.at(0).toInt() != 1 && result.at(0).toInt() != 2);
    }

    void aHugePageIsLimited()
    {
        // A file can claim any size of page: this one's 5 metres across.
        const QString path = m_dir->filePath(QStringLiteral("huge.pdf"));
        const QByteArray content = "BT /F1 400 Tf 100 7000 Td (Big) Tj ET";
        QByteArray pdf = "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
                         "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
                         "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 14400 14400] /Contents 4 0 R >>\nendobj\n";
        pdf += "4 0 obj\n<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream\nendobj\n";
        pdf += "trailer\n<< /Root 1 0 R >>\n";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(pdf);
        file.close();

        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        client.open(path, {});
        QVERIFY(opened.wait(kWaitMs));
        client.setWantedPages({{0, 8.0}});
        QVERIFY(rendered.wait(kWaitMs));
        const QImage image = rendered.first().at(2).value<QImage>();
        QVERIFY(image.width() <= 16000);
        QVERIFY(qint64(image.width()) * image.height() <= qint64(101e6));
    }

    void readsTheContentsAndLinksOfAPdf()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy outline(&client, &DocumentClient::outlineReady);
        QSignalSpy links(&client, &DocumentClient::linksReady);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.requestOutline();
        QVERIFY(outline.wait(kWaitMs));
        const auto items = outline.first().at(0).value<QList<ReaderOutlineItem>>();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(0).title, QStringLiteral("Chapter One"));
        QCOMPARE(items.at(0).depth, 0);
        QCOMPARE(items.at(1).title, QStringLiteral("A Section"));
        QCOMPARE(items.at(1).depth, 1);
        QCOMPARE(items.at(1).page, 1);
        QCOMPARE(items.at(2).page, 3);

        client.requestLinks(0);
        QVERIFY(links.wait(kWaitMs));
        const auto found = links.first().at(1).value<QList<ReaderLink>>();
        QCOMPARE(found.size(), 3);
        bool internal = false;
        bool web = false;
        bool unsafe = false;
        for (const ReaderLink &link : found) {
            internal = internal || (link.uri.isEmpty() && link.page == 4);
            web = web || link.uri == QStringLiteral("https://example.org/paper");
            unsafe = unsafe || link.uri.startsWith(QStringLiteral("file:"));
        }
        QVERIFY(internal && web && unsafe);
    }

    void searchesEveryPageStartingFromAPlace()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.startSearch(QStringLiteral("findme"), 3);
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(hits.size(), 2);
        QCOMPARE(hits.at(0).at(0).toInt(), 4); // The first found is the nearest after page 3.
        QCOMPARE(hits.at(0).at(1).value<QList<QRectF>>().size(), 2);
        QCOMPARE(hits.at(1).at(0).toInt(), 1);
    }

    void readsLinesAndPartsOfAPage()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy lines(&client, &DocumentClient::linesReady);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        QSignalSpy text(&client, &DocumentClient::areaTextReady);
        QSignalSpy picture(&client, &DocumentClient::areaPictureReady);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.startSearch(QStringLiteral("findme"), 4);
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(hits.first().at(0).toInt(), 4);
        const QRectF hit = hits.first().at(1).value<QList<QRectF>>().first();

        // Each character has a box, and the word's agree with where searching found it.
        client.requestLines(4);
        QVERIFY(lines.wait(kWaitMs));
        QCOMPARE(lines.first().at(0).toInt(), 4);
        const auto found = lines.first().at(1).value<QList<ReaderTextLine>>();
        QVERIFY(!found.isEmpty());
        bool matched = false;
        for (const ReaderTextLine &line : found) {
            QCOMPARE(line.chars.size(), line.text.size());
            const qsizetype at = line.text.indexOf(QStringLiteral("findme"));
            if (at < 0)
                continue;
            QRectF word = line.chars.at(at);
            for (qsizetype i = at; i < at + 6; ++i)
                word = word.united(line.chars.at(i));
            if (qAbs(word.left() - hit.left()) < 1.5 && qAbs(word.top() - hit.top()) < 3)
                matched = true;
        }
        QVERIFY(matched);

        // The text inside part of a page, and a picture of it at the scale asked for.
        const QRectF area = hit.adjusted(-2, -2, 2, 2);
        client.requestAreaText(7, 4, area);
        QVERIFY(text.wait(kWaitMs));
        QCOMPARE(text.first().at(0).toLongLong(), qint64(7));
        QCOMPARE(text.first().at(2).toString(), QStringLiteral("findme"));
        client.requestAreaPicture(8, 4, area, 2.0, true);
        QVERIFY(picture.wait(kWaitMs));
        QCOMPARE(picture.first().at(0).toLongLong(), qint64(8));
        const QImage image = picture.first().at(2).value<QImage>();
        QVERIFY(!image.isNull());
        QVERIFY(qAbs(image.width() - area.width() * 2) <= 2);
        QVERIFY(qAbs(image.height() - area.height() * 2) <= 2);

        // Part of a page that isn't on it at all is answered all the same, with nothing.
        picture.clear();
        client.requestAreaPicture(9, 4, QRectF(-500, -500, 10, 10), 2.0, false);
        QVERIFY(picture.wait(kWaitMs));
        QVERIFY(picture.first().at(2).value<QImage>().isNull());
    }

    void findsWhatReferencesPointAtWithoutLinks()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        QSignalSpy found(&client, &DocumentClient::referenceReady);
        client.open(sample(QStringLiteral("smart_jump.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));
        QCOMPARE(opened.first().at(0).toInt(), 4);

        // Where a word is on the first page, as the reader's own search finds it.
        const auto where = [&](const QString &word) {
            hits.clear();
            finished.clear();
            client.startSearch(word, 0);
            if (!finished.wait(kWaitMs) || hits.isEmpty() || hits.first().at(0).toInt() != 0)
                return QRectF();
            return hits.first().at(1).value<QList<QRectF>>().first();
        };
        const auto lookUp = [&](const QString &word, int first = 0, int last = 3) {
            const QRectF rect = where(word);
            found.clear();
            client.requestReference(0, rect.center(), first, last);
            return found.wait(kWaitMs) ? found.first().first().value<ReaderReference>() : ReaderReference{};
        };

        ReaderReference figure = lookUp(QStringLiteral("Figure 2"));
        QVERIFY(figure.found);
        QCOMPARE(figure.label, QStringLiteral("Figure 2"));
        QCOMPARE(figure.page, 2);
        // The caption, not the mention of it beneath, and the picture above it with it.
        QVERIFY2(figure.rect.top() > 400 && figure.rect.top() < 430, qPrintable(QString::number(figure.rect.top())));
        QVERIFY(figure.region.top() < 150);
        QVERIFY(figure.region.bottom() > figure.rect.bottom() - 1);

        ReaderReference table = lookUp(QStringLiteral("Table 1"));
        QVERIFY(table.found);
        QCOMPARE(table.page, 1);
        QVERIFY(table.region.contains(table.rect.center()));

        ReaderReference equation = lookUp(QStringLiteral("(3)"));
        QVERIFY(equation.found);
        QCOMPARE(equation.label, QStringLiteral("Equation 3"));
        QCOMPARE(equation.page, 1);

        ReaderReference section = lookUp(QStringLiteral("Section 2"));
        QVERIFY(section.found);
        QCOMPARE(section.page, 1);

        // In the references, the whole entry, which runs onto a second line.
        ReaderReference numbered = lookUp(QStringLiteral("[4]"));
        QVERIFY(numbered.found);
        QCOMPARE(numbered.label, QStringLiteral("reference 4"));
        QCOMPARE(numbered.page, 3);
        QVERIFY(numbered.region.height() > numbered.rect.height() * 1.8);

        ReaderReference named = lookUp(QStringLiteral("Garner"));
        QVERIFY(named.found);
        QCOMPARE(named.label, QStringLiteral("Garner 2019"));
        QCOMPARE(named.page, 3);
        QVERIFY(named.rect.top() > numbered.rect.top());

        // Only within the pages it's asked to look in, as for an article that's part of a file.
        ReaderReference outside = lookUp(QStringLiteral("[4]"), 0, 2);
        QVERIFY(!outside.found);
        QCOMPARE(outside.label, QStringLiteral("reference 4"));

        // Words that aren't a reference, and a click on nothing.
        ReaderReference plain = lookUp(QStringLiteral("beams bend"));
        QVERIFY(!plain.found);
        QVERIFY(plain.label.isEmpty());
        found.clear();
        client.requestReference(0, QPointF(500, 820), 0, 3);
        QVERIFY(found.wait(kWaitMs));
        QVERIFY(!found.first().first().value<ReaderReference>().found);
    }

    void aMentionAtTheStartOfALineIsntTheThingItself()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        QSignalSpy found(&client, &DocumentClient::referenceReady);
        client.open(sample(QStringLiteral("smart_jump_headings.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        // The first of each on the first page is the mention clicked.
        const auto lookUp = [&](const QString &word) {
            hits.clear();
            finished.clear();
            client.startSearch(word, 0);
            if (!finished.wait(kWaitMs) || hits.isEmpty())
                return ReaderReference{};
            const QRectF first = hits.first().at(1).value<QList<QRectF>>().first();
            found.clear();
            client.requestReference(0, first.center(), 0, 2);
            return found.wait(kWaitMs) ? found.first().first().value<ReaderReference>() : ReaderReference{};
        };

        // Not "Appendix E." wrapped onto the start of a line two lines down, but the heading in bold.
        const ReaderReference appendix = lookUp(QStringLiteral("Appendix E,"));
        QVERIFY(appendix.found);
        QCOMPARE(appendix.label, QStringLiteral("Appendix E"));
        QCOMPARE(appendix.page, 2);

        // Not "Figure 4." starting a line in the paragraph, but its caption, set apart on the next page.
        const ReaderReference figure = lookUp(QStringLiteral("Figure 4 of"));
        QVERIFY(figure.found);
        QCOMPARE(figure.page, 1);
    }

    void aContentsEntryLeadsToItsHeadingAndAMentionSkipsTheContents()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        QSignalSpy found(&client, &DocumentClient::referenceReady);
        client.open(sample(QStringLiteral("smart_jump_contents.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        // Ctrl+click on the first place these words are, from page `from` on.
        const auto clickOn = [&](const QString &words, int from) {
            hits.clear();
            finished.clear();
            client.startSearch(words, from);
            if (!finished.wait(kWaitMs) || hits.isEmpty())
                return ReaderReference{};
            const int page = hits.first().at(0).toInt();
            const QRectF first = hits.first().at(1).value<QList<QRectF>>().first();
            found.clear();
            client.requestReference(page, first.center(), 0, 165);
            return found.wait(kWaitMs) ? found.first().first().value<ReaderReference>() : ReaderReference{};
        };

        // The contents give page numbers set at the right, with no dots and no links. Each entry leads to its heading:
        // a section, a chapter, and an appendix, whether the title or its number is clicked.
        const ReaderReference section = clickOn(QStringLiteral("Bending Made-Up"), 0);
        QVERIFY(section.found);
        QCOMPARE(section.page, 3);
        QVERIFY(section.label.contains(QStringLiteral("Bending Made-Up Beams")));
        QVERIFY(section.rect.top() > 100); // The section's heading, not the chapter's at the top of the page.
        QCOMPARE(clickOn(QStringLiteral("Made-Up Beginnings"), 0).page, 2);
        QCOMPARE(clickOn(QStringLiteral("Made-Up Tables"), 0).page, 164);
        QCOMPARE(clickOn(QStringLiteral("Index"), 0).page, 165);

        // "Appendix E." starting a line in a paragraph, and the contents entry for it, aren't the appendix: its heading
        // is, which the contents say is further away than Smart Jump would otherwise look.
        const ReaderReference appendix = clickOn(QStringLiteral("Appendix E. The"), 1);
        QVERIFY(appendix.found);
        QCOMPARE(appendix.page, 164);
        const ReaderReference mention = clickOn(QStringLiteral("Section 2.1"), 1);
        QVERIFY(mention.found);
        QCOMPARE(mention.page, 3);
    }

    void selectsTextBetweenTwoPoints()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy selected(&client, &DocumentClient::selectionReady);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QVERIFY(opened.wait(kWaitMs));

        client.requestSelection(5, QPointF(40, 50), QPointF(300, 70));
        QVERIFY(selected.wait(kWaitMs));
        QVERIFY2(selected.first().at(2).toString().contains(QStringLiteral("The last page")),
                 qPrintable(selected.first().at(2).toString()));
        QVERIFY(!selected.first().at(1).value<QList<QRectF>>().isEmpty());
    }

    void layingOutAnEpubAgainKeepsThePlace()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy changed(&client, &DocumentClient::layoutChanged);
        ReaderLayout layout;
        client.open(sample(QStringLiteral("reader_nav.epub")), layout);
        QVERIFY(opened.wait(kWaitMs));
        QVERIFY(opened.first().at(1).toBool());
        const int pages = opened.first().at(0).toInt();
        QVERIFY(pages > 3);

        layout.fontSize = 24;
        client.setLayout(layout, pages - 1);
        QVERIFY(changed.wait(kWaitMs));
        const int larger = changed.first().at(0).toInt();
        QVERIFY2(larger > pages, qPrintable(QStringLiteral("%1 pages, then %2").arg(pages).arg(larger)));
        // The last page's text is still near the end, on a different page number.
        QVERIFY(changed.first().at(1).toInt() > pages - 1);
    }

    void readsBothKindsOfEpubContents_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("items");
        QTest::newRow("EPUB 3 navigation document only") << "reader_nav.epub" << 4;
        QTest::newRow("older NCX only") << "reader_ncx.epub" << 3;
    }

    void readsBothKindsOfEpubContents()
    {
        QFETCH(QString, file);
        QFETCH(int, items);
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy outline(&client, &DocumentClient::outlineReady);
        client.open(sample(file), {});
        QVERIFY(opened.wait(kWaitMs));
        client.requestOutline();
        QVERIFY(outline.wait(kWaitMs));
        const auto found = outline.first().at(0).value<QList<ReaderOutlineItem>>();
        QCOMPARE(found.size(), items);
        QCOMPARE(found.first().title, QStringLiteral("The First Chapter"));
        QCOMPARE(found.first().page, 0);
        int previous = -1;
        for (const ReaderOutlineItem &item : found) {
            QVERIFY2(item.page > previous || (item.page >= 0 && item.page == previous), qPrintable(item.title));
            previous = item.page;
        }
        QVERIFY(found.last().page > 0);
        if (items == 4) {
            QCOMPARE(found.at(2).depth, 1); // Nested, and it stops before the landmarks list.
            QCOMPARE(found.at(2).title, QStringLiteral("A Part of It"));
        }
    }

    // ---- Every format ----

    void everyFormatOpensAndShowsSomething_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<bool>("reflowable");
        QTest::addColumn<QString>("word"); // Empty if it has no text to find.
        QTest::newRow("PDF") << "reader_doc.pdf" << false << "findme";
        QTest::newRow("EPUB") << "reader_nav.epub" << true << "findmefirst";
        QTest::newRow("DjVu") << "reader_doc.djvu" << false << "findme";
        QTest::newRow("MOBI") << "old_format.mobi" << true << "made-up";
        QTest::newRow("FB2") << "sample.fb2" << true << "made-up";
        QTest::newRow("KF8") << "kf8_frankenstein.azw3" << true << "Walton";
        QTest::newRow("CBZ") << "sample.cbz" << false << "";
        QTest::newRow("XPS") << "sample.xps" << false << "";
    }

    void everyFormatOpensAndShowsSomething()
    {
        QFETCH(QString, file);
        QFETCH(bool, reflowable);
        QFETCH(QString, word);

        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy failed(&client, &DocumentClient::failed);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        client.open(sample(file), {}, m_dir->filePath(QStringLiteral("converted.epub")));
        QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty() || !failed.isEmpty(), kWaitMs);
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.first().at(1).toString()));
        QCOMPARE(opened.first().at(1).toBool(), reflowable);

        client.setWantedPages({{0, 1.0}});
        QVERIFY(rendered.wait(kWaitMs));
        const QImage image = rendered.first().at(2).value<QImage>();
        bool foundInk = false;
        for (int y = 0; y < image.height() && !foundInk; y += 2) {
            for (int x = 0; x < image.width() && !foundInk; x += 2)
                foundInk = qGray(image.pixel(x, y)) < 200;
        }
        QVERIFY2(foundInk, "the first page is blank");

        if (!word.isEmpty()) {
            client.startSearch(word, 0);
            QVERIFY(finished.wait(kWaitMs));
            QVERIFY2(!hits.isEmpty(), "the text couldn't be searched");
        }
    }

    void otherNamesForFormatsOpen_data()
    {
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("as");
        QTest::newRow(".azw") << "old_format.mobi" << "book.azw";
        QTest::newRow(".oxps") << "sample.xps" << "book.oxps";
        QTest::newRow(".djv") << "reader_doc.djvu" << "book.djv";
    }

    void otherNamesForFormatsOpen()
    {
        QFETCH(QString, from);
        QFETCH(QString, as);
        const QString path = copied(from, as);
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        client.open(path, {});
        QVERIFY(opened.wait(kWaitMs));
    }

    void djvuHasContentsLinksSearchAndSelection()
    {
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy outline(&client, &DocumentClient::outlineReady);
        QSignalSpy links(&client, &DocumentClient::linksReady);
        QSignalSpy hits(&client, &DocumentClient::searchHits);
        QSignalSpy finished(&client, &DocumentClient::searchFinished);
        QSignalSpy selected(&client, &DocumentClient::selectionReady);
        client.open(sample(QStringLiteral("reader_doc.djvu")), {});
        QVERIFY(opened.wait(kWaitMs));
        QCOMPARE(opened.first().at(0).toInt(), 3);

        client.requestOutline();
        QVERIFY(outline.wait(kWaitMs));
        const auto items = outline.first().at(0).value<QList<ReaderOutlineItem>>();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(0).title, QStringLiteral("First Part"));
        QCOMPARE(items.at(0).page, 0);
        QCOMPARE(items.at(1).depth, 1);
        QCOMPARE(items.at(1).page, 1);
        QCOMPARE(items.at(2).page, 2);

        client.requestLinks(0);
        QVERIFY(links.wait(kWaitMs));
        const auto found = links.first().at(1).value<QList<ReaderLink>>();
        QCOMPARE(found.size(), 3);
        bool internal = false;
        bool web = false;
        bool unsafe = false;
        for (const ReaderLink &link : found) {
            internal = internal || (link.uri.isEmpty() && link.page == 2);
            web = web || link.uri == QStringLiteral("https://example.org/djvu");
            unsafe = unsafe || link.uri.startsWith(QStringLiteral("file:"));
        }
        QVERIFY(internal && web && unsafe);

        // Any case, on the pages they're on: one on the first, and two on the last.
        client.startSearch(QStringLiteral("findme"), 0);
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(hits.size(), 2);
        QCOMPARE(hits.at(0).at(0).toInt(), 0);
        QCOMPARE(hits.at(0).at(1).value<QList<QRectF>>().size(), 1);
        QCOMPARE(hits.at(1).at(0).toInt(), 2);
        QCOMPARE(hits.at(1).at(1).value<QList<QRectF>>().size(), 2);

        // The first word is at (50, 700) in 800 pixels up from the bottom, at 100 dpi: near the top.
        const QRectF first = hits.at(0).at(1).value<QList<QRectF>>().first();
        QVERIFY(first.top() < 60);
        QVERIFY(first.left() > 100);

        // A phrase, including one running over a line break.
        hits.clear();
        finished.clear();
        client.startSearch(QStringLiteral("words second"), 0);
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.first().at(1).value<QList<QRectF>>().size(), 2);

        // From the first word to the last, keeping the line break.
        client.requestSelection(0, QPointF(40, 50), QPointF(280, 100));
        QVERIFY(selected.wait(kWaitMs));
        QCOMPARE(selected.first().at(2).toString(), QStringLiteral("Plain findme words\nsecond line"));

        // Its lines, with each word's box shared out among its letters, and the text inside part of the page.
        QSignalSpy lines(&client, &DocumentClient::linesReady);
        QSignalSpy text(&client, &DocumentClient::areaTextReady);
        client.requestLines(0);
        QVERIFY(lines.wait(kWaitMs));
        const auto pageLines = lines.first().at(1).value<QList<ReaderTextLine>>();
        QCOMPARE(pageLines.size(), 2);
        QCOMPARE(pageLines.at(0).text, QStringLiteral("Plain findme words"));
        QCOMPARE(pageLines.at(0).chars.size(), pageLines.at(0).text.size());
        QCOMPARE(pageLines.at(1).text, QStringLiteral("second line"));
        const qsizetype at = pageLines.at(0).text.indexOf(QStringLiteral("findme"));
        QVERIFY(qAbs(pageLines.at(0).chars.at(at).left() - first.left()) < 0.5);
        client.requestAreaText(1, 0, first.adjusted(-1, -1, 1, 1));
        QVERIFY(text.wait(kWaitMs));
        QCOMPARE(text.first().at(2).toString(), QStringLiteral("findme"));
        QCOMPARE(selected.first().at(1).value<QList<QRectF>>().size(), 5);
    }

    void aKf8BookOpensWithItsContentsAndLeavesTheFileAlone()
    {
        const QString path = copied(QStringLiteral("kf8_frankenstein.azw3"), QStringLiteral("Fr\u00E4nkenstein.azw3"));
        const QByteArray before = hashOf(path);
        const QString converted = m_dir->filePath(QStringLiteral("cache/book.epub"));
        {
            DocumentClient client(worker());
            QSignalSpy opened(&client, &DocumentClient::opened);
            QSignalSpy outline(&client, &DocumentClient::outlineReady);
            client.open(path, {}, converted);
            QVERIFY(opened.wait(kWaitMs));
            QVERIFY(opened.first().at(0).toInt() > 50);
            QVERIFY(opened.first().at(1).toBool());
            client.requestOutline();
            QVERIFY(outline.wait(kWaitMs));
            QVERIFY(outline.first().at(0).value<QList<ReaderOutlineItem>>().size() > 5);
        }
        QVERIFY(QFile::exists(converted));
        QCOMPARE(hashOf(path), before);

        // The converted copy is used next time, rather than being made again.
        const QDateTime made = QFileInfo(converted).lastModified();
        QTest::qWait(20);
        DocumentClient again(worker());
        QSignalSpy opened(&again, &DocumentClient::opened);
        again.open(path, {}, converted);
        QVERIFY(opened.wait(kWaitMs));
        QCOMPARE(QFileInfo(converted).lastModified(), made);
    }

    void aJointKindleFileOpensAsItsKf8Part()
    {
        const QString path = m_dir->filePath(QStringLiteral("joint.mobi"));
        QVERIFY(kindle_samples::writeAll(path, kindle_samples::jointFile(kindle_samples::readAll(sample(QStringLiteral("old_format.mobi"))),
                                                                         kindle_samples::readAll(sample(QStringLiteral("kf8_frankenstein.azw3"))))));
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        client.open(path, {}, m_dir->filePath(QStringLiteral("joint.epub")));
        QVERIFY(opened.wait(kWaitMs));
        QVERIFY2(opened.first().at(0).toInt() > 50, qPrintable(QString::number(opened.first().at(0).toInt())));
    }

    void aDrmLockedBookSaysSo()
    {
        const QString path = m_dir->filePath(QStringLiteral("locked.azw3"));
        QVERIFY(kindle_samples::writeAll(path, kindle_samples::lockedFile(kindle_samples::readAll(sample(QStringLiteral("kf8_frankenstein.azw3"))))));
        DocumentClient client(worker());
        QSignalSpy failed(&client, &DocumentClient::failed);
        client.open(path, {}, m_dir->filePath(QStringLiteral("locked.epub")));
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(failed.first().at(0).value<DocumentClient::Failure>(), DocumentClient::Failure::Protected);
        QVERIFY(!QFile::exists(m_dir->filePath(QStringLiteral("locked.epub"))));
    }

    void twoProcessesCanConvertOneKf8BookAtOnce()
    {
        // The reader and the indexer can both get to a book before it has been converted. Found on Arch.
        const QString converted = m_dir->filePath(QStringLiteral("shared.epub"));
        for (int round = 0; round < 3; ++round) {
            QFile::remove(converted);
            DocumentClient first(worker());
            DocumentClient second(worker());
            QSignalSpy firstOpened(&first, &DocumentClient::opened);
            QSignalSpy secondOpened(&second, &DocumentClient::opened);
            QSignalSpy firstFailed(&first, &DocumentClient::failed);
            QSignalSpy secondFailed(&second, &DocumentClient::failed);
            first.open(sample(QStringLiteral("kf8_frankenstein.azw3")), {}, converted);
            second.open(sample(QStringLiteral("kf8_frankenstein.azw3")), {}, converted);
            QTRY_VERIFY_WITH_TIMEOUT(firstOpened.size() + firstFailed.size() > 0 && secondOpened.size() + secondFailed.size() > 0, kWaitMs);
            QCOMPARE(firstFailed.size(), 0);
            QCOMPARE(secondFailed.size(), 0);
            QVERIFY(QFile::exists(converted));
        }
        QCOMPARE(QDir(m_dir->path()).entryList({QStringLiteral("*.tmp")}, QDir::Files).size(), 0);
    }

    void aKf8BookOpensFromTheLibraryAndResumes()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("kf8_frankenstein.azw3"));
        // Conversion is named by the file's contents, which the import would normally have worked out.
        const FileRecord file = m_session->files().filesFor(id).first();
        m_session->files().setHash(file.id, QStringLiteral("abcdef"));

        ReaderTab *reader = waitForReader(id);
        QVERIFY(reader->isReady());
        QVERIFY(!reader->isShowingProblem());
        QTRY_VERIFY_WITH_TIMEOUT(reader->pageView()->isPageLoaded(0), kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(reader->contents()->topLevelItemCount() > 5, kWaitMs);
        reader->pageView()->goToPage(20);
        m_window->closeDocumentTab(1);

        reader = waitForReader(id);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->topPage(), 20, kWaitMs);
    }

    void djvuLinksInADocumentAreFollowedSafely()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.djvu"));
        PageView *view = reader->pageView();
        QStringList opened;
        reader->setOpenLinkHandler([&](const QUrl &url) { opened << url.toString(); });
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QTest::qWait(200); // For the page's links to arrive.
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 2, kWaitMs);

        // 100 dpi, so a point is 100/72 pixels, and the page is 576 points high.
        const auto click = [&](double pixelsUp) {
            const QPoint point = view->viewportPointFor(0, QPointF(100, (800 - pixelsUp) * 72.0 / 100.0));
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        };
        click(420);
        QCOMPARE(opened, QStringList{QStringLiteral("https://example.org/djvu")});
        click(320); // file: isn't followed.
        QCOMPARE(opened.size(), 1);
        click(520);
        QCOMPARE(view->topPage(), 2);
    }

    // ---- Files that go wrong ----

    void aFileThatCrashesOnOpeningIsReported()
    {
        const QString path = copied(QStringLiteral("reader_doc.pdf"), QStringLiteral("__crash__.pdf"));
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        DocumentClient client(worker());
        QSignalSpy failed(&client, &DocumentClient::failed);
        client.open(path, {});
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(failed.first().at(0).value<DocumentClient::Failure>(), DocumentClient::Failure::Crashed);
    }

    void aPageThatCrashesOnlyCostsThatPage()
    {
        const QString path = copied(QStringLiteral("reader_doc.pdf"), QStringLiteral("__crashpage2__.pdf"));
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy pageFailed(&client, &DocumentClient::pageFailed);
        QSignalSpy recovered(&client, &DocumentClient::recovered);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        client.open(path, {});
        QVERIFY(opened.wait(kWaitMs));

        client.setWantedPages({{2, 1.0}, {3, 1.0}});
        QTRY_COMPARE_WITH_TIMEOUT(pageFailed.size(), 1, kWaitMs);
        QCOMPARE(pageFailed.first().at(0).toInt(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(recovered.size(), 1, kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(rendered.size(), 1, kWaitMs);
        QCOMPARE(rendered.first().at(0).toInt(), 3);
        QCOMPARE(client.restartCount(), 1);

        // It isn't tried again, so scrolling back and forth can't keep crashing.
        client.setWantedPages({{2, 1.0}, {4, 1.0}});
        QTRY_COMPARE_WITH_TIMEOUT(rendered.size(), 2, kWaitMs);
        QCOMPARE(client.restartCount(), 1);
    }

    void aPageThatCrashesWhileLookingForAReferenceIsSurvived()
    {
        // The references are on the last page, which crashes the parser.
        const QString path = copied(QStringLiteral("smart_jump.pdf"), QStringLiteral("__crashpage3__.pdf"));
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        DocumentClient client(worker());
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy found(&client, &DocumentClient::referenceReady);
        QSignalSpy recovered(&client, &DocumentClient::recovered);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        client.open(path, {});
        QVERIFY(opened.wait(kWaitMs));

        // "Earlier work [4]" is set at the PDF's y of 668.
        client.requestReference(0, QPointF(117, 842 - 668 - 3), 0, 3);
        QTRY_COMPARE_WITH_TIMEOUT(found.size(), 1, kWaitMs);
        QVERIFY(!found.first().first().value<ReaderReference>().found);
        QTRY_COMPARE_WITH_TIMEOUT(recovered.size(), 1, kWaitMs);

        // The rest of the document carries on.
        client.setWantedPages({{0, 1.0}});
        QTRY_COMPARE_WITH_TIMEOUT(rendered.size(), 1, kWaitMs);
    }

    void aPageThatHangsOnlyCostsThatPage()
    {
        const QString path = copied(QStringLiteral("reader_doc.pdf"), QStringLiteral("__hangpage1__.pdf"));
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        DocumentClient client(worker());
        client.setTimeouts(kWaitMs, 500);
        QSignalSpy opened(&client, &DocumentClient::opened);
        QSignalSpy pageFailed(&client, &DocumentClient::pageFailed);
        QSignalSpy rendered(&client, &DocumentClient::pageRendered);
        client.open(path, {});
        QVERIFY(opened.wait(kWaitMs));

        client.setWantedPages({{1, 1.0}, {0, 1.0}});
        QTRY_COMPARE_WITH_TIMEOUT(pageFailed.size(), 1, kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(rendered.size(), 1, kWaitMs);
        QCOMPARE(rendered.first().at(0).toInt(), 0);
    }

    void badFilesNeverBringDownTheReader_data()
    {
        QTest::addColumn<QString>("file");
        QTest::newRow("truncated") << "truncated.pdf"; // This one really does crash Arch's MuPDF.
        QTest::newRow("garbage") << "garbage.pdf";
        QTest::newRow("empty") << "empty.pdf";
        QTest::newRow("encrypted") << "encrypted.pdf";
        QTest::newRow("not really an EPUB") << "not_really.epub";
        QTest::newRow("KF8 with nothing behind its header") << "kf8_header.azw3";
    }

    void badFilesNeverBringDownTheReader()
    {
        QFETCH(QString, file);
        openLibraryWindow();
        const qint64 id = addLinkedPublication(file);

        ReaderTab *reader = m_window->openPublication(id);
        QVERIFY(reader != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reader->isReady() || reader->isShowingProblem(), kWaitMs);
        if (reader->isReady()) {
            // Whatever it could recover, any page that can't be shown just says so.
            QTest::qWait(300);
        } else {
            QVERIFY(!reader->problemLabel()->text().isEmpty());
        }
        QVERIFY(m_window->isVisible());
        m_window->closeDocumentTab(m_window->documentTabs()->currentIndex());
        QCOMPARE(m_window->documentTabs()->count(), 1);
    }

    void aPasswordProtectedFileSaysSo()
    {
        DocumentClient client(worker());
        QSignalSpy failed(&client, &DocumentClient::failed);
        client.open(sample(QStringLiteral("encrypted.pdf")), {});
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(failed.first().at(0).value<DocumentClient::Failure>(), DocumentClient::Failure::NeedsPassword);
    }

    void aMissingHelperIsReported()
    {
        DocumentClient client(m_dir->filePath(QStringLiteral("no-such-worker")));
        QSignalSpy failed(&client, &DocumentClient::failed);
        client.open(sample(QStringLiteral("reader_doc.pdf")), {});
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(0).value<DocumentClient::Failure>(), DocumentClient::Failure::CantStart);
    }

    // ---- In the window ----

    void openingAPublicationGivesItATab()
    {
        openLibraryWindow();
        const qint64 first = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        const qint64 second = addLinkedPublication(QStringLiteral("article.pdf"));
        QCOMPARE(m_window->documentTabs()->count(), 1);

        ReaderTab *reader = waitForReader(first);
        QCOMPARE(m_window->documentTabs()->count(), 2);
        QCOMPARE(m_window->documentTabs()->currentWidget(), static_cast<QWidget *>(reader));

        // Opening it again goes back to the same tab.
        m_window->documentTabs()->setCurrentIndex(0);
        QCOMPARE(m_window->openPublication(first), reader);
        QCOMPARE(m_window->documentTabs()->count(), 2);

        ReaderTab *other = waitForReader(second);
        QVERIFY(other != reader);
        QCOMPARE(m_window->documentTabs()->count(), 3);

        // The library can't be closed, and each document can.
        m_window->closeDocumentTab(0);
        QCOMPARE(m_window->documentTabs()->count(), 3);
        m_window->closeDocumentTab(2);
        m_window->closeDocumentTab(1);
        QCOMPARE(m_window->documentTabs()->count(), 1);
    }

    void doubleClickingInTheLibraryOpensIt()
    {
        openLibraryWindow();
        addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->rowCount(), 1, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->model()->idAt(0).has_value(), kWaitMs);

        emit m_window->gridView()->activated(m_window->model()->index(0, 0));
        QCOMPARE(m_window->documentTabs()->count(), 2);
    }

    void aPublicationWithNoFileDoesNotOpen()
    {
        openLibraryWindow();
        Publication p;
        p.title = QStringLiteral("No File");
        const qint64 id = m_session->publications().create(p);
        QCOMPARE(m_window->openPublication(id), nullptr);
        QCOMPARE(m_window->documentTabs()->count(), 1);
    }

    void pagesAreShownAndMovedThrough()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        QCOMPARE(view->pageCount(), 6);
        QCOMPARE(reader->pageBox()->text(), QStringLiteral("1"));
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);

        view->goToPage(2);
        QCOMPARE(view->topPage(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(2), kWaitMs);
        QCOMPARE(reader->pageBox()->text(), QStringLiteral("3"));

        view->nextPage();
        QCOMPARE(view->topPage(), 3);
        view->previousPage();
        QCOMPARE(view->topPage(), 2);

        reader->pageBox()->setText(QStringLiteral("5"));
        emit reader->pageBox()->returnPressed();
        QCOMPARE(view->topPage(), 4);

        // Past the ends is simply the ends.
        view->goToPage(100);
        QCOMPARE(view->currentPage(), 5);
        view->goToPage(-5);
        QCOMPARE(view->topPage(), 0);
    }

    void layoutModesArrangeThePages()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();

        view->setLayoutMode(PageView::LayoutMode::Continuous);
        QVERIFY(view->pageRect(0).bottom() < view->pageRect(1).top());
        QVERIFY(!view->pageRect(5).isNull());

        view->setLayoutMode(PageView::LayoutMode::TwoPages);
        QCOMPARE(view->pageRect(0).top(), view->pageRect(1).top());
        QVERIFY(view->pageRect(0).right() < view->pageRect(1).left());
        QVERIFY(view->pageRect(2).isNull());
        view->nextPage();
        QVERIFY(view->pageRect(0).isNull());
        QVERIFY(!view->pageRect(2).isNull());
        QVERIFY(!view->pageRect(3).isNull());

        view->setLayoutMode(PageView::LayoutMode::SinglePage);
        QVERIFY(!view->pageRect(view->topPage()).isNull());
        QVERIFY(view->pageRect(view->topPage() + 1).isNull());

        // Remembered as the setting for next time.
        reader->layoutBox()->setCurrentIndex(reader->layoutBox()->findData(int(PageView::LayoutMode::TwoPages)));
        QCOMPARE(view->layoutMode(), PageView::LayoutMode::TwoPages);
        QCOMPARE(m_session->settings().value(QStringLiteral("reader.page_layout")).toString(), QStringLiteral("two_page"));
    }

    void zoomModesFitThePage()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        const int width = view->viewport()->width();

        view->setZoomMode(PageView::ZoomMode::FitWidth);
        QVERIFY(view->pageRect(0).width() <= width);
        QVERIFY(view->pageRect(0).width() >= width - 60);

        // The one landscape page in this document doesn't shrink the rest, or push them sideways.
        QVERIFY(view->pageRect(3).width() > width);
        QVERIFY(view->pageRect(0).left() >= 0);
        QCOMPARE(view->horizontalScrollBar()->maximum(), 0);
        view->goToPage(3);
        QVERIFY(view->horizontalScrollBar()->maximum() > 0);
        view->goToPage(0);

        view->setZoomMode(PageView::ZoomMode::FitPage);
        QVERIFY(view->pageRect(view->currentPage()).height() <= view->viewport()->height());

        view->setScale(2.0);
        QCOMPARE(view->zoomMode(), PageView::ZoomMode::Fixed);
        QCOMPARE(view->pageRect(0).width(), 1190.0);
        QCOMPARE(reader->zoomBox()->currentText(), QStringLiteral("150%"));

        // A zoom keeps the same place in view.
        view->goToPage(4);
        view->zoomBy(1.5);
        QCOMPARE(view->topPage(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(4), kWaitMs);
    }

    void pageColoursChangeWithoutRenderingAgain()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QSignalSpy rendered(reader->client(), &DocumentClient::pageRendered);

        const QPoint blank = view->viewportPointFor(0, QPointF(10, 10));
        const auto colourAt = [&] { return view->viewport()->grab().toImage().pixelColor(blank); };
        QCOMPARE(colourAt().lightness(), 255);

        view->setColours(PageView::Colours::Dark);
        QVERIFY(colourAt().lightness() < 60);

        view->setColours(PageView::Colours::Sepia);
        const QColor sepia = colourAt();
        QVERIFY(sepia.red() > sepia.blue());
        QVERIFY(sepia.lightness() > 150);

        reader->coloursBox()->setCurrentIndex(reader->coloursBox()->findData(int(PageView::Colours::Dark)));
        QCOMPARE(view->colours(), PageView::Colours::Dark);
        QCOMPARE(m_session->settings().value(QStringLiteral("reader.page_colours")).toString(), QStringLiteral("dark"));
        QCOMPARE(rendered.size(), 0);
    }

    void theWindowsThemeAndPageColoursAreSeparate()
    {
        // A dark window with light pages, each chosen on its own.
        openLibraryWindow();
        const QString styleBefore = QApplication::style()->name();
        m_session->settings().setValue(QStringLiteral("appearance.theme"), QStringLiteral("dark"));
        m_session->settings().setValue(QStringLiteral("reader.page_colours"), QStringLiteral("light"));
        applyTheme(Theme::Dark);
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 128);
        QCOMPARE(view->colours(), PageView::Colours::Light);
        const QPoint blank = view->viewportPointFor(0, QPointF(10, 10));
        QCOMPARE(view->viewport()->grab().toImage().pixelColor(blank).lightness(), 255);

        // And the other way round: a light window with dark pages.
        m_session->settings().setValue(QStringLiteral("appearance.theme"), QStringLiteral("light"));
        applyTheme(Theme::Light);
        QVERIFY(QApplication::palette().color(QPalette::Window).lightness() >= 128);
        QCOMPARE(view->colours(), PageView::Colours::Light); // The window's theme changes nothing in a document.
        reader->coloursBox()->setCurrentIndex(reader->coloursBox()->findData(int(PageView::Colours::Dark)));
        QVERIFY(view->viewport()->grab().toImage().pixelColor(blank).lightness() < 60);
        QCOMPARE(m_session->settings().value(QStringLiteral("appearance.theme")).toString(), QStringLiteral("light"));

        // Left as the other tests expect it.
        QApplication::setStyle(styleBefore);
        QApplication::setPalette(QPalette());
    }

    void darkPagesLeavePicturesAlone()
    {
        // The sample's picture is a grey square, 200 points across, its top-left 100 points in and 342 down.
        ReaderTab *reader = openSample(QStringLiteral("picture_page.pdf"));
        PageView *view = reader->pageView();
        view->setZoomMode(PageView::ZoomMode::FitPage); // So all of it is in view.
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        const QPoint paper = view->viewportPointFor(0, QPointF(30, 600));
        const QPoint picture = view->viewportPointFor(0, QPointF(200, 360));
        const auto colourAt = [&](const QPoint &point) { return view->viewport()->grab().toImage().pixelColor(point); };
        const int pictureBefore = colourAt(picture).lightness();
        QVERIFY2(pictureBefore > 120 && pictureBefore < 200, qPrintable(QString::number(pictureBefore)));

        // Light pages don't need to know where it is.
        QTest::qWait(200);
        QVERIFY(view->imageAreasOn(0).isEmpty());

        view->setColours(PageView::Colours::Dark);
        QTRY_COMPARE_WITH_TIMEOUT(view->imageAreasOn(0).size(), 1, kWaitMs);
        const QRectF area = view->imageAreasOn(0).first();
        QVERIFY2(qAbs(area.x() - 100) < 2 && qAbs(area.y() - 342) < 2 && qAbs(area.width() - 200) < 2,
                 qPrintable(QStringLiteral("%1,%2 %3x%4").arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height())));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(colourAt(picture).lightness() - pictureBefore) < 6, kWaitMs);
        QVERIFY(colourAt(paper).lightness() < 60);
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty())
            view->viewport()->grab().save(QDir(pictures).filePath(QStringLiteral("dark-page-picture.png")));
    }

    void aScanIsStillDarkened()
    {
        // A picture covering the whole page is the page itself, so it goes dark like any other.
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        view->setImageAreas(0, {QRectF(0, 0, 595, 842)});
        QVERIFY(view->imageAreasOn(0).isEmpty());
        view->setImageAreas(0, {QRectF(500, 800, 400, 400)});
        QCOMPARE(view->imageAreasOn(0).size(), 1); // Kept to the page.
        QVERIFY(view->imageAreasOn(0).first().right() <= 595.5);
    }

    void aPageThatCantBeShownDoesNotAffectTheOthers()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        openLibraryWindow();
        const QString name = QStringLiteral("__crashpage1__.pdf");
        copied(QStringLiteral("reader_doc.pdf"), name);
        const qint64 id = linkFile(name);

        ReaderTab *reader = waitForReader(id);
        PageView *view = reader->pageView();
        QSignalSpy failed(reader->client(), &DocumentClient::pageFailed);
        view->setLayoutMode(PageView::LayoutMode::SinglePage);
        view->goToPage(1);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, kWaitMs);
        QVERIFY(!reader->isShowingProblem());
        view->goToPage(2);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(2), kWaitMs);
    }

    // ---- Contents, thumbnails, links, search, selection ----

    void contentsLeadToTheirPages()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 2, kWaitMs);
        QTreeWidgetItem *chapterOne = reader->contents()->topLevelItem(0);
        QCOMPARE(chapterOne->text(0), QStringLiteral("Chapter One"));
        QCOMPARE(chapterOne->childCount(), 1);

        emit reader->contents()->itemClicked(reader->contents()->topLevelItem(1), 0);
        QCOMPARE(reader->pageView()->topPage(), 3);
        QVERIFY(reader->pageView()->canGoBack());

        reader->pageView()->goBack();
        QCOMPARE(reader->pageView()->topPage(), 0);

        // Forward goes to where Back came from, by key and by button, until something new is followed.
        PageView *view = reader->pageView();
        QVERIFY(view->canGoForward());
        QTest::keyClick(view, Qt::Key_Right, Qt::AltModifier);
        QCOMPARE(view->topPage(), 3);
        QVERIFY(!view->canGoForward());
        QTest::keyClick(view, Qt::Key_Left, Qt::AltModifier);
        QCOMPARE(view->topPage(), 0);
        QVERIFY(reader->forwardButton()->isEnabled());
        reader->forwardButton()->click();
        QCOMPARE(view->topPage(), 3);
        view->goBack();
        emit reader->contents()->itemClicked(reader->contents()->topLevelItem(1), 0);
        QVERIFY(!view->canGoForward());
        QVERIFY(!reader->forwardButton()->isEnabled());
    }

    void contentsAreFoundByTypingWithCtrlG()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 2, kWaitMs);
        QAction *goTo = m_window->goToAction();
        QCOMPARE(goTo->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_G));

        // It opens on everything, starting where the reader is.
        goTo->trigger();
        ContentsPopup *popup = reader->contentsPopup();
        QVERIFY(popup != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(popup->isVisible(), kWaitMs);
        QCOMPARE(popup->list()->topLevelItemCount(), 3);
        QCOMPARE(popup->list()->currentItem()->text(0), QStringLiteral("Chapter One"));
        QCOMPARE(popup->list()->topLevelItem(1)->text(0).trimmed(), QStringLiteral("A Section"));
        QCOMPARE(popup->list()->topLevelItem(2)->text(1), QStringLiteral("4"));

        // Typing narrows it down, and Enter goes there, which Back comes back from.
        // For looking at by eye, if asked for.
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty())
            popup->grab().save(QDir(pictures).filePath(QStringLiteral("contents-popup.png")));
        QTest::keyClicks(popup->searchBox(), QStringLiteral("two"));
        QCOMPARE(popup->list()->topLevelItemCount(), 1);
        QCOMPARE(popup->list()->currentItem()->text(0), QStringLiteral("Chapter Two"));
        QTest::keyClick(popup->searchBox(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(reader->contentsPopup() == nullptr, kWaitMs);
        QCOMPARE(reader->pageView()->topPage(), 3);
        reader->pageView()->goBack();
        QCOMPARE(reader->pageView()->topPage(), 0);

        // Words from what an entry is inside find it too, shown with them.
        goTo->trigger();
        popup = reader->contentsPopup();
        QTest::keyClicks(popup->searchBox(), QStringLiteral("section one"));
        QCOMPARE(popup->list()->topLevelItemCount(), 1);
        QCOMPARE(popup->list()->currentItem()->text(0), QStringLiteral("Chapter One › A Section"));
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty())
            popup->grab().save(QDir(pictures).filePath(QStringLiteral("contents-popup-search.png")));

        // A page number goes to that page.
        popup->searchBox()->clear();
        QTest::keyClicks(popup->searchBox(), QStringLiteral("5"));
        QCOMPARE(popup->list()->currentItem()->text(0), QStringLiteral("Go to page 5"));
        QTest::keyClick(popup->searchBox(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(reader->contentsPopup() == nullptr, kWaitMs);
        QCOMPARE(reader->pageView()->topPage(), 4);

        // Escape leaves the page as it was.
        goTo->trigger();
        popup = reader->contentsPopup();
        QTest::keyClicks(popup->searchBox(), QStringLiteral("one"));
        QTest::keyClick(popup->searchBox(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(reader->contentsPopup() == nullptr, kWaitMs);
        QCOMPARE(reader->pageView()->topPage(), 4);
    }

    void contentsMatchIgnoringCaseAccentsAndOrder()
    {
        const QList<OutlineEntry> entries = {
            {QStringLiteral("Résumé of Results"), QString(), 0, 0, 0, QStringLiteral("1")},
            {QStringLiteral("Methods"), QString(), 0, 2, 0, QStringLiteral("3")},
            {QStringLiteral("Sampling"), QStringLiteral("Methods"), 1, 3, 0, QStringLiteral("4")},
            {QStringLiteral("Results by method"), QString(), 0, 5, 0, QStringLiteral("6")},
        };
        QCOMPARE(contentsMatches(QStringLiteral("RESUME"), entries), QList<int>{0});
        QCOMPARE(contentsMatches(QStringLiteral("results résumé"), entries), QList<int>{0});
        // In the document's order, including those inside a match.
        QCOMPARE(contentsMatches(QStringLiteral("method"), entries), (QList<int>{1, 2, 3}));
        QCOMPARE(contentsMatches(QStringLiteral("methods sampling"), entries), QList<int>{2});
        QCOMPARE(contentsMatches(QStringLiteral("nothing"), entries), QList<int>{});

        // The words searched for are found in the title as shown, accents and all, to be drawn in bold.
        QCOMPARE(matchedRanges(QStringLiteral("Résumé of Results"), {QStringLiteral("resume")}), (QList<QPair<int, int>>{{0, 6}}));
        QCOMPARE(matchedRanges(QStringLiteral("Results by method"), {QStringLiteral("method"), QStringLiteral("res")}),
                 (QList<QPair<int, int>>{{0, 3}, {11, 6}}));
    }

    void thumbnailsLoadAsTheyreSeen()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        QSignalSpy thumbnails(reader->client(), &DocumentClient::thumbnailReady);
        auto *side = reader->findChild<QTabWidget *>();
        QVERIFY(side != nullptr);
        reader->setSidePanelVisible(true);
        QListView *list = reader->thumbnails();
        side->setCurrentWidget(list);
        // They're only asked for as they're drawn, and there's no screen here to draw on.
        QTRY_VERIFY_WITH_TIMEOUT((list->grab(), !thumbnails.isEmpty()), kWaitMs);
        QVERIFY(thumbnails.first().at(1).value<QImage>().width() <= 124);

        emit list->clicked(list->model()->index(4, 0));
        QCOMPARE(reader->pageView()->topPage(), 4);
    }

    void linksInADocumentAreFollowedSafely()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        QStringList opened;
        reader->setOpenLinkHandler([&](const QUrl &url) { opened << url.toString(); });
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QTest::qWait(200); // For the page's links to arrive.

        // In points from the top: the PDF's y of 727 on a page 842 high.
        const auto click = [&](double y) {
            const QPoint point = view->viewportPointFor(0, QPointF(100, y));
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        };

        // A web address goes through.
        click(842 - 707);
        QCOMPARE(opened, QStringList{QStringLiteral("https://example.org/paper")});

        // Anything else doesn't, whatever the document says.
        click(842 - 687);
        QCOMPARE(opened.size(), 1);

        // A link within the document goes to its page, and back again.
        click(842 - 727);
        QCOMPARE(view->topPage(), 4);
        view->goBack();
        QCOMPARE(view->topPage(), 0);
    }

    void ctrlClickingAReferencePreviewsWhereItPoints()
    {
        ReaderTab *reader = openSample(QStringLiteral("smart_jump.pdf"));
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QSignalSpy said(reader, &ReaderTab::statusMessage);

        // "As Figure 2 shows" is set at the PDF's y of 740, 11 points high, from 50 across: "Figure" is just after "As".
        const QPoint onFigure = view->viewportPointFor(0, QPointF(80, 842 - 740 - 3));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, onFigure);
        QVERIFY(!view->hasSelection());
        QTRY_VERIFY_WITH_TIMEOUT(reader->referencePreview() != nullptr, kWaitMs);
        ReferencePreview *preview = reader->referencePreview();
        QCOMPARE(preview->headingLabel()->text(), QStringLiteral("Figure 2, page 3"));
        QCOMPARE(preview->headingLabel()->textFormat(), Qt::PlainText);
        QVERIFY(!preview->pictureLabel()->pixmap().isNull());
        QVERIFY(preview->pictureLabel()->pixmap().deviceIndependentSize().width() > 200);
        QCOMPARE(view->topPage(), 0); // Only a look, so far.
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty())
            preview->grab().save(QDir(pictures).filePath(QStringLiteral("reference-preview.png")));

        // Going there can be come back from.
        QTest::keyClick(preview, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(reader->referencePreview() == nullptr, kWaitMs);
        QCOMPARE(view->currentPage(), 2);
        QVERIFY(view->canGoBack());
        view->goBack();
        QCOMPARE(view->topPage(), 0);

        // Escape closes it where it is.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, onFigure);
        QTRY_VERIFY_WITH_TIMEOUT(reader->referencePreview() != nullptr, kWaitMs);
        QTest::keyClick(reader->referencePreview(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(reader->referencePreview() == nullptr, kWaitMs);
        QCOMPARE(view->topPage(), 0);

        // Words that don't refer to anything say so.
        said.clear();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, view->viewportPointFor(0, QPointF(230, 842 - 740 - 3)));
        QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty() && said.last().first().toString() == QStringLiteral("There's nothing there to go to."),
                                 kWaitMs);
        QVERIFY(reader->referencePreview() == nullptr);
    }

    void ctrlClickingALinkPreviewsItsPage()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QTest::qWait(200); // For the page's links to arrive.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, view->viewportPointFor(0, QPointF(100, 842 - 727)));
        QTRY_VERIFY_WITH_TIMEOUT(reader->referencePreview() != nullptr, kWaitMs);
        QCOMPARE(reader->referencePreview()->headingLabel()->text(), QStringLiteral("Link, page 5"));
        QCOMPARE(view->topPage(), 0);
        reader->referencePreview()->goButton()->click();
        QCOMPARE(view->topPage(), 4);
    }

    void searchingFindsAndMovesThroughMatches()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();

        reader->searchBox()->setText(QStringLiteral("findme"));
        reader->applySearch();
        QTRY_COMPARE_WITH_TIMEOUT(reader->searchStatus()->text(), QStringLiteral("1 of 3"), kWaitMs);
        QCOMPARE(reader->hitCount(), 3);
        QCOMPARE(view->currentPage(), 1);

        reader->nextHit();
        QCOMPARE(reader->searchStatus()->text(), QStringLiteral("2 of 3"));
        QCOMPARE(view->currentPage(), 4);
        reader->nextHit();
        reader->nextHit(); // Wraps round to the first.
        QCOMPARE(reader->searchStatus()->text(), QStringLiteral("1 of 3"));
        reader->previousHit(); // And back round to the last.
        QCOMPARE(reader->searchStatus()->text(), QStringLiteral("3 of 3"));

        reader->searchBox()->setText(QStringLiteral("no such word anywhere"));
        reader->applySearch();
        QTRY_COMPARE_WITH_TIMEOUT(reader->searchStatus()->text(), QStringLiteral("No matches"), kWaitMs);
        QCOMPARE(reader->hitCount(), 0);

        reader->searchBox()->clear();
        reader->applySearch();
        QCOMPARE(reader->searchStatus()->text(), QString());
    }

    void textCanBeSelectedAndCopied()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        PageView *view = reader->pageView();
        view->setScale(1.5);
        view->goToPage(5);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(5), kWaitMs);

        QPoint from = view->viewportPointFor(5, QPointF(45, 58));
        QPoint to = view->viewportPointFor(5, QPointF(300, 68));
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), view->viewport()->mapToGlobal(QPointF(to)), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &move);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, to);

        QTRY_VERIFY_WITH_TIMEOUT(view->hasSelection(), kWaitMs);
        QVERIFY2(view->selectedText().contains(QStringLiteral("The last page")), qPrintable(view->selectedText()));

        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("The last page")));

        view->clearSelection();
        QVERIFY(!view->hasSelection());
    }

    // ---- EPUB and coming back ----

    void anEpubCanHaveItsTextResized()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_nav.epub"));
        QVERIFY(reader->isReflowable());
        PageView *view = reader->pageView();
        const int pages = view->pageCount();
        QVERIFY(pages > 3);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 3, kWaitMs);

        view->goToPage(pages - 1);
        QSignalSpy changed(reader->client(), &DocumentClient::layoutChanged);
        reader->changeFontSize(+8);
        QVERIFY(changed.wait(kWaitMs));
        QVERIFY(view->pageCount() > pages);
        QVERIFY(view->topPage() > pages - 1);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(view->currentPage()), kWaitMs);
        QCOMPARE(m_session->settings().value(QStringLiteral("reader.font_size")).toDouble(), 20.0);

        // The contents follow the new pages, and searching still works.
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 3, kWaitMs);
        reader->searchBox()->setText(QStringLiteral("findmethird"));
        reader->applySearch();
        QTRY_COMPARE_WITH_TIMEOUT(reader->hitCount(), 1, kWaitMs);
    }

    void bookmarksAreKeptNamedAndFoundAgain()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        m_session->files().setHash(m_session->files().filesFor(id).first().id, QStringLiteral("made-up-hash"));
        ReaderTab *reader = waitForReader(id);
        QVERIFY(reader && reader->isReady());
        PageView *view = reader->pageView();
        view->goToPage(3, 0.5);
        reader->addBookmark();
        QListWidget *list = reader->bookmarkList();
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Page 4"));

        // Renamed in place, and kept.
        list->item(0)->setText(QStringLiteral("Made-up place"));
        const QList<Bookmark> kept = BookmarkRepository(m_session->database()).forContents(QStringLiteral("made-up-hash"));
        QCOMPARE(kept.size(), 1);
        QCOMPARE(kept.first().label, QStringLiteral("Made-up place"));
        QCOMPARE(kept.first().page, 3);
        QVERIFY(kept.first().y > 0);

        // Going there can be come back from.
        view->goToPage(0);
        reader->goToBookmark(kept.first().id);
        QCOMPARE(view->topPage(), 3);
        QVERIFY(view->canGoBack());

        // It's among what Ctrl+G finds.
        m_window->goToAction()->trigger();
        ContentsPopup *popup = reader->contentsPopup();
        QVERIFY(popup);
        QTest::keyClicks(popup->searchBox(), QStringLiteral("made-up"));
        QCOMPARE(popup->list()->topLevelItemCount(), 1);
        QVERIFY(popup->list()->topLevelItem(0)->text(0).contains(QStringLiteral("Made-up place")));
        QTest::keyClick(popup->searchBox(), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(reader->contentsPopup() == nullptr, kWaitMs);

        // Still there next time, and deleted with Delete.
        m_window->closeDocumentTab(1);
        reader = waitForReader(id);
        QVERIFY(reader && reader->isReady());
        list = reader->bookmarkList();
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Made-up place"));
        list->setCurrentRow(0);
        QAction *remove = nullptr;
        for (QAction *action : list->actions()) {
            if (action->shortcut() == QKeySequence(QKeySequence::Delete))
                remove = action;
        }
        QVERIFY(remove);
        remove->trigger();
        QCOMPARE(list->count(), 0);
        QVERIFY(BookmarkRepository(m_session->database()).forContents(QStringLiteral("made-up-hash")).isEmpty());
    }

    void anEpubsBookmarkFollowsItsTextToANewPage()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_nav.epub"));
        m_session->files().setHash(m_session->files().filesFor(id).first().id, QStringLiteral("made-up-epub-hash"));
        ReaderTab *reader = waitForReader(id);
        QVERIFY(reader && reader->isReflowable());
        PageView *view = reader->pageView();
        const int pages = view->pageCount();
        QVERIFY(pages > 3);
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 3, kWaitMs);
        view->goToPage(pages - 1);
        QCOMPARE(view->topPage(), pages - 1);
        reader->addBookmark();
        BookmarkRepository bookmarks(m_session->database());
        QTRY_VERIFY_WITH_TIMEOUT(!bookmarks.forContents(QStringLiteral("made-up-epub-hash")).value(0).place.isEmpty(), kWaitMs);
        const qint64 bookmark = bookmarks.forContents(QStringLiteral("made-up-epub-hash")).first().id;

        // Larger text moves it to a later page, where it's found again.
        QSignalSpy changed(reader->client(), &DocumentClient::layoutChanged);
        reader->changeFontSize(+8);
        QVERIFY(changed.wait(kWaitMs));
        QTRY_VERIFY_WITH_TIMEOUT(reader->bookmarkPage(bookmark) > pages - 1, kWaitMs);
        view->goToPage(0);
        reader->goToBookmark(bookmark);
        QCOMPARE(view->topPage(), reader->bookmarkPage(bookmark));
    }

    void aDocumentOpensWhereItWasLeft()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        {
            ReaderTab *reader = waitForReader(id);
            reader->pageView()->setScale(2.0);
            reader->pageView()->goToPage(4, 0.25);
            m_window->closeDocumentTab(1);
        }
        const auto state = ReadingStateRepository(m_session->database()).get(id);
        QVERIFY(state.has_value());
        QVERIFY(state->lastOpened > 0);

        // And after closing the whole window too.
        m_window.reset();
        openLibraryWindow();
        ReaderTab *reader = waitForReader(id);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->topPage(), 4, kWaitMs);
        QCOMPARE(reader->pageView()->scale(), 2.0);
        QCOMPARE(reader->pageView()->zoomMode(), PageView::ZoomMode::Fixed);
        QVERIFY(qAbs(reader->pageView()->topOffset() - 0.25) < 0.02);
    }

    void anEpubOpensWhereItWasLeftEvenAtAnotherTextSize()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_nav.epub"));
        int pagesAtFirst = 0;
        {
            ReaderTab *reader = waitForReader(id);
            pagesAtFirst = reader->pageView()->pageCount();
            QSignalSpy bookmarked(reader->client(), &DocumentClient::bookmarkReady);
            reader->pageView()->goToPage(pagesAtFirst - 1);
            QTRY_VERIFY_WITH_TIMEOUT(!bookmarked.isEmpty() && bookmarked.last().at(0).toInt() == reader->pageView()->topPage(),
                                     kWaitMs);
            m_window->closeDocumentTab(1);
        }

        // The same text now falls on a later page.
        m_session->settings().setValue(QStringLiteral("reader.font_size"), 20.0);
        ReaderTab *reader = waitForReader(id);
        QVERIFY(reader->pageView()->pageCount() > pagesAtFirst);
        QTRY_VERIFY_WITH_TIMEOUT(reader->pageView()->topPage() > pagesAtFirst - 1, kWaitMs);
    }

    void pagesTurnKeepingEverythingInThePagesOwnPoints()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_doc.pdf"));
        QVERIFY(reader && reader->isReady());
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
        const QSizeF size = view->pageSize(0);
        QVERIFY(size.width() > 0 && size.height() > 0);
        const QPointF inside(size.width() * 0.25, size.height() * 0.1); // Near the top left.

        for (const int turn : {90, 180, 270, 0}) {
            reader->rotateBy(90);
            QCOMPARE(view->rotation(), turn);
            const QRectF shown = view->pageRect(0);
            // A sideways page is as wide as it was tall.
            const double ratio = shown.width() / shown.height();
            const double expected = turn % 180 == 0 ? size.width() / size.height() : size.height() / size.width();
            QVERIFY2(qAbs(ratio - expected) < 0.02, qPrintable(QStringLiteral("%1: %2 vs %3").arg(turn).arg(ratio).arg(expected)));

            // A point on the page goes to the screen and back, landing where turning it would put it.
            view->goToPage(0);
            const QPoint onScreen = view->viewportPointFor(0, inside);
            const QPointF relative((onScreen.x() + view->horizontalScrollBar()->value() - shown.x()) / shown.width(),
                                   (onScreen.y() + view->verticalScrollBar()->value() - shown.y()) / shown.height());
            const QPointF wanted = turn == 90 ? QPointF(0.9, 0.25) : turn == 180 ? QPointF(0.75, 0.9) : turn == 270 ? QPointF(0.1, 0.75)
                                                                                                                       : QPointF(0.25, 0.1);
            QVERIFY2(qAbs(relative.x() - wanted.x()) < 0.02 && qAbs(relative.y() - wanted.y()) < 0.02,
                     qPrintable(QStringLiteral("%1: %2,%3").arg(turn).arg(relative.x()).arg(relative.y())));
            // Each way round, for looking at by eye if asked for.
            if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty()) {
                QTRY_VERIFY_WITH_TIMEOUT(view->isPageLoaded(0), kWaitMs);
                view->viewport()->grab().save(QDir(pictures).filePath(QStringLiteral("turned-%1.png").arg(turn)));
            }

            // Drawing an area there gives the page's own points.
            QSignalSpy chosen(view, &PageView::areaChosen);
            const QRectF area(inside, QSizeF(40, 30));
            QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::ShiftModifier, view->viewportPointFor(0, area.topLeft()));
            QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::ShiftModifier, view->viewportPointFor(0, area.bottomRight()));
            QCOMPARE(chosen.size(), 1);
            const QRectF drawn = chosen.first().at(1).toRectF();
            QVERIFY2(qAbs(drawn.left() - area.left()) < 2 && qAbs(drawn.top() - area.top()) < 2 && qAbs(drawn.width() - area.width()) < 3,
                     qPrintable(QStringLiteral("%1: %2,%3 %4x%5").arg(turn).arg(drawn.x()).arg(drawn.y()).arg(drawn.width()).arg(drawn.height())));
        }

        // Kept with the place for this format, and put back next time.
        reader->rotateBy(-90);
        QCOMPARE(view->rotation(), 270);
        reader->saveState();
        const qint64 id = reader->publicationId();
        QCOMPARE(ReadingStateRepository(m_session->database()).placeFor(id, QStringLiteral("pdf")).value(QStringLiteral("rotation")).toInt(),
                 270);
        m_window->closeDocumentTab(1);
        reader = waitForReader(id);
        QVERIFY(reader && reader->isReady());
        QCOMPARE(reader->pageView()->rotation(), 270);
    }

    void reflowedPagesArentTurned()
    {
        ReaderTab *reader = openSample(QStringLiteral("reader_nav.epub"));
        QVERIFY(reader && reader->isReady());
        QSignalSpy messages(reader, &ReaderTab::statusMessage);
        reader->rotateBy(90);
        QCOMPARE(reader->pageView()->rotation(), 0);
        QCOMPARE(messages.size(), 1);
    }

    void fullScreenShowsOnlyThePagesUntilEscape()
    {
        openLibraryWindow();
        QAction *full = m_window->fullScreenAction();
        QCOMPARE(full->shortcut(), QKeySequence(Qt::Key_F11));
        QVERIFY(!full->isEnabled()); // Nothing to read yet.
        const qint64 first = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        const qint64 second = addLinkedPublication(QStringLiteral("smart_jump.pdf"));
        ReaderTab *left = waitForReader(first);
        QVERIFY(left && left->isReady());
        ReaderTab *right = m_window->openBeside(second, m_window->formatChoices(second).first().id);
        QTRY_VERIFY_WITH_TIMEOUT(right->isReady(), kWaitMs);
        QVERIFY(full->isEnabled());

        // Both sides lose their toolbars, and the window its bars.
        full->trigger();
        QVERIFY(m_window->isFullScreenReading());
        QVERIFY(full->isChecked());
        QVERIFY(left->isChromeHidden() && right->isChromeHidden());
        QVERIFY(!m_window->statusBar()->isVisible());
        QVERIFY(!m_window->documentTabs()->tabBar()->isVisible());
        QCOMPARE(m_window->menuBar()->height(), 0);

        // Escape first stops what it would anyway, such as a selection.
        PageView *view = left->pageView();
        view->setSelection(0, {QRectF(10, 10, 50, 10)}, QStringLiteral("made-up words"));
        QVERIFY(view->hasSelection());
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(!view->hasSelection());
        QVERIFY(m_window->isFullScreenReading());
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(!m_window->isFullScreenReading());
        QVERIFY(!full->isChecked());
        QVERIFY(!left->isChromeHidden() && !right->isChromeHidden());
        QVERIFY(m_window->statusBar()->isVisible());
        QVERIFY(m_window->menuBar()->height() > 0);

        // Going to the library ends it.
        full->trigger();
        QVERIFY(m_window->isFullScreenReading());
        m_window->documentTabs()->setCurrentIndex(0);
        QVERIFY(!m_window->isFullScreenReading());
        QVERIFY(!full->isEnabled());
        QVERIFY(!left->isChromeHidden());
    }

    void twoDocumentsCanBeReadSideBySide()
    {
        openLibraryWindow();
        const qint64 first = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        const qint64 second = addLinkedPublication(QStringLiteral("smart_jump.pdf"));
        ReaderTab *left = waitForReader(first);
        QVERIFY(left && left->isReady());
        QCOMPARE(m_window->lastDocumentTab(), left);

        // From the library, beside the document last read.
        const qint64 secondFile = m_window->formatChoices(second).first().id;
        ReaderTab *right = m_window->openBeside(second, secondFile);
        QVERIFY(right);
        QTRY_VERIFY_WITH_TIMEOUT(right->isReady(), kWaitMs);
        QTabWidget *tabs = m_window->documentTabs();
        QCOMPARE(tabs->count(), 2);
        auto *pair = qobject_cast<ReaderPair *>(tabs->currentWidget());
        QVERIFY(pair);
        QCOMPARE(pair->left(), left);
        QCOMPARE(pair->right(), right);
        QVERIFY(tabs->tabText(1).contains(QStringLiteral(" | ")));
        QVERIFY(left->isInPair() && right->isInPair());
        QVERIFY(left->closeSideButton()->isVisibleTo(left));
        QCOMPARE(m_window->readersIn(pair).size(), 2);
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty()) {
            QTest::qWait(1000);
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("side-by-side.png")));
        }

        // Shortcuts go to the side being read, which is the one last used.
        QCOMPARE(m_window->activeReader(), right);
        left->pageView()->setFocus();
        QTRY_COMPARE_WITH_TIMEOUT(m_window->activeReader(), left, kWaitMs);
        m_window->goToAction()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(left->contentsPopup() != nullptr, kWaitMs);
        QVERIFY(right->contentsPopup() == nullptr);
        left->contentsPopup()->close();

        // Opening either again goes to it where it is.
        QCOMPARE(m_window->openPublicationFile(second, secondFile), right);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(m_window->activeReader(), right);

        // Separate makes two tabs again, and a tab's menu puts them back together.
        std::unique_ptr<QMenu> pairMenu(m_window->documentTabMenu(1));
        pairMenu->findChild<QAction *>(QStringLiteral("separate"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 3, kWaitMs);
        QCOMPARE(tabs->widget(1), left);
        QCOMPARE(tabs->widget(2), right);
        QVERIFY(!left->isInPair());
        std::unique_ptr<QMenu> menu(m_window->documentTabMenu(1));
        QMenu *beside = menu->findChild<QMenu *>(QStringLiteral("showBeside"));
        QVERIFY(beside);
        beside->actions().first()->trigger();
        QCOMPARE(tabs->count(), 2);
        pair = qobject_cast<ReaderPair *>(tabs->widget(1));
        QVERIFY(pair);

        // Closing one side leaves the other as a tab of its own, at its own place.
        right->pageView()->goToPage(2);
        right->closeSideButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<ReaderTab *>(tabs->widget(1)) == left, kWaitMs);
        QCOMPARE(tabs->count(), 2);
        QVERIFY(!left->isInPair());
        const QJsonObject kept = ReadingStateRepository(m_session->database()).placeFor(second, QStringLiteral("pdf"));
        QCOMPARE(kept.value(QStringLiteral("page")).toInt(), 2);
    }

    void theSameDocumentCanBeOnBothSides()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = waitForReader(id);
        QVERIFY(reader && reader->isReady());
        reader->pageView()->goToPage(1);

        std::unique_ptr<QMenu> menu(m_window->documentTabMenu(1));
        menu->findChild<QAction *>(QStringLiteral("again"))->trigger();
        auto *pair = qobject_cast<ReaderPair *>(m_window->documentTabs()->widget(1));
        QVERIFY(pair);
        ReaderTab *again = pair->right();
        QTRY_VERIFY_WITH_TIMEOUT(again->isReady(), kWaitMs);
        QVERIFY(again != reader);
        QCOMPARE(again->publicationId(), id);
        // It opens where the first side was.
        QTRY_COMPARE_WITH_TIMEOUT(again->pageView()->topPage(), 1, kWaitMs);

        // Each keeps its own page, and only the side being read keeps its place for next time.
        again->pageView()->goToPage(4);
        pair->setActive(reader);
        QVERIFY(reader->savesPlace());
        QVERIFY(!again->savesPlace());
        reader->pageView()->goToPage(3);
        m_window->close();
        m_window.reset();
        const QJsonObject kept = ReadingStateRepository(m_session->database()).placeFor(id, QStringLiteral("pdf"));
        QCOMPARE(kept.value(QStringLiteral("page")).toInt(), 3);
    }

    void removingAPublicationClosesOnlyItsSide()
    {
        openLibraryWindow();
        const qint64 first = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        const qint64 second = addLinkedPublication(QStringLiteral("smart_jump.pdf"));
        ReaderTab *left = waitForReader(first);
        QVERIFY(left && left->isReady());
        ReaderTab *right = m_window->openBeside(second, m_window->formatChoices(second).first().id);
        QTRY_VERIFY_WITH_TIMEOUT(right->isReady(), kWaitMs);
        QVERIFY(qobject_cast<ReaderPair *>(m_window->documentTabs()->widget(1)));

        m_window->removePublications({second});
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<ReaderTab *>(m_window->documentTabs()->widget(1)) == left, kWaitMs);
        QCOMPARE(m_window->documentTabs()->count(), 2);
    }

    void eachFormatOpensInItsOwnTabAtItsOwnPlace()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        copied(QStringLiteral("reader_nav.epub"), QStringLiteral("reader_nav.epub"));
        const qint64 other = linkFile(QStringLiteral("reader_nav.epub"));
        mergePublications(m_session->database(), id, other, QStringLiteral("manual"));

        const QList<FileRecord> choices = m_window->formatChoices(id);
        QCOMPARE(choices.size(), 2);
        QCOMPARE(choices.at(0).format, QStringLiteral("pdf"));
        QCOMPARE(choices.at(1).format, QStringLiteral("epub"));

        ReaderTab *pdf = m_window->openPublicationFile(id, choices.at(0).id);
        QTRY_VERIFY_WITH_TIMEOUT(pdf->isReady(), kWaitMs);
        ReaderTab *epub = m_window->openPublicationFile(id, choices.at(1).id);
        QTRY_VERIFY_WITH_TIMEOUT(epub->isReady(), kWaitMs);
        QVERIFY(pdf != epub);
        QVERIFY(!pdf->isReflowable());
        QVERIFY(epub->isReflowable());
        QCOMPARE(m_window->documentTabs()->count(), 3);
        QVERIFY(m_window->documentTabs()->tabText(1).endsWith(QStringLiteral("(PDF)")));
        QVERIFY(m_window->documentTabs()->tabText(2).endsWith(QStringLiteral("(EPUB)")));

        // Going back to a format finds the tab it already has.
        QCOMPARE(m_window->openPublicationFile(id, choices.at(0).id), pdf);
        QCOMPARE(m_window->documentTabs()->count(), 3);

        pdf->pageView()->goToPage(4);
        epub->pageView()->goToPage(8);
        m_window->closeDocumentTab(2);
        m_window->closeDocumentTab(1);

        pdf = m_window->openPublicationFile(id, choices.at(0).id);
        QTRY_COMPARE_WITH_TIMEOUT(pdf->pageView()->topPage(), 4, kWaitMs);
        epub = m_window->openPublicationFile(id, choices.at(1).id);
        QTRY_COMPARE_WITH_TIMEOUT(epub->pageView()->topPage(), 8, kWaitMs);
    }

    void theDetailsOfferEachFormatAndCanSeparateThem()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        copied(QStringLiteral("reader_nav.epub"), QStringLiteral("reader_nav.epub"));
        mergePublications(m_session->database(), id, linkFile(QStringLiteral("reader_nav.epub")), QStringLiteral("manual"));

        m_window->details()->showPublication(id);
        const auto buttonNamed = [&](const QString &text) -> QPushButton * {
            for (QPushButton *button : m_window->details()->findChildren<QPushButton *>()) {
                if (button->text() == text)
                    return button;
            }
            return nullptr;
        };
        QVERIFY(buttonNamed(QStringLiteral("Open PDF")) != nullptr);
        QVERIFY(buttonNamed(QStringLiteral("Open EPUB")) != nullptr);

        buttonNamed(QStringLiteral("Open EPUB"))->click();
        QCOMPARE(m_window->documentTabs()->count(), 2);
        auto *reader = qobject_cast<ReaderTab *>(m_window->documentTabs()->currentWidget());
        QVERIFY(reader != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reader->isReady(), kWaitMs);
        QVERIFY(reader->isReflowable());
        m_window->closeDocumentTab(1);

        QPushButton *separate = buttonNamed(QStringLiteral("Not the same publication: separate the EPUB"));
        QVERIFY(separate != nullptr);
        separate->click();
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), 2, kWaitMs);
        QCOMPARE(m_window->formatChoices(id).size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(buttonNamed(QStringLiteral("Open")) != nullptr && buttonNamed(QStringLiteral("Open"))->isVisibleTo(m_window->details()),
                                 kWaitMs);
    }

    void aCopyThatsGoneIsntOfferedAsAChoice()
    {
        openLibraryWindow();
        const qint64 id = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        const QString epub = copied(QStringLiteral("reader_nav.epub"), QStringLiteral("reader_nav.epub"));
        mergePublications(m_session->database(), id, linkFile(QStringLiteral("reader_nav.epub")), QStringLiteral("manual"));
        QCOMPARE(m_window->formatChoices(id).size(), 2);
        QVERIFY(QFile::remove(epub));
        QCOMPARE(m_window->formatChoices(id).size(), 1);
    }

    void combiningTheSelectionMakesOnePublication()
    {
        openLibraryWindow();
        const qint64 first = addLinkedPublication(QStringLiteral("reader_doc.pdf"));
        copied(QStringLiteral("reader_nav.epub"), QStringLiteral("reader_nav.epub"));
        const qint64 second = linkFile(QStringLiteral("reader_nav.epub"));
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->rowCount(), 2, kWaitMs);

        m_window->gridView()->selectAll();
        QCOMPARE(m_window->selectedCount(), 2);
        m_window->combineSelected();
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->rowCount(), 1, kWaitMs);
        Q_UNUSED(first)
        Q_UNUSED(second)
        const qint64 kept = m_session->query().ids({}, {}).first();
        QCOMPARE(m_session->files().filesFor(kept).size(), 2);
        QCOMPARE(m_window->model()->data(m_window->model()->index(0, PublicationListModel::FormatColumn)).toString().contains(u','), true);
    }

    void readingLeavesTheFileAlone()
    {
        const QString path = copied(QStringLiteral("reader_doc.pdf"), QStringLiteral("to_read.pdf"));
        const QByteArray before = hashOf(path);
        const QDateTime modified = QFileInfo(path).lastModified();

        openLibraryWindow();
        const qint64 id = linkFile(QStringLiteral("to_read.pdf"));
        ReaderTab *reader = waitForReader(id);
        reader->pageView()->goToPage(4);
        reader->searchBox()->setText(QStringLiteral("findme"));
        reader->applySearch();
        QTRY_COMPARE_WITH_TIMEOUT(reader->hitCount(), 3, kWaitMs);
        m_window->closeDocumentTab(1);

        QCOMPARE(hashOf(path), before);
        QCOMPARE(QFileInfo(path).lastModified(), modified);
    }

private:
    QString worker() const { return QStringLiteral(PAPERTRAIL_WORKER_PATH); }

    QString copied(const QString &name, const QString &as)
    {
        const QString folder = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(folder);
        const QString path = QDir(folder).filePath(as);
        QFile::copy(sample(name), path);
        return path;
    }

    void openLibraryWindow()
    {
        if (!m_session) {
            LibraryPaths paths;
            paths.database = m_dir->filePath(QStringLiteral("library.db"));
            paths.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
            paths.workerPath = worker();
            paths.settleMs = 0;
            m_session = std::make_unique<LibrarySession>(paths);
            m_session->settings().setValue(QStringLiteral("library.rescan_on_startup"), false);
        }
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->resize(1200, 800);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    qint64 linkFile(const QString &name)
    {
        const QString folder = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(folder);
        const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(folder));
        qint64 root = 0;
        for (const LibraryRoot &existing : m_session->files().roots())
            root = existing.id;
        if (root == 0)
            root = m_session->files().addRoot(QStringLiteral("Docs"), cleaned);

        FileRecord file;
        file.rootId = root;
        file.relativePath = name;
        file.size = QFileInfo(QDir(folder).filePath(name)).size();
        file.modifiedAt = 1;
        file.format = QFileInfo(name).suffix();
        m_session->files().upsert(file);

        Publication p;
        p.title = name;
        const qint64 id = m_session->publications().create(p);
        m_session->files().link(id, file.id, true);
        return id;
    }

    qint64 addLinkedPublication(const QString &name)
    {
        copied(name, name);
        return linkFile(name);
    }

    ReaderTab *waitForReader(qint64 id)
    {
        ReaderTab *reader = m_window->openPublication(id);
        if (!reader)
            return nullptr;
        for (int waited = 0; waited < kWaitMs && !reader->isReady() && !reader->isShowingProblem(); waited += 10)
            QTest::qWait(10);
        return reader;
    }

    ReaderTab *openSample(const QString &name)
    {
        openLibraryWindow();
        ReaderTab *reader = waitForReader(addLinkedPublication(name));
        if (reader && reader->isReady())
            QTest::qWait(50);
        return reader;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
};

QTEST_MAIN(ReaderTest)
#include "test_reader.moc"
