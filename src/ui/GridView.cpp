#include "ui/GridView.h"

#include <QApplication>
#include <QDrag>
#include <QItemSelection>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>

namespace papertrail::ui {

namespace {

// A drag starts only from something already chosen, so that pressing elsewhere still chooses by dragging.
void allowDragFrom(QAbstractItemView *view, QMouseEvent *event)
{
    const QModelIndex pressed = view->indexAt(event->position().toPoint());
    const bool chosen = pressed.isValid() && view->selectionModel() && view->selectionModel()->isSelected(pressed);
    view->setDragEnabled(chosen && event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier);
}

qint64 chosenCount(QAbstractItemView *view)
{
    qint64 count = 0;
    if (view->selectionModel()) {
        for (const QItemSelectionRange &range : view->selectionModel()->selection())
            count += range.height();
    }
    return count;
}

} // namespace

void dragChosenPublications(QAbstractItemView *view)
{
    const qint64 count = chosenCount(view);
    if (count == 0)
        return;
    auto *data = new QMimeData;
    data->setData(QString::fromLatin1(kPublicationDragMime), QByteArray::number(count));
    auto *drag = new QDrag(view);
    drag->setMimeData(data);

    // A small label saying how many, since the covers themselves could be thousands.
    const QString text = count == 1 ? QApplication::translate("GridView", "1 publication")
                                    : QApplication::translate("GridView", "%1 publications").arg(count);
    const QFontMetrics metrics(view->font());
    const qreal ratio = view->devicePixelRatioF();
    const QSize size(metrics.horizontalAdvance(text) + 20, metrics.height() + 10);
    QPixmap pixmap(size * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(view->palette().color(QPalette::Highlight));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), 6, 6);
    painter.setPen(view->palette().color(QPalette::HighlightedText));
    painter.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, text);
    painter.end();
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(-12, -12));
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

GridView::GridView(QWidget *parent)
    : QListView(parent)
{
}

void GridView::mousePressEvent(QMouseEvent *event)
{
    allowDragFrom(this, event);
    QListView::mousePressEvent(event);
}

void GridView::startDrag(Qt::DropActions)
{
    dragChosenPublications(this);
}

PublicationTableView::PublicationTableView(QWidget *parent)
    : QTableView(parent)
{
}

void PublicationTableView::mousePressEvent(QMouseEvent *event)
{
    allowDragFrom(this, event);
    QTableView::mousePressEvent(event);
}

void PublicationTableView::startDrag(Qt::DropActions)
{
    dragChosenPublications(this);
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
