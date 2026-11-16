// Books downloaded as a zip of PDFs: which zips are books, the order of their parts, and what moves with its part
// when the book's stitched again in another order. Every name and title here is made up.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/ReadingStateRepository.h"
#include "core/repo/StitchRepository.h"
#include "import/Restitch.h"
#include "import/Stitching.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace papertrail;
using namespace papertrail::import;

namespace {

ZipPart part(const QString &name, int pages, const QList<PrintedNumber> &numbers = {}, const QString &heading = {})
{
    ZipPart p;
    p.name = name;
    p.pageCount = pages;
    p.sha256 = QString(QStringLiteral("%1")).arg(qHash(name), 64, 16, QLatin1Char('0')).right(64);
    p.numbers = numbers;
    p.heading = heading;
    return p;
}

PrintedNumber number(int page, int value, bool roman = false)
{
    PrintedNumber n;
    n.page = page;
    n.value = value;
    n.roman = roman;
    return n;
}

QStringList namesOf(const StitchPlan &plan)
{
    QStringList names;
    for (const PlacedPart &placed : plan.parts)
        names << placed.part.name;
    return names;
}

OrderedPart ordered(const QString &name, int pages, bool included = true)
{
    OrderedPart p;
    p.name = name;
    p.sha256 = QString(64, u'0');
    p.pageCount = pages;
    p.included = included;
    p.title = name;
    return p;
}

const QString kBook = QStringLiteral("_2011_Made-Up-Network-Machines.pdf");

} // namespace

class StitchingTest : public QObject {
    Q_OBJECT

private slots:
    void onlyZipsOfSeveralPdfsAreBooks()
    {
        QStringList pdfs;
        QVERIFY(isBookZip({QStringLiteral("Front-matter") + kBook, QStringLiteral("Chapter-1---Starting") + kBook,
                           QStringLiteral("__MACOSX/._Chapter-1.pdf"), QStringLiteral(".DS_Store"), QStringLiteral("parts/")},
                          &pdfs));
        QCOMPARE(pdfs.size(), qsizetype(2));
        QVERIFY(!isBookZip({QStringLiteral("paper.pdf"), QStringLiteral("data.csv")}));
        QVERIFY(!isBookZip({QStringLiteral("paper.pdf")}));
        QVERIFY(!isBookZip({}));
    }

    void printedNumbersAreFoundAloneOrInARunningHead()
    {
        int value = 0;
        bool roman = false;
        QVERIFY(printedNumberIn(QStringLiteral("57"), value, roman));
        QCOMPARE(value, 57);
        QVERIFY(!roman);
        QVERIFY(printedNumberIn(QStringLiteral("xii"), value, roman));
        QCOMPARE(value, 12);
        QVERIFY(roman);
        QVERIFY(printedNumberIn(QStringLiteral("2   CHAPTER 1 Starting out"), value, roman));
        QCOMPARE(value, 2);
        QVERIFY(printedNumberIn(QStringLiteral("Starting out   3"), value, roman));
        QCOMPARE(value, 3);
        // Numbers that aren't the page's.
        QVERIFY(!printedNumberIn(QStringLiteral("Figure 3"), value, roman));
        QVERIFY(!printedNumberIn(QStringLiteral("© 2011 Made-Up Press"), value, roman));
        QVERIFY(!printedNumberIn(QStringLiteral("doi:10.1000/182"), value, roman));
        QVERIFY(!printedNumberIn(QStringLiteral("A sentence that runs across the page and happens to end in 4"), value, roman));
    }

    void printedNumbersDecideTheOrder()
    {
        // Stored out of order, as zips often are.
        const QList<ZipPart> parts{
            part(QStringLiteral("Chapter-2---Going-on") + kBook, 2, {number(0, 4), number(1, 5)}),
            part(QStringLiteral("Front-matter") + kBook, 2, {number(0, 1, true), number(1, 2, true)}),
            part(QStringLiteral("Chapter-10---Ending") + kBook, 2, {number(0, 6), number(1, 7)}),
            part(QStringLiteral("Chapter-1---Starting") + kBook, 3, {number(0, 1), number(2, 3)}),
        };
        const StitchPlan plan = stitchOrder(parts);
        QCOMPARE(namesOf(plan), (QStringList{parts[1].name, parts[3].name, parts[0].name, parts[2].name}));
        QVERIFY(plan.certain);
        QCOMPARE(plan.unplaced, 0);
        QCOMPARE(plan.parts[0].reason, QStringLiteral("Printed pages i–ii"));
        QCOMPARE(plan.parts[1].reason, QStringLiteral("Printed pages 1–3"));
    }

