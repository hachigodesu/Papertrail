#pragma once

#include <QListView>

namespace papertrail::ui {

// The library's grid of covers: a list that wraps from left to right, with every card the same size.
class GridView : public QListView {
    Q_OBJECT

public:
    explicit GridView(QWidget *parent = nullptr);

    // Selects what a rectangle dragged across the view, in its viewport's coordinates, would. For tests.
    void selectArea(const QRect &rect) { setSelection(rect, QItemSelectionModel::ClearAndSelect); }

protected:
    // Dragging a rectangle selects every card it touches. Qt's own version misses cards in a last row that
    // isn't full when the rectangle runs past them, so this works it out from the grid's rows and columns.
    void setSelection(const QRect &rect, QItemSelectionModel::SelectionFlags command) override;
};

} // namespace papertrail::ui
