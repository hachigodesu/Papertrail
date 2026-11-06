#pragma once

#include <QStyledItemDelegate>

namespace papertrail::ui {

class CoverCache;

// One card in the grid: the front page, then the title, authors and year.
class GridDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit GridDelegate(CoverCache &covers, QObject *parent = nullptr);

    static QSize cardSize();

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    CoverCache &m_covers;
};

// The list's title column: a small front page beside the title.
class ListTitleDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit ListTitleDelegate(CoverCache &covers, QObject *parent = nullptr);

    static int rowHeight();

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    CoverCache &m_covers;
};

} // namespace papertrail::ui
