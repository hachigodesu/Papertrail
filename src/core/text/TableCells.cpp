#include "core/text/TableCells.h"

#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <utility>

namespace papertrail::core {

namespace {

// Rules closer than this are one, as a double rule is, and so are the edges of pieces that line up.
constexpr double kLineUp = 3;
// Thicker than this, a shape isn't a rule.
constexpr double kThinnest = 2.5;

struct Piece {
    QString text;
    QRectF rect;
};

// A rule drawn on the page: where it is across or down, and where it runs from and to the other way.
struct Rule {
    double at = 0;
    double from = 0;
    double to = 0;
};

using Span = std::pair<double, double>;

double median(QList<double> values)
{
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    return values.at((values.size() - 1) / 2);
}

// A joining box, put in where there's no character, has no place on the page.
bool hasNoPlace(const QRectF &box)
{
    return box.isNull() || box.width() < 0 || box.height() < 0;
}

// The pieces of the lines inside the area: the characters whose middles are in it, split wherever the gap between
// two is wider than most of the type's size, or a rule runs between them.
QList<Piece> piecesIn(const QList<TableLine> &lines, const QRectF &area, const QList<double> &splitAt)
{
    QList<Piece> pieces;
    for (const TableLine &line : lines) {
        const qsizetype n = qMin(line.text.size(), line.chars.size());
        double size = line.size;
        if (size <= 0) {
            QList<double> heights;
            for (qsizetype i = 0; i < n; ++i) {
                if (!hasNoPlace(line.chars.at(i)))
                    heights << line.chars.at(i).height();
            }
            size = median(heights);
        }
        if (size <= 0)
            size = 10;
        Piece piece;
        bool open = false;
        bool space = false;
        double lastMiddle = 0;
        const auto close = [&] {
            if (open) {
                piece.text = piece.text.trimmed();
                if (!piece.text.isEmpty())
                    pieces << piece;
            }
            piece = Piece{};
            open = false;
            space = false;
        };
        for (qsizetype i = 0; i < n; ++i) {
            const QChar c = line.text.at(i);
            const QRectF box = line.chars.at(i);
            if (hasNoPlace(box) || c.isSpace()) {
                space = open;
                continue;
            }
            if (!area.contains(box.center())) {
                close();
                continue;
            }
            const double middle = box.center().x();
            if (open) {
                const double gap = box.left() - piece.rect.right();
                const bool ruled = std::any_of(splitAt.cbegin(), splitAt.cend(),
                                               [&](double x) { return x > lastMiddle && x < middle; });
                if (gap > 0.8 * size || gap < -2 * size || ruled)
                    close();
            }
            if (!open) {
                piece.rect = box;
                open = true;
            } else {
                if (space || box.left() - piece.rect.right() > 0.3 * size)
                    piece.text += u' ';
                piece.rect = piece.rect.united(box);
            }
            piece.text += c;
            lastMiddle = middle;
            space = false;
        }
        close();
    }
    return pieces;
}

// Where rules lie, those within a few points of each other taken as one, keeping those that together run along at
// least `share` of the length from `from` to `to`, as a table's rules do.
QList<double> rulePositions(QList<Rule> rules, double from, double to, double share)
{
    std::sort(rules.begin(), rules.end(), [](const Rule &a, const Rule &b) { return a.at < b.at; });
    QList<double> kept;
    const double length = to - from;
    qsizetype i = 0;
    while (i < rules.size()) {
        qsizetype j = i + 1;
        while (j < rules.size() && rules.at(j).at - rules.at(j - 1).at <= kLineUp)
            ++j;
        QList<Span> spans;
        double sum = 0;
        for (qsizetype k = i; k < j; ++k) {
            sum += rules.at(k).at;
            const double a = qMax(rules.at(k).from, from);
            const double b = qMin(rules.at(k).to, to);
            if (b > a)
                spans << Span{a, b};
        }
        std::sort(spans.begin(), spans.end());
        double covered = 0;
        double reached = -std::numeric_limits<double>::infinity();
        for (const Span &span : std::as_const(spans)) {
            const double start = qMax(span.first, reached);
            if (span.second > start)
                covered += span.second - start;
            reached = qMax(reached, span.second);
        }
        if (length <= 0 ? !spans.isEmpty() : covered >= share * length)
            kept << sum / double(j - i);
        i = j;
    }
    return kept;
}

// Spans taken together where they overlap or come within a few points of each other.
QList<Span> clustersOf(QList<Span> spans)
{
    std::sort(spans.begin(), spans.end());
    QList<Span> clusters;
    for (const Span &span : std::as_const(spans)) {
        if (!clusters.isEmpty() && span.first <= clusters.last().second + kLineUp)
            clusters.last().second = qMax(clusters.last().second, span.second);
        else
            clusters << span;
    }
    return clusters;
}

// Mostly figures, as a table's numbers are, such as "30 (60%)" or "-3.2": never the second line of a wrapped cell.
bool mostlyFigures(const QString &cell)
{
    int digits = 0;
    for (const QChar c : cell) {
        if (c.isDigit())
            ++digits;
        else if (c.isLetter())
            return false;
    }
    return digits > 0;
}

// A cell's text over two lines, with a word broken at the end of the first joined again.
QString joined(const QString &above, const QString &below)
{
    if (above.isEmpty())
        return below;
    if (below.isEmpty())
        return above;
    if (above.size() >= 2 && above.endsWith(u'-') && above.at(above.size() - 2).isLetter() && below.at(0).isLower())
        return above.chopped(1) + below;
    return above + u' ' + below;
}

} // namespace

QList<QList<int>> rowsOfBoxes(const QList<QRectF> &boxes)
{
    QList<int> order;
    for (int i = 0; i < boxes.size(); ++i) {
        if (!boxes.at(i).isEmpty())
            order << i;
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return boxes.at(a).center().y() < boxes.at(b).center().y(); });
    QList<QList<int>> rows;
    QRectF first;
    for (int i : std::as_const(order)) {
        const QRectF rect = boxes.at(i);
        if (!rows.isEmpty()) {
            const double overlap = qMin(first.bottom(), rect.bottom()) - qMax(first.top(), rect.top());
            if (overlap > qMin(first.height(), rect.height()) * 0.5) {
                rows.last() << i;
                continue;
            }
        }
        rows << QList<int>{i};
        first = rect;
    }
    for (QList<int> &row : rows)
        std::stable_sort(row.begin(), row.end(), [&](int a, int b) { return boxes.at(a).left() < boxes.at(b).left(); });
    return rows;
}

