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

// One entry in a reader's Notes panel: where it is, small and dimmed; the highlighted words, dimmed
// beside a bar in the highlight's colour; then the note in full colour in a box of its own, so the
// two can't be mistaken for each other. Everything is drawn as plain text.
class AnnotationDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    enum Role {
        HeadingRole = Qt::UserRole + 1,
        QuoteRole,
        NoteRole,
        ColourRole,
    };

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace papertrail::ui