    void anOffsetNumberGivesTheStart()
    {
        // Only a later page has a number, so its first page's is worked out from it.
        const QList<ZipPart> parts{
            part(QStringLiteral("Chapter-2---Going-on") + kBook, 3, {number(2, 12)}),
            part(QStringLiteral("Chapter-1---Starting") + kBook, 9, {number(1, 2), number(8, 9)}),
        };
        const StitchPlan plan = stitchOrder(parts);
        QCOMPARE(namesOf(plan), (QStringList{parts[1].name, parts[0].name}));
        QVERIFY(plan.parts[1].hasStart);
        QCOMPARE(plan.parts[1].start, 10);
    }

    void namesOrderPartsWithoutNumbers()
    {
        const QList<ZipPart> parts{
            part(QStringLiteral("Index") + kBook, 2),
            part(QStringLiteral("Chapter-10---Ending") + kBook, 2),
            part(QStringLiteral("Preface") + kBook, 1),
            part(QStringLiteral("Chapter-9---Nearly-there") + kBook, 2),
            part(QStringLiteral("Copyright") + kBook, 1),
            part(QStringLiteral("Front-matter") + kBook, 1),
            part(QStringLiteral("Appendix-A---Tables") + kBook, 1),
        };
        const StitchPlan plan = stitchOrder(parts);
        QCOMPARE(namesOf(plan), (QStringList{parts[5].name, parts[4].name, parts[2].name, parts[3].name, parts[1].name,
                                             parts[6].name, parts[0].name}));
        QVERIFY(!plan.certain);
        QCOMPARE(plan.unplaced, 0);
        QVERIFY(plan.parts[0].reason.startsWith(QStringLiteral("By its name")));
    }

    void aPartNothingPlacesGoesLast()
    {
        const QList<ZipPart> parts{
            part(QStringLiteral("No-title-available") + kBook, 1),
            part(QStringLiteral("Chapter-1---Starting") + kBook, 2, {number(0, 1), number(1, 2)}),
            part(QStringLiteral("Chapter-2---Going-on") + kBook, 2, {number(0, 3), number(1, 4)}),
        };
        const StitchPlan plan = stitchOrder(parts);
        QCOMPARE(namesOf(plan).last(), parts[0].name);
        QVERIFY(!plan.parts.last().placed);
        QCOMPARE(plan.unplaced, 1);
        QVERIFY(!plan.certain);
        QCOMPARE(plan.parts.last().reason, QStringLiteral("Couldn't tell where this goes"));
    }

    void aNumberedUntitledPartIsPlacedByItsNumbers()
    {
        const QList<ZipPart> parts{
            part(QStringLiteral("No-title-available") + kBook, 2, {number(0, 5), number(1, 6)}),
            part(QStringLiteral("Chapter-1---Starting") + kBook, 4, {number(0, 1), number(3, 4)}),
        };
        const StitchPlan plan = stitchOrder(parts);
        QCOMPARE(namesOf(plan), (QStringList{parts[1].name, parts[0].name}));
        QVERIFY(plan.certain);
    }

    void aGapLeavesTheOrderUncertain()
    {
        const QList<ZipPart> parts{
            part(QStringLiteral("Chapter-1---Starting") + kBook, 2, {number(0, 1), number(1, 2)}),
            part(QStringLiteral("Chapter-3---Later") + kBook, 2, {number(0, 20), number(1, 21)}),
        };
        const StitchPlan plan = stitchOrder(parts);
        QCOMPARE(namesOf(plan), (QStringList{parts[0].name, parts[1].name}));
        QVERIFY(!plan.certain);
    }

    void partTitlesAreTidied()
    {
        ZipPart cut = part(QStringLiteral("Chapter-3---Requirements-of-made-up-syst") + kBook, 1);
        cut.heading = QStringLiteral("Requirements of made-up systems");
        QCOMPARE(partTitle(cut), QStringLiteral("Chapter 3 – Requirements of made-up systems"));

        ZipPart capitals = part(QStringLiteral("Chapter-4---Wiring-the-machi") + kBook, 1);
        capitals.heading = QStringLiteral("WIRING THE MACHINES");
        QCOMPARE(partTitle(capitals), QStringLiteral("Chapter 4 – Wiring the machines"));

        ZipPart untitled = part(QStringLiteral("No-title-available") + kBook, 1);
        untitled.heading = QStringLiteral("Index");
        QCOMPARE(partTitle(untitled), QStringLiteral("Index"));
        QCOMPARE(partTitle(part(QStringLiteral("No-title-available") + kBook, 1)), QStringLiteral("Untitled part"));
        QCOMPARE(partTitle(part(QStringLiteral("Preface") + kBook, 1)), QStringLiteral("Preface"));
    }

