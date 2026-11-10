#include "ui/ContentsPopup.h"

#include "core/model/Identifiers.h"

#include <QApplication>
#include <QHeaderView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace papertrail::ui {

namespace {

constexpr int kEntryRole = Qt::UserRole + 1; // The entry's index, or -1 for "Go to page".
constexpr int kNumberRole = Qt::UserRole + 2;
constexpr int kTitleRole = Qt::UserRole + 3;
constexpr int kParentsRole = Qt::UserRole + 4; // Shown only while searching.
constexpr int kDepthRole = Qt::UserRole + 5;   // Indented by, while not searching.
constexpr int kIndentPerLevel = 16;
constexpr int kPadding = 4;

QStringList foldedWords(const QString &text)
{
    return core::foldedText(text).split(u' ', Qt::SkipEmptyParts);
}

bool hasEvery(const QString &folded, const QStringList &words)
{
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return folded.contains(word); });
}

// An entry in a list that's easy to scan: what it's inside dimmed before it, its title in full colour with the words
// searched for in bold, and room around it. Everything is drawn as plain text.
class EntryDelegate : public QStyledItemDelegate {
public:
    explicit EntryDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    QStringList words;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.column() != 0) {
            QStyleOptionViewItem dimmed = option;
            initStyleOption(&dimmed, index);
            QColor colour = dimmed.palette.color(QPalette::Text);
            colour.setAlphaF(0.6f);
            dimmed.palette.setColor(QPalette::Text, colour);
            QStyledItemDelegate::paint(painter, dimmed, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString title = index.data(kTitleRole).toString();
        const QString parents = index.data(kParentsRole).toString();
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        painter->save();
        const bool selected = opt.state & QStyle::State_Selected;
        QColor text = opt.palette.color(QPalette::Normal, selected ? QPalette::HighlightedText : QPalette::Text);
        if (!(opt.state & QStyle::State_Enabled))
            text.setAlphaF(0.45f);
        QColor dim = text;
        dim.setAlphaF(text.alphaF() * 0.6f);
        QRect area = opt.rect.adjusted(kPadding + index.data(kDepthRole).toInt() * kIndentPerLevel, 0, -kPadding, 0);
        const QFontMetrics metrics(opt.font);
        if (!parents.isEmpty()) {
            const QString shown = metrics.elidedText(parents + QStringLiteral(" › "), Qt::ElideMiddle, area.width() * 45 / 100);
            painter->setPen(dim);
            painter->setFont(opt.font);
            painter->drawText(area, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, shown);
            area.setLeft(area.left() + metrics.horizontalAdvance(shown));
        }
        const QString elided = metrics.elidedText(title, Qt::ElideRight, area.width());
        QList<QTextLayout::FormatRange> bold;
        for (const auto &[start, length] : matchedRanges(elided, words)) {
            QTextLayout::FormatRange range;
            range.start = start;
            range.length = length;
            range.format.setFontWeight(QFont::Bold);
            bold << range;
        }
        QTextLayout layout(elided, opt.font);
        layout.setFormats(bold);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(area.width() * 2); // Bold can be a little wider: one line regardless.
        layout.endLayout();
        painter->setPen(text);
        layout.draw(painter, QPointF(area.left(), area.top() + (area.height() - line.height()) / 2));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(QFontMetrics(option.font).height() + 2 * kPadding + 2);
        return size;
    }
};

} // namespace

QList<int> contentsMatches(const QString &query, const QList<OutlineEntry> &entries)
{
    const QStringList words = foldedWords(query);
    QList<int> found;
    for (int i = 0; i < entries.size(); ++i) {
        if (hasEvery(core::foldedText(entries.at(i).parents) + u' ' + core::foldedText(entries.at(i).title), words))
            found << i;
    }
    return found;
}

QList<QPair<int, int>> matchedRanges(const QString &text, const QStringList &foldedWords)
{
    // The text folded a character at a time, remembering where each folded character came from.
    QString folded;
    QList<int> from;
    for (int i = 0; i < text.size(); ++i) {
        const QString one = text.at(i).isSpace() ? QStringLiteral(" ") : core::foldedText(QString(text.at(i)));
        for (const QChar c : one) {
            folded += c;
            from << i;
        }
    }
    QList<bool> marked(text.size(), false);
    for (const QString &word : foldedWords) {
        if (word.isEmpty())
            continue;
        for (qsizetype at = folded.indexOf(word); at >= 0; at = folded.indexOf(word, at + 1)) {
            for (qsizetype j = at; j < at + word.size(); ++j)
                marked[from.at(j)] = true;
        }
    }
    QList<QPair<int, int>> ranges;
    for (int i = 0; i < marked.size(); ++i) {
        if (!marked.at(i))
            continue;
        int end = i;
        while (end + 1 < marked.size() && marked.at(end + 1))
            ++end;
        ranges << qMakePair(i, end - i + 1);
        i = end;
    }
    return ranges;
}