TableGrid tableIn(const QList<TableLine> &lines, const QList<TableDrawing> &drawings, const QRectF &area)
{
    TableGrid grid;
    if (area.isEmpty())
        return grid;
    QList<Piece> pieces = piecesIn(lines, area, {});
    if (pieces.isEmpty())
        return grid;
    QRectF extent;
    for (const Piece &piece : std::as_const(pieces))
        extent = extent.united(piece.rect);

    // The rules: thin shapes across or down, and the sides of rectangles, such as cells' borders.
    QList<Rule> down;
    QList<Rule> across;
    const QRectF near = area.adjusted(-2, -2, 2, 2);
    for (const TableDrawing &drawing : drawings) {
        const QRectF r = drawing.rect;
        if (!r.isValid() || !r.intersects(near))
            continue;
        if (r.width() <= kThinnest && r.height() >= 4 && r.height() > 3 * r.width()) {
            down << Rule{r.center().x(), r.top(), r.bottom()};
        } else if (r.height() <= kThinnest && r.width() >= 4 && r.width() > 3 * r.height()) {
            across << Rule{r.center().y(), r.left(), r.right()};
        } else if (drawing.rectangle && r.width() > kThinnest && r.height() > kThinnest) {
            down << Rule{r.left(), r.top(), r.bottom()} << Rule{r.right(), r.top(), r.bottom()};
            across << Rule{r.top(), r.left(), r.right()} << Rule{r.bottom(), r.left(), r.right()};
        }
    }
    QList<double> allDown;
    for (double x : rulePositions(down, extent.top(), extent.bottom(), 0.4)) {
        if (x >= near.left() && x <= near.right())
            allDown << x;
    }
    QList<double> inside;
    for (double x : std::as_const(allDown)) {
        if (x > extent.left() + 0.5 && x < extent.right() - 0.5)
            inside << x;
    }
    QList<double> rulesAcross;
    for (double y : rulePositions(across, extent.left(), extent.right(), 0.3)) {
        if (y >= near.top() && y <= near.bottom())
            rulesAcross << y;
    }
    grid.ruled = allDown.size() >= 2 && !inside.isEmpty();
    // Characters on both sides of a rule are in different cells, however close.
    if (!inside.isEmpty())
        pieces = piecesIn(lines, area, inside);

    QList<QRectF> boxes;
    for (const Piece &piece : std::as_const(pieces))
        boxes << piece.rect;
    const QList<QList<int>> rows = rowsOfBoxes(boxes);
    if (rows.isEmpty())
        return grid;

    // The columns from how the pieces line up: where the rows with the most pieces leave gaps, then with every piece
    // that's inside one of those columns, which finds a column only some rows have. A piece over several, such as a
    // heading over the columns it heads, doesn't close the gaps between them.
    QList<double> counts;
    for (const QList<int> &row : rows) {
        if (row.size() >= 2)
            counts << double(row.size());
    }
    QList<Span> clusters;
    if (counts.isEmpty()) {
        for (const Piece &piece : std::as_const(pieces))
            clusters << Span{piece.rect.left(), piece.rect.right()};
        clusters = clustersOf(clusters);
    } else {
        const double most = median(counts);
        QList<Span> spans;
        for (const QList<int> &row : rows) {
            if (row.size() >= most) {
                for (int i : row)
                    spans << Span{pieces.at(i).rect.left(), pieces.at(i).rect.right()};
            }
        }
        const QList<Span> first = clustersOf(spans);
        spans.clear();
        for (const Piece &piece : std::as_const(pieces)) {
            const auto over = std::count_if(first.cbegin(), first.cend(), [&](const Span &c) {
                return piece.rect.left() < c.second && piece.rect.right() > c.first;
            });
            if (over <= 1)
                spans << Span{piece.rect.left(), piece.rect.right()};
        }
        clusters = clustersOf(spans);
    }
    QList<double> boundaries = inside;
    for (qsizetype c = 0; c + 1 < clusters.size(); ++c) {
        const double gapFrom = clusters.at(c).second;
        const double gapTo = clusters.at(c + 1).first;
        const bool ruled = std::any_of(inside.cbegin(), inside.cend(), [&](double x) { return x >= gapFrom - 1 && x <= gapTo + 1; });
        if (!ruled)
            boundaries << (gapFrom + gapTo) / 2;
    }
    std::sort(boundaries.begin(), boundaries.end());

    // Each piece in the column holding most of it.
    const auto columnOf = [&](const QRectF &rect) {
        int best = 0;
        double bestOverlap = -std::numeric_limits<double>::infinity();
        for (int k = 0; k <= boundaries.size(); ++k) {
            const double from = k == 0 ? -std::numeric_limits<double>::infinity() : boundaries.at(k - 1);
            const double to = k == boundaries.size() ? std::numeric_limits<double>::infinity() : boundaries.at(k);
            const double overlap = rect.width() > 0 ? qMin(rect.right(), to) - qMax(rect.left(), from)
                                                    : (rect.center().x() >= from && rect.center().x() < to ? 0.0 : -1.0);
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                best = k;
            }
        }
        return best;
    };
    QList<QList<int>> columnsOfRows;
    QList<bool> used(boundaries.size() + 1, false);
    for (const QList<int> &row : rows) {
        QList<int> columns;
        for (int i : row) {
            const int column = columnOf(pieces.at(i).rect);
            columns << column;
            used[column] = true;
        }
        columnsOfRows << columns;
    }
    // Columns that nothing is in are left out.
    QList<int> renumbered(used.size(), -1);
    for (int k = 0; k < used.size(); ++k) {
        if (used.at(k))
            renumbered[k] = grid.columns++;
    }

    QList<QRectF> rowRects;
    for (qsizetype r = 0; r < rows.size(); ++r) {
        QStringList cells(grid.columns);
        QRectF rect;
        for (qsizetype p = 0; p < rows.at(r).size(); ++p) {
            const Piece &piece = pieces.at(rows.at(r).at(p));
            QString &cell = cells[renumbered.at(columnsOfRows.at(r).at(p))];
            cell = cell.isEmpty() ? piece.text : cell + u' ' + piece.text;
            rect = rect.united(piece.rect);
        }
        grid.rows << cells;
        rowRects << rect;
    }

    // Which rows carry on the cells of the row above. Never across a rule. Between rules for each row, everything
    // between two rules is one row. Otherwise, a row whose first cell is empty, under one whose first cell isn't, at
    // the usual spacing, where most rows fill their first cells; or a row with only some of the cells of the one above,
    // set closer to it than rows usually are. Never a row of figures.
    const auto band = [&](qsizetype r) {
        const double middle = rowRects.at(r).center().y();
        return std::count_if(rulesAcross.cbegin(), rulesAcross.cend(), [&](double y) { return y < middle; });
    };
    QList<qsizetype> bands;
    QList<double> pitches;
    int firstFilled = 0;
    for (qsizetype r = 0; r < grid.rows.size(); ++r) {
        if (bands.isEmpty() || bands.last() != band(r))
            bands << band(r);
        if (r > 0)
            pitches << rowRects.at(r).center().y() - rowRects.at(r - 1).center().y();
        firstFilled += grid.rows.at(r).at(0).isEmpty() ? 0 : 1;
    }
    const bool rowsBetweenRules = grid.ruled && rulesAcross.size() >= 2 && bands.size() >= 3 && 2 * bands.size() >= grid.rows.size();
    const double usual = median(pitches);
    grid.continues = QList<bool>(grid.rows.size(), false);
    for (qsizetype r = 1; r < grid.rows.size(); ++r) {
        if (band(r) != band(r - 1))
            continue;
        if (rowsBetweenRules) {
            grid.continues[r] = true;
            continue;
        }
        const QStringList &cells = grid.rows.at(r);
        const QStringList &above = grid.rows.at(r - 1);
        if (grid.columns < 2 || std::any_of(cells.cbegin(), cells.cend(), mostlyFigures))
            continue;
        const double pitch = pitches.at(r - 1);
        const bool firstEmpty = cells.at(0).isEmpty() && (!above.at(0).isEmpty() || grid.continues.at(r - 1))
                                && 2 * firstFilled > grid.rows.size() && pitch <= 1.25 * usual + 0.5;
        bool fewer = pitch <= 0.85 * usual;
        int filled = 0;
        int filledAbove = 0;
        for (int c = 0; c < grid.columns && fewer; ++c) {
            filled += cells.at(c).isEmpty() ? 0 : 1;
            filledAbove += above.at(c).isEmpty() ? 0 : 1;
            if (!cells.at(c).isEmpty() && above.at(c).isEmpty())
                fewer = false;
        }
        grid.continues[r] = firstEmpty || (fewer && filled < filledAbove);
    }
    return grid;
}