    void theBooksTitleAndYearComeFromTheNames()
    {
        const BookFromNames book = bookFromNames({QStringLiteral("Front-matter_2011_Made-Up-Network-Machines.pdf"),
                                                  QStringLiteral("Chapter-1---Starting_2011_Made-Up-Network-Mach.pdf"),
                                                  QStringLiteral("Index_2011_Made-Up-Network-Machines.pdf")});
        QCOMPARE(book.title, QStringLiteral("Made Up Network Machines"));
        QCOMPARE(book.year, 2011);
        QCOMPARE(bookFromNames({QStringLiteral("one.pdf"), QStringLiteral("two.pdf")}).year, 0);
    }

    void anOrderIsKeptAsJson()
    {
        const QList<OrderedPart> order{ordered(QStringLiteral("a.pdf"), 2), ordered(QStringLiteral("b.pdf"), 3, false)};
        const QList<OrderedPart> back = orderFromJson(orderToJson(order));
        QCOMPARE(back.size(), qsizetype(2));
        QCOMPARE(back[1].name, QStringLiteral("b.pdf"));
        QCOMPARE(back[1].pageCount, 3);
        QVERIFY(!back[1].included);
        QCOMPARE(orderKey(back), orderKey(order));
        QVERIFY(orderFromJson("not json").isEmpty());

        // A part left out doesn't change what's stitched, but another order does, and makes another stitched file,
        // beside the zip's converted copy's usual place.
        QList<OrderedPart> allIn = order;
        allIn[1].included = true;
        QCOMPARE(orderKey({order[1], order[0]}), orderKey(order));
        const QList<OrderedPart> swapped{allIn[1], allIn[0]};
        QVERIFY(orderKey(swapped) != orderKey(allIn));
        QVERIFY(orderKey(swapped) != orderKey(order));
        const QString converted = QStringLiteral("/cache/converted/ab/abc.pdf");
        QCOMPARE(orderPathFor(converted), QStringLiteral("/cache/converted/ab/abc.order.json"));
        QVERIFY(stitchedPathFor(converted, order).startsWith(QStringLiteral("/cache/converted/ab/abc-")));
        QVERIFY(stitchedPathFor(converted, order) != stitchedPathFor(converted, swapped));
    }