ContentsPopup::ContentsPopup(const QList<OutlineEntry> &entries, int current, int shownPages, QWidget *parent)
    : QFrame(parent, Qt::Popup), m_entries(entries), m_current(current), m_shownPages(shownPages)
{
    setObjectName(QStringLiteral("contentsPopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    m_search = new QLineEdit(this);
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(entries.isEmpty() ? tr("This document has no contents. Type a page number.")
                                                   : tr("Search the contents, or type a page number"));
    m_search->setAccessibleName(tr("Go to"));
    m_search->installEventFilter(this);
    // A little larger than the window's own text, since it's read at a glance.
    QFont larger = font();
    larger.setPointSizeF(larger.pointSizeF() * 1.1);
    m_search->setFont(larger);
    layout->addWidget(m_search);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(2);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setIndentation(0);
    m_list->setUniformRowHeights(true);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->setFocusPolicy(Qt::NoFocus); // Typing stays in the search box, which moves through the list itself.
    m_list->setFont(larger);
    m_list->setItemDelegate(new EntryDelegate(m_list));
    layout->addWidget(m_list);

    connect(m_search, &QLineEdit::textChanged, this, &ContentsPopup::refill);
    connect(m_search, &QLineEdit::returnPressed, this, [this] { activate(m_list->currentItem()); });
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) { activate(item); });
    refill();
}

void ContentsPopup::refill()
{
    m_list->clear();
    const QString query = m_search->text().trimmed();

    bool isNumber = false;
    const int number = query.toInt(&isNumber);
    if (isNumber && number >= 1 && number <= m_shownPages) {
        auto *item = new QTreeWidgetItem(m_list, {tr("Go to page %1").arg(number), QString()});
        item->setData(0, kTitleRole, item->text(0));
        item->setData(0, kEntryRole, -1);
        item->setData(0, kNumberRole, number);
    }

    const bool searching = !query.isEmpty();
    static_cast<EntryDelegate *>(m_list->itemDelegate())->words = foldedWords(query);
    QList<int> shown;
    if (searching) {
        shown = contentsMatches(query, m_entries);
    } else {
        for (int i = 0; i < m_entries.size(); ++i)
            shown << i;
    }
    QTreeWidgetItem *current = nullptr;
    for (const int index : std::as_const(shown)) {
        const OutlineEntry &entry = m_entries.at(index);
        // Text from the file is shown as it is: it's drawn as plain text, and never treated as formatting.
        const QString path = entry.parents.isEmpty() ? entry.title : entry.parents + QStringLiteral(" › ") + entry.title;
        auto *item = new QTreeWidgetItem(m_list, {searching ? path : entry.title, entry.pageLabel});
        item->setToolTip(0, path);
        item->setData(0, kTitleRole, entry.title);
        item->setData(0, kParentsRole, searching ? entry.parents : QString());
        item->setData(0, kDepthRole, searching ? 0 : entry.depth);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, kEntryRole, index);
        if (entry.page < 0)
            item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        if (!searching && index == m_current)
            current = item;
    }
    if (!current) {
        for (int row = 0; row < m_list->topLevelItemCount() && !current; ++row) {
            if (m_list->topLevelItem(row)->flags() & Qt::ItemIsEnabled)
                current = m_list->topLevelItem(row);
        }
    }
    m_list->setCurrentItem(current);
    if (current)
        m_list->scrollToItem(current, QAbstractItemView::PositionAtCenter);
}

void ContentsPopup::activate(QTreeWidgetItem *item)
{
    if (!item || !(item->flags() & Qt::ItemIsEnabled))
        return;
    const int index = item->data(0, kEntryRole).toInt();
    if (index < 0) {
        const int number = item->data(0, kNumberRole).toInt();
        close();
        emit pageChosen(number);
        return;
    }
    const OutlineEntry entry = m_entries.at(index);
    close();
    emit entryChosen(entry.page, entry.y);
}

void ContentsPopup::moveBy(int rows)
{
    const int count = m_list->topLevelItemCount();
    if (count == 0)
        return;
    int row = m_list->currentItem() ? m_list->indexOfTopLevelItem(m_list->currentItem()) : -1;
    for (int step = 0; step < count; ++step) {
        row = qBound(0, row + (rows > 0 ? 1 : -1), count - 1);
        QTreeWidgetItem *item = m_list->topLevelItem(row);
        if (item->flags() & Qt::ItemIsEnabled) {
            m_list->setCurrentItem(item);
            return;
        }
        if (row == 0 || row == count - 1)
            return;
    }
}

bool ContentsPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down:
            moveBy(1);
            return true;
        case Qt::Key_Up:
            moveBy(-1);
            return true;
        case Qt::Key_PageDown:
            for (int i = 0; i < 10; ++i)
                moveBy(1);
            return true;
        case Qt::Key_PageUp:
            for (int i = 0; i < 10; ++i)
                moveBy(-1);
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void ContentsPopup::popUpOver(QWidget *over)
{
    const int width = qBound(300, over->width() - 40, 760);
    const int height = qBound(160, over->height() * 6 / 10, 520);
    resize(width, height);
    QPoint place = over->mapToGlobal(QPoint((over->width() - width) / 2, 12));
    if (const QScreen *screen = over->screen()) {
        const QRect available = screen->availableGeometry();
        place.setX(qBound(available.left(), place.x(), qMax(available.left(), available.right() - width)));
        place.setY(qBound(available.top(), place.y(), qMax(available.top(), available.bottom() - height)));
    }
    move(place);
    show();
    m_search->setFocus();
}

} // namespace papertrail::ui
