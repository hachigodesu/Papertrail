#pragma once

#include <QListView>
#include <QTableView>

namespace papertrail::ui {

// What a drag of publications carries. Only the window's own selection is ever dragged, so where it's dropped
// reads that rather than a list of ids, which for a large selection would be slow to make.
inline constexpr char kPublicationDragMime[] = "application/x-papertrail-publications";

// Starts dragging what's chosen in a view of publications, showing how many there are. Returns once it's
// dropped or abandoned.
void dragChosenPublications(QAbstractItemView *view);

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

    // Pressing on a chosen card and moving drags the choice, as onto a collection. Pressing anywhere else and
    // moving chooses what's dragged across, as before.
    void mousePressEvent(QMouseEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
};

// The library's list, which drags what's chosen in the same way.
class PublicationTableView : public QTableView {
    Q_OBJECT

public:
    explicit PublicationTableView(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
};

} // namespace papertrail::ui