    void pagesMapByPartAndPageWithinIt()
    {
        const QList<OrderedPart> from{ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3), ordered(QStringLiteral("c"), 1)};
        const QList<OrderedPart> to{ordered(QStringLiteral("c"), 1), ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3, false)};
        QCOMPARE(pageMapping(from, to), (QList<int>{1, 2, -1, -1, -1, 0}));
        QString name;
        int page = 0;
        QVERIFY(partOfPage(from, 3, name, page));
        QCOMPARE(name, QStringLiteral("b"));
        QCOMPARE(page, 1);
        QVERIFY(!partOfPage(from, 6, name, page));
        QCOMPARE(pageOfPart(to, QStringLiteral("a"), 1), 2);
        QCOMPARE(pageOfPart(to, QStringLiteral("b"), 0), -1);
    }

    // ---- Stitching again, in a library.

    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_db = std::make_unique<core::Database>(m_dir->filePath(QStringLiteral("library.db")));
        core::migrateLibrary(*m_db);
    }

    void cleanup()
    {
        m_db.reset();
        m_dir.reset();
    }

    void marksMoveWithTheirParts()
    {
        const qint64 book = addBook();
        const QList<OrderedPart> first{ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3), ordered(QStringLiteral("c"), 1)};
        core::StitchRepository(*m_db).set(m_sha, orderToJson(first), false);
        addHighlight(book, 3);                 // b's second page.
        addBookmark(book, 5);                  // c's only page.
        core::ReadingStateRepository(*m_db).setPlaceFor(book, QStringLiteral("zip"), QJsonObject{{QStringLiteral("page"), 4}}, 1);
        const qint64 chapter = addChapter(book, 3, 5); // From b's first page to its last, counting from 1.

        const QList<OrderedPart> second{ordered(QStringLiteral("c"), 1), ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3)};
        const RestitchCounts counts = applyStitchOrder(*m_db, m_sha, second, true);
        QCOMPARE(counts.moved, 2);
        QCOMPARE(counts.setAside, 0);
        QCOMPARE(highlightPage(), 4);
        QCOMPARE(scalar("SELECT page FROM bookmark"), qint64(0));
        QCOMPARE(core::ReadingStateRepository(*m_db).placeFor(book, QStringLiteral("zip")).value(QStringLiteral("page")).toInt(), 5);
        QCOMPARE(scalar(QStringLiteral("SELECT page_start FROM publication WHERE id = %1").arg(chapter).toUtf8()), qint64(4));
        QCOMPARE(scalar(QStringLiteral("SELECT page_end FROM publication WHERE id = %1").arg(chapter).toUtf8()), qint64(6));
        const std::optional<core::StitchOrder> kept = core::StitchRepository(*m_db).get(m_sha);
        QVERIFY(kept && kept->confirmed);
        QCOMPARE(orderKey(orderFromJson(kept->partsJson)), orderKey(second));
        QCOMPARE(scalar("SELECT page_count FROM file"), qint64(6));
    }

    void marksOnPartsLeftOutComeBackWithThem()
    {
        const qint64 book = addBook();
        const QList<OrderedPart> all{ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3), ordered(QStringLiteral("c"), 1)};
        core::StitchRepository(*m_db).set(m_sha, orderToJson(all), true);
        addHighlight(book, 3); // On b.
        addBookmark(book, 0);  // On a.
        addBookmark(book, 2);  // On b.

        const QList<OrderedPart> withoutB{ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3, false), ordered(QStringLiteral("c"), 1)};
        QCOMPARE(marksLeftOut(*m_db, m_sha, withoutB), 2);
        RestitchCounts counts = applyStitchOrder(*m_db, m_sha, withoutB, true);
        QCOMPARE(counts.setAside, 2);
        QCOMPARE(scalar("SELECT count(*) FROM annotation"), qint64(0));
        QCOMPARE(scalar("SELECT count(*) FROM bookmark"), qint64(1));
        QCOMPARE(scalar("SELECT page_count FROM file"), qint64(3));

        // Back, first in the book this time.
        const QList<OrderedPart> bFirst{ordered(QStringLiteral("b"), 3), ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("c"), 1)};
        counts = applyStitchOrder(*m_db, m_sha, bFirst, true);
        QCOMPARE(counts.broughtBack, 2);
        QCOMPARE(highlightPage(), 1);
        QCOMPARE(scalar("SELECT count(*) FROM bookmark"), qint64(2));
        QCOMPARE(scalar("SELECT min(page) FROM bookmark"), qint64(0));  // b's first page, now the book's first.
        QCOMPARE(scalar("SELECT max(page) FROM bookmark"), qint64(3));  // a's first, after b's three.
        // Searches find the highlight again, as its triggers keep them.
        QCOMPARE(scalar("SELECT count(*) FROM annotation_fts WHERE annotation_fts MATCH 'stitched'"), qint64(1));
        QCOMPARE(core::StitchRepository(*m_db).get(m_sha)->setAsideJson,
                 QByteArray("{\"annotations\":[],\"bookmarks\":[]}"));
    }

    void marksOfAPublicationGoneSinceStayGone()
    {
        const qint64 book = addBook();
        const QList<OrderedPart> all{ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3)};
        core::StitchRepository(*m_db).set(m_sha, orderToJson(all), true);
        addHighlight(book, 3);
        applyStitchOrder(*m_db, m_sha, {ordered(QStringLiteral("a"), 2), ordered(QStringLiteral("b"), 3, false)}, true);
        m_db->exec("DELETE FROM publication");
        const RestitchCounts counts = applyStitchOrder(*m_db, m_sha, all, true);
        QCOMPARE(counts.broughtBack, 0);
        QCOMPARE(scalar("SELECT count(*) FROM annotation"), qint64(0));
    }

    void unconfirmedOrdersAreListed()
    {
        const qint64 book = addBook();
        core::StitchRepository orders(*m_db);
        orders.set(m_sha, orderToJson({ordered(QStringLiteral("a"), 1), ordered(QStringLiteral("b"), 1)}), false);
        QCOMPARE(orders.unconfirmed(), QList<QString>{m_sha});
        QCOMPARE(orders.unconfirmedFor(book), std::optional<QString>(m_sha));
        QCOMPARE(orders.publicationFor(m_sha), std::optional<qint64>(book));
        orders.setConfirmed(m_sha, true);
        QVERIFY(orders.unconfirmed().isEmpty());
        QVERIFY(!orders.unconfirmedFor(book));
    }

    void skippedZipsAreKnownUntilTheyChange()
    {
        const qint64 root = core::FileRepository(*m_db).addRoot(QStringLiteral("Books"), m_dir->filePath(QStringLiteral("books")));
        core::StitchRepository orders(*m_db);
        orders.skipZip(root, QStringLiteral("data.zip"), 100, 5);
        QVERIFY(orders.isSkippedZip(root, QStringLiteral("data.zip"), 100, 5));
        QVERIFY(!orders.isSkippedZip(root, QStringLiteral("data.zip"), 101, 5));
        QVERIFY(!orders.isSkippedZip(root, QStringLiteral("data.zip"), 100, 6));
        QVERIFY(!orders.isSkippedZip(root, QStringLiteral("other.zip"), 100, 5));
    }

    void theMigrationRunsAgain()
    {
        core::StitchRepository(*m_db).set(m_sha, orderToJson({ordered(QStringLiteral("a"), 1)}), true);
        m_db->exec("PRAGMA user_version = 22");
        core::migrateLibrary(*m_db);
        QVERIFY(core::StitchRepository(*m_db).get(m_sha).has_value());
    }

