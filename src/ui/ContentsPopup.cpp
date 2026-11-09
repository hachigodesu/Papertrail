#include "ui/ContentsPopup.h"

#include "core/model/Identifiers.h"

#include <QApplication>
#include <QHeaderView>
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

QStringList foldedWords(const QString &text)
{
    return core::foldedText(text).split(u' ', Qt::SkipEmptyParts);
}

bool hasEvery(const QString &folded, const QStringList &words)
{
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return folded.contains(word); });
}

} // namespace

QList<int> contentsMatches(const QString &query, const QList<OutlineEntry> &entries)
{
    const QStringList words = foldedWords(query);
    QList<int> ownTitle;
    QList<int> withParents;
    for (int i = 0; i < entries.size(); ++i) {
        const QString title = core::foldedText(entries.at(i).title);
        if (hasEvery(title, words))
            ownTitle << i;
        else if (hasEvery(core::foldedText(entries.at(i).parents) + u' ' + title, words))
            withParents << i;
    }
    return ownTitle + withParents;
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
        item->setData(0, kEntryRole, -1);
        item->setData(0, kNumberRole, number);
    }

    const bool searching = !query.isEmpty();
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
        // Text from the file is shown as it is: tree items never treat it as formatting.
        const QString title = searching ? (entry.parents.isEmpty() ? entry.title : entry.parents + QStringLiteral(" › ") + entry.title)
                                        : QString(entry.depth * 4, u' ') + entry.title;
        auto *item = new QTreeWidgetItem(m_list, {title, entry.pageLabel});
        item->setToolTip(0, entry.parents.isEmpty() ? entry.title : entry.parents + QStringLiteral(" › ") + entry.title);
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
    const int width = qBound(260, over->width() - 40, 560);
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
