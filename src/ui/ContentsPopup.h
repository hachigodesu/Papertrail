#pragma once

#include <QFrame>
#include <QList>
#include <QString>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

// One entry in a document's contents, as the popup lists it.
struct OutlineEntry {
    QString title;
    QString parents;   // The entries it's inside, such as "Chapter Two › Part One", or empty at the top.
    int depth = 0;
    int page = -1;     // In the file, counting from 0. -1 if it doesn't lead anywhere.
    double y = 0;      // In points from the top of the page.
    QString pageLabel; // As the reader numbers it, such as "12". Empty if it doesn't lead anywhere.
};

// The entries a search finds, by index: those with every word of it in their title or the titles they're inside,
// ignoring case and accents, in any order. Those with every word in their own title come first, then in order.
QList<int> contentsMatches(const QString &query, const QList<OutlineEntry> &entries);

// A document's contents over its pages, searched as you type, for going somewhere without the side panel. Typing a
// page number offers that page too. It closes when something's chosen, on Escape, or on a click elsewhere.
class ContentsPopup : public QFrame {
    Q_OBJECT

public:
    // `current` is the entry to start on, or -1. Page numbers from 1 to `shownPages` can be typed.
    ContentsPopup(const QList<OutlineEntry> &entries, int current, int shownPages, QWidget *parent = nullptr);

    // Shows it over the top of `over`, with the cursor in its search box.
    void popUpOver(QWidget *over);

    QLineEdit *searchBox() const { return m_search; }
    QTreeWidget *list() const { return m_list; }

signals:
    void entryChosen(int page, double y);
    void pageChosen(int number); // As typed, from 1.

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refill();
    void activate(QTreeWidgetItem *item);
    void moveBy(int rows);

    QList<OutlineEntry> m_entries;
    int m_current = -1;
    int m_shownPages = 0;
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_list = nullptr;
};

} // namespace papertrail::ui