private:
    qint64 addBook()
    {
        core::Publication p;
        p.title = QStringLiteral("Made-Up Network Machines");
        p.type = core::PublicationType::Book;
        const qint64 id = core::PublicationRepository(*m_db).create(p);
        core::FileRepository files(*m_db);
        const qint64 root = files.addRoot(QStringLiteral("Books"), m_dir->filePath(QStringLiteral("books")));
        auto insert = m_db->prepare("INSERT INTO file (root_id, relative_path, file_name, size, modified_at, sha256, format, status, page_count) "
                                    "VALUES (?, 'book.zip', 'book.zip', 10, 1, ?, 'zip', 'ok', 6)");
        insert->bind(1, root);
        insert->bind(2, m_sha);
        insert->step();
        files.link(id, m_db->lastInsertId(), true);
        return id;
    }

    qint64 addChapter(qint64 book, int start, int end)
    {
        core::Publication chapter;
        chapter.title = QStringLiteral("A Made-Up Chapter");
        chapter.type = core::PublicationType::BookChapter;
        chapter.parentId = book;
        chapter.pageStart = start;
        chapter.pageEnd = end;
        return core::PublicationRepository(*m_db).create(chapter);
    }

    void addHighlight(qint64 publication, int page)
    {
        const QJsonObject location{{QStringLiteral("parts"),
                                    QJsonArray{QJsonObject{{QStringLiteral("page"), page},
                                                           {QStringLiteral("rects"), QJsonArray{QJsonArray{1, 2, 3, 4}}},
                                                           {QStringLiteral("text"), QStringLiteral("stitched words")}}}}};
        auto insert = m_db->prepare("INSERT INTO annotation (uuid, publication_id, content_hash, location_json, type, colour, "
                                    "selected_text, note, needs_review, created_at, updated_at) "
                                    "VALUES (?, ?, ?, ?, 'highlight', 'yellow', 'stitched words', NULL, 0, 1, 1)");
        insert->bind(1, QStringLiteral("annotation-%1").arg(++m_uuids));
        insert->bind(2, publication);
        insert->bind(3, m_sha);
        insert->bind(4, QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact)));
        insert->step();
    }

    void addBookmark(qint64 publication, int page)
    {
        auto insert = m_db->prepare("INSERT INTO bookmark (uuid, publication_id, content_hash, format, page, y, label, created_at, updated_at) "
                                    "VALUES (?, ?, ?, 'zip', ?, 0, 'A mark', 1, 1)");
        insert->bind(1, QStringLiteral("bookmark-%1").arg(++m_uuids));
        insert->bind(2, publication);
        insert->bind(3, m_sha);
        insert->bind(4, page);
        insert->step();
    }

    int highlightPage()
    {
        auto select = m_db->prepare("SELECT location_json FROM annotation");
        if (!select->step())
            return -1;
        return QJsonDocument::fromJson(select->textAt(0).toUtf8())
            .object()
            .value(QStringLiteral("parts"))
            .toArray()
            .first()
            .toObject()
            .value(QStringLiteral("page"))
            .toInt();
    }

    qint64 scalar(const QByteArray &sql)
    {
        auto select = m_db->prepare(sql);
        select->step();
        return select->int64At(0);
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<core::Database> m_db;
    const QString m_sha = QString(64, u'7');
    int m_uuids = 0;
};

QTEST_GUILESS_MAIN(StitchingTest)
#include "test_stitching.moc"