QList<QStringList> tableRows(const TableGrid &grid, bool joinLines)
{
    if (!joinLines)
        return grid.rows;
    QList<QStringList> rows;
    for (qsizetype r = 0; r < grid.rows.size(); ++r) {
        if (!rows.isEmpty() && r < grid.continues.size() && grid.continues.at(r)) {
            QStringList &above = rows.last();
            const QStringList &cells = grid.rows.at(r);
            for (qsizetype c = 0; c < above.size() && c < cells.size(); ++c)
                above[c] = joined(above.at(c), cells.at(c));
            continue;
        }
        rows << grid.rows.at(r);
    }
    return rows;
}

QString spreadsheetSafe(const QString &cell)
{
    if (cell.isEmpty())
        return cell;
    const QChar first = cell.at(0);
    if (first != u'=' && first != u'+' && first != u'-' && first != u'@' && first != u'\t' && first != u'\r')
        return cell;
    static const QRegularExpression number(QStringLiteral("^[+-]?(\\d[\\d,]*(\\.\\d*)?|\\.\\d+)([eE][+-]?\\d+)?%?$"));
    if (number.match(cell).hasMatch())
        return cell;
    return u'\'' + cell;
}

QByteArray tableAsCsv(const QList<QStringList> &rows)
{
    QByteArray out("\xEF\xBB\xBF");
    for (const QStringList &row : rows) {
        QStringList fields;
        for (const QString &cell : row) {
            QString field = spreadsheetSafe(cell);
            if (field.contains(u',') || field.contains(u'"') || field.contains(u'\n') || field.contains(u'\r'))
                field = u'"' + field.replace(u'"', QStringLiteral("\"\"")) + u'"';
            fields << field;
        }
        out += fields.join(u',').toUtf8() + "\r\n";
    }
    return out;
}

QString tableAsTsv(const QList<QStringList> &rows)
{
    QStringList lines;
    for (const QStringList &row : rows) {
        QStringList fields;
        for (const QString &cell : row) {
            QString field = spreadsheetSafe(cell);
            field.replace(u'\t', u' ').replace(u'\r', u' ').replace(u'\n', u' ');
            // A field starting with a quote would be read as quoted.
            if (field.startsWith(u'"'))
                field = u'"' + field.replace(u'"', QStringLiteral("\"\"")) + u'"';
            fields << field;
        }
        lines << fields.join(u'\t');
    }
    return lines.join(u'\n') + (lines.isEmpty() ? QString() : QStringLiteral("\n"));
}

QString tableAsHtml(const QList<QStringList> &rows)
{
    QString out = QStringLiteral("<table>");
    for (const QStringList &row : rows) {
        out += QStringLiteral("<tr>");
        for (const QString &cell : row)
            out += QStringLiteral("<td>") + spreadsheetSafe(cell).toHtmlEscaped() + QStringLiteral("</td>");
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table>");
}

} // namespace papertrail::core
