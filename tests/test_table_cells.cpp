// Reading a table from an area of a page, and writing it out for a spreadsheet.

#include "core/text/TableCells.h"

#include <QTest>

using namespace papertrail::core;

namespace {

// A line of 9-point type at this height, from this far across, each character, spaces too, 5 points wide.
TableLine line(const QString &text, double x, double y)
{
    TableLine made;
    made.text = text;
    made.size = 9;
    for (qsizetype i = 0; i < text.size(); ++i)
        made.chars << QRectF(x + 5 * i, y, 5, 9);
    return made;
}

const QRectF kWholePage(0, 0, 595, 842);

using Rows = QList<QStringList>;

} // namespace

class TableCellsTest : public QObject {
    Q_OBJECT

private slots:
    void boxesSideBySideAreARow()
    {
        const QList<QRectF> boxes{QRectF(100, 50, 20, 10), QRectF(10, 52, 20, 10), QRectF(10, 70, 20, 10), QRectF()};
        const QList<QList<int>> rows = rowsOfBoxes(boxes);
        QCOMPARE(rows, (QList<QList<int>>{{1, 0}, {2}}));
    }

    void aTableIsReadFromHowItsWordsLineUp()
    {
        const QList<TableLine> lines{
            line(QStringLiteral("Name"), 50, 100), line(QStringLiteral("Count"), 150, 100), line(QStringLiteral("Mean"), 220, 100),
            line(QStringLiteral("Alpha"), 50, 114), line(QStringLiteral("12"), 150, 114), line(QStringLiteral("-3.2"), 220, 114),
            // A cell with nothing in it.
            line(QStringLiteral("Beta"), 50, 128), line(QStringLiteral("4.5"), 220, 128),
            line(QStringLiteral("Gamma"), 50, 142), line(QStringLiteral("7"), 150, 142), line(QStringLiteral("0.1"), 220, 142),
        };
        const TableGrid grid = tableIn(lines, {}, kWholePage);
        QCOMPARE(grid.columns, 3);
        QVERIFY(!grid.ruled);
        QCOMPARE(grid.rows, (Rows{{QStringLiteral("Name"), QStringLiteral("Count"), QStringLiteral("Mean")},
                                  {QStringLiteral("Alpha"), QStringLiteral("12"), QStringLiteral("-3.2")},
                                  {QStringLiteral("Beta"), QString(), QStringLiteral("4.5")},
                                  {QStringLiteral("Gamma"), QStringLiteral("7"), QStringLiteral("0.1")}}));
        QVERIFY(!grid.continues.contains(true));
    }

    void onlyWhatsInsideTheAreaIsTaken()
    {
        // One line holding a whole row, its cells apart, and running on past the area.
        const QList<TableLine> lines{line(QStringLiteral("First      Second      Not this"), 50, 100),
                                     line(QStringLiteral("One        Two         Nor this"), 50, 114)};
        const TableGrid grid = tableIn(lines, {}, QRectF(40, 90, 120, 40));
        QCOMPARE(grid.rows, (Rows{{QStringLiteral("First"), QStringLiteral("Second")},
                                  {QStringLiteral("One"), QStringLiteral("Two")}}));
        // Empty boxes, which join pieces of a row into one line, are gaps too.
        TableLine joinedRow = line(QStringLiteral("Left Right"), 50, 100);
        joinedRow.chars[4] = QRectF();
        for (qsizetype i = 5; i < joinedRow.chars.size(); ++i)
            joinedRow.chars[i].translate(100, 0);
        QCOMPARE(tableIn({joinedRow, line(QStringLiteral("A"), 50, 114), line(QStringLiteral("B"), 175, 114)}, {}, kWholePage).rows,
                 (Rows{{QStringLiteral("Left"), QStringLiteral("Right")}, {QStringLiteral("A"), QStringLiteral("B")}}));
        QVERIFY(tableIn(lines, {}, QRectF(400, 400, 50, 50)).rows.isEmpty());
        QVERIFY(tableIn(lines, {}, QRectF()).rows.isEmpty());
    }

