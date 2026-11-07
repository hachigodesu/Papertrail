#include "ui/GridView.h"

#include <QItemSelection>

namespace papertrail::ui {

GridView::GridView(QWidget *parent)
    : QListView(parent)
{
}

void GridView::setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command)
{
    if (!model() || !selectionModel())
        return;
    const int count = model()->rowCount(rootIndex());
    QItemSelection selection;
    const auto at = [&](int row) { return model()->index(row, modelColumn(), rootIndex()); };
    if (count == 0) {
        selectionModel()->select(selection, command);
        return;
    }

    // The cards all have one size and wrap into rows, so the first row says how many go across.
    const QRect first = visualRect(at(0));
    int columns = 1;
    while (columns < count && visualRect(at(columns)).top() == first.top())
        ++columns;
    const int rows = (count + columns - 1) / columns;
    const QRect area = rect.normalized();

    // The columns the rectangle touches, from the first row's cards.
    int firstColumn = -1;
    int lastColumn = -1;
    for (int column = 0; column < columns; ++column) {
        const QRect card = visualRect(at(column));
        if (card.right() >= area.left() && card.left() <= area.right()) {
            if (firstColumn < 0)
                firstColumn = column;
            lastColumn = column;
        }
    }

    if (firstColumn >= 0) {
        for (int row = 0; row < rows; ++row) {
            const QRect card = visualRect(at(row * columns));
            if (card.bottom() < area.top())
                continue;
            if (card.top() > area.bottom())
                break;
            const int from = row * columns + firstColumn;
            const int to = qMin(row * columns + lastColumn, count - 1); // A last row that isn't full stops short.
            if (from <= to)
                selection.select(at(from), at(to));
        }
    }
    if (selectionBehavior() == QAbstractItemView::SelectRows)
        command |= QItemSelectionModel::Rows;
    selectionModel()->select(selection, command);
}

} // namespace papertrail::ui
