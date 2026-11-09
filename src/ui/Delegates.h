#pragma once

#include <QStyledItemDelegate>

namespace papertrail::ui {

class CoverCache;

// A red bar with white text along the bottom of a cover, such as "Retracted", and returns its height. Where the
// cover's too small for the words, the bar is drawn alone.
int drawNoticeBar(QPainter *painter, const QRect &cover, const QString &label);

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
// two can't be mistaken for each other. Everything is drawn as plain text. An area drawn over a page shows a
// picture of it in place of the words, or a space for one until it comes.
class AnnotationDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    enum Role {
        HeadingRole = Qt::UserRole + 1,
        QuoteRole,
        NoteRole,
        ColourRole,
        AreaRole,    // Whether it's an area.
        PictureRole, // A QImage of the area, once there is one.
    };

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// One article in an issue's or book's list: its title wrapped to the list's width, then its pages small
// and dimmed beneath, with room around it and a rule between entries. Drawn as plain text.
class ArticleDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        PagesRole,
    };

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace papertrail::ui