    void aCellOverTwoLinesIsJoined()
    {
        const QList<TableLine> lines{
            line(QStringLiteral("Item"), 50, 100), line(QStringLiteral("Description"), 150, 100),
            line(QStringLiteral("Bolt"), 50, 114), line(QStringLiteral("A made-up long exam-"), 150, 114),
            line(QStringLiteral("ple of one"), 150, 126),
            line(QStringLiteral("Nut"), 50, 140), line(QStringLiteral("Short"), 150, 140),
        };
        const TableGrid grid = tableIn(lines, {}, kWholePage);
        QCOMPARE(grid.continues, (QList<bool>{false, false, true, false}));
        QCOMPARE(tableRows(grid, true), (Rows{{QStringLiteral("Item"), QStringLiteral("Description")},
                                             {QStringLiteral("Bolt"), QStringLiteral("A made-up long example of one")},
                                             {QStringLiteral("Nut"), QStringLiteral("Short")}}));
        QCOMPARE(tableRows(grid, false).size(), 4);
        QCOMPARE(tableRows(grid, false).at(2), (QStringList{QString(), QStringLiteral("ple of one")}));

        // A label wrapped onto a line of its own, set closer than the rows are, beside numbers on its first line.
        const QList<TableLine> label{
            line(QStringLiteral("First"), 50, 100), line(QStringLiteral("12"), 150, 100),
            line(QStringLiteral("Mean size of"), 50, 114), line(QStringLiteral("3.1"), 150, 114),
            line(QStringLiteral("the measure"), 54, 124),
            line(QStringLiteral("Other"), 50, 138), line(QStringLiteral("1"), 150, 138),
            // Not a label wrapped: as far below as rows are.
            line(QStringLiteral("Heading of section"), 46, 152),
            line(QStringLiteral("Last"), 50, 166), line(QStringLiteral("2"), 150, 166),
        };
        const TableGrid labelled = tableIn(label, {}, kWholePage);
        QCOMPARE(labelled.continues, (QList<bool>{false, false, true, false, false, false}));
        QCOMPARE(tableRows(labelled, true).at(1), (QStringList{QStringLiteral("Mean size of the measure"), QStringLiteral("3.1")}));

        // Never a row of figures, which is a row of its own.
        const QList<TableLine> figures{
            line(QStringLiteral("Group"), 50, 100), line(QStringLiteral("A"), 150, 100), line(QStringLiteral("B"), 200, 100),
            line(QStringLiteral("North"), 50, 114), line(QStringLiteral("1"), 150, 114), line(QStringLiteral("2"), 200, 114),
            line(QStringLiteral("3"), 150, 128), line(QStringLiteral("4"), 200, 128),
            line(QStringLiteral("South"), 50, 142), line(QStringLiteral("5"), 150, 142), line(QStringLiteral("6"), 200, 142),
        };
        QVERIFY(!tableIn(figures, {}, kWholePage).continues.contains(true));
    }

    void aPieceOverTwoColumnsGoesWhereMostOfItIs()
    {
        const QList<TableLine> lines{
            // A heading over the columns it heads, mostly over the first of them.
            line(QStringLiteral("Results of all"), 140, 100),
            line(QStringLiteral("Name"), 50, 114), line(QStringLiteral("A"), 150, 114), line(QStringLiteral("B"), 200, 114),
            line(QStringLiteral("Alpha"), 50, 128), line(QStringLiteral("1"), 150, 128), line(QStringLiteral("2"), 200, 128),
            line(QStringLiteral("Beta"), 50, 142), line(QStringLiteral("3"), 150, 142), line(QStringLiteral("4"), 200, 142),
        };
        const TableGrid grid = tableIn(lines, {}, kWholePage);
        QCOMPARE(grid.columns, 3);
        QCOMPARE(grid.rows.first(), (QStringList{QString(), QStringLiteral("Results of all"), QString()}));
        QCOMPARE(grid.rows.last(), (QStringList{QStringLiteral("Beta"), QStringLiteral("3"), QStringLiteral("4")}));
    }

