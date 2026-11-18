#pragma once

#include <QByteArray>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

// Reading a table from an area marked on a page, for copying into a spreadsheet. It works from a page's lines with a
// box for each character, and the shapes drawn on it, so it can be tested without opening a document.
namespace papertrail::core {

// One line of a page as the document gives it.
struct TableLine {
    QString text;
    QList<QRectF> chars; // A box for each UTF-16 unit of `text`. An empty box, such as a space put in to join pieces, is a gap.
    double size = 0;     // The type's size in points, or 0 if it isn't known.
};

// Something drawn on the page, which may be one of a table's rules.
struct TableDrawing {
    QRectF rect;
    bool rectangle = false; // A shape that's only a rectangle, whose sides may be a cell's borders.
};

struct TableGrid {
    QList<QStringList> rows; // Top to bottom, each `columns` cells long, a line of text at a time.
    QList<bool> continues;   // Whether each row carries on the cells of the row above it, as a cell over two lines does.
    int columns = 0;
    bool ruled = false; // Its columns came from the rules drawn between them, rather than from how its words line up.
};

// Boxes in rows: those side by side, whose heights overlap the row's first by more than half the smaller's. Each row
// is its boxes' indexes, left to right, and the rows go top to bottom. Empty boxes are left out.
QList<QList<int>> rowsOfBoxes(const QList<QRectF> &boxes);

// The table in `area` of a page: the characters whose middles are inside it, in pieces wherever there's a wide gap,
// in rows, and in columns between the rules drawn across it, or else between the gaps no piece crosses.
TableGrid tableIn(const QList<TableLine> &lines, const QList<TableDrawing> &drawings, const QRectF &area);

// The table's rows, with those that carry on a cell joined to the row above if `joinLines`.
QList<QStringList> tableRows(const TableGrid &grid, bool joinLines);

// A cell as a spreadsheet should be given it: one that would be read as a formula, starting "=", "+", "-", "@", a tab
// or a carriage return, gets an apostrophe in front, unless it's only a number such as "-3.2".
QString spreadsheetSafe(const QString &cell);

// RFC 4180: UTF-8 with a byte order mark so Excel reads accents, CRLF after each row, and a field quoted when it holds
// a comma, a quote or a line break.
QByteArray tableAsCsv(const QList<QStringList> &rows);
// Tab-separated, which spreadsheets paste into cells.
QString tableAsTsv(const QList<QStringList> &rows);
// An HTML table, every cell escaped.
QString tableAsHtml(const QList<QStringList> &rows);

} // namespace papertrail::core