    void aRuledTableIsReadFromItsRules()
    {
        const QList<TableLine> lines{
            line(QStringLiteral("Alpha"), 42, 95), line(QStringLiteral("Beta"), 102, 95), line(QStringLiteral("Gamma"), 162, 95),
            // A cell over two lines, its first column filled on both.
            line(QStringLiteral("Long first"), 42, 113), line(QStringLiteral("x"), 102, 113), line(QStringLiteral("y"), 162, 113),
            line(QStringLiteral("cell"), 42, 122),
            // Two cells so close that only the rule between them tells them apart.
            line(QStringLiteral("foo bar"), 85, 135), line(QStringLiteral("c"), 162, 135),
        };
        QList<TableDrawing> drawings;
        for (double x : {40.0, 100.0, 160.0, 220.0})
            drawings << TableDrawing{QRectF(x - 0.25, 90, 0.5, 60), false};
        for (double y : {90.0, 110.0, 132.0, 150.0})
            drawings << TableDrawing{QRectF(40, y - 0.25, 180, 0.5), false};
        // A picture's frame elsewhere on the page.
        drawings << TableDrawing{QRectF(300, 300, 100, 100), true};
        const TableGrid grid = tableIn(lines, drawings, kWholePage);
        QVERIFY(grid.ruled);
        QCOMPARE(grid.columns, 3);
        QCOMPARE(grid.continues, (QList<bool>{false, false, true, false}));
        QCOMPARE(tableRows(grid, true), (Rows{{QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")},
                                             {QStringLiteral("Long first cell"), QStringLiteral("x"), QStringLiteral("y")},
                                             {QStringLiteral("foo"), QStringLiteral("bar"), QStringLiteral("c")}}));

        // Cells' borders drawn as a rectangle each do as well.
        QList<TableDrawing> boxes;
        for (double x : {40.0, 100.0, 160.0}) {
            for (double y : {90.0, 110.0, 132.0})
                boxes << TableDrawing{QRectF(x, y, 60, y == 110.0 ? 22 : 18), true};
        }
        const TableGrid boxed = tableIn(lines, boxes, kWholePage);
        QVERIFY(boxed.ruled);
        QCOMPARE(tableRows(boxed, true), tableRows(grid, true));

        // Without its rules, it's read as best it can be from the words alone.
        const TableGrid unruled = tableIn(lines, {}, kWholePage);
        QVERIFY(!unruled.ruled);
        QVERIFY(unruled.rows.last().contains(QStringLiteral("foo bar")));

        // A box round the whole table isn't enough to go by.
        const TableGrid framed = tableIn(lines, {TableDrawing{QRectF(40, 90, 180, 60), true}}, kWholePage);
        QVERIFY(!framed.ruled);
    }

    void cellsAreWrittenForSpreadsheets()
    {
        QCOMPARE(spreadsheetSafe(QStringLiteral("=1+1")), QStringLiteral("'=1+1"));
        QCOMPARE(spreadsheetSafe(QStringLiteral("+44 20 7946 0000")), QStringLiteral("'+44 20 7946 0000"));
        QCOMPARE(spreadsheetSafe(QStringLiteral("@SUM(A1)")), QStringLiteral("'@SUM(A1)"));
        QCOMPARE(spreadsheetSafe(QStringLiteral("-cmd")), QStringLiteral("'-cmd"));
        QCOMPARE(spreadsheetSafe(QStringLiteral("\tx")), QStringLiteral("'\tx"));
        QCOMPARE(spreadsheetSafe(QStringLiteral("\rx")), QStringLiteral("'\rx"));
        QCOMPARE(spreadsheetSafe(QStringLiteral("-")), QStringLiteral("'-"));
        for (const QString &number : {QStringLiteral("-3.2"), QStringLiteral("+5"), QStringLiteral("-1,234.5"), QStringLiteral("-12%"),
                                      QStringLiteral("-.5"), QStringLiteral("-1e-3")})
            QCOMPARE(spreadsheetSafe(number), number);
        QCOMPARE(spreadsheetSafe(QStringLiteral("a=b")), QStringLiteral("a=b"));
        QCOMPARE(spreadsheetSafe(QString()), QString());

        const Rows rows{{QStringLiteral("a,b"), QStringLiteral("say \"hi\""), QStringLiteral("line\nbreak"), QStringLiteral("=1+1"),
                         QStringLiteral("-3.2"), QStringLiteral("café"), QString()},
                        {QStringLiteral("plain")}};
        QCOMPARE(tableAsCsv(rows), QByteArray("\xEF\xBB\xBF\"a,b\",\"say \"\"hi\"\"\",\"line\nbreak\",'=1+1,-3.2,caf\xC3\xA9,\r\nplain\r\n"));
        QCOMPARE(tableAsTsv(rows), QStringLiteral("a,b\tsay \"hi\"\tline break\t'=1+1\t-3.2\tcafé\t\nplain\n"));
        QCOMPARE(tableAsTsv({{QStringLiteral("\"Quoted\" first"), QStringLiteral("a\tb")}}),
                 QStringLiteral("\"\"\"Quoted\"\" first\"\ta b\n"));
        QCOMPARE(tableAsHtml({{QStringLiteral("<b>&"), QStringLiteral("=x")}}),
                 QStringLiteral("<table><tr><td>&lt;b&gt;&amp;</td><td>'=x</td></tr></table>"));
        QCOMPARE(tableAsCsv({}), QByteArray("\xEF\xBB\xBF"));
    }
};

QTEST_GUILESS_MAIN(TableCellsTest)
#include "test_table_cells.moc"
