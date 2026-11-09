#pragma once

#include "ui/ReaderTab.h"

#include <QPointer>
#include <QWidget>

class QSplitter;

namespace papertrail::ui {

// Two documents side by side in one tab, each with its own reader and its own process. The one last used is the
// active one, which keyboard shortcuts such as Ctrl+F and Ctrl+G go to. The same document can be on both sides,
// each at its own page, in which case only the active side keeps its place for next time.
class ReaderPair : public QWidget {
    Q_OBJECT

public:
    // Takes both readers into the pair.
    ReaderPair(ReaderTab *left, ReaderTab *right, QWidget *parent = nullptr);

    ReaderTab *left() const { return m_left; }
    ReaderTab *right() const { return m_right; }
    QList<ReaderTab *> readers() const;
    ReaderTab *activeReader() const;
    ReaderTab *otherThan(const ReaderTab *reader) const;
    void setActive(ReaderTab *reader);

    // Lets go of one side, which is no longer shown here, and returns it.
    ReaderTab *take(ReaderTab *reader);
    // Puts a reader in place of one side, which is let go of and returned.
    ReaderTab *replace(ReaderTab *side, ReaderTab *reader);

    QSplitter *splitter() const { return m_splitter; }

signals:
    void activeChanged(papertrail::ui::ReaderTab *reader);

private:
    void adopt(ReaderTab *reader, int index);
    void release(ReaderTab *reader);
    void updatePlaces();

    QSplitter *m_splitter = nullptr;
    QPointer<ReaderTab> m_left;
    QPointer<ReaderTab> m_right;
    QPointer<ReaderTab> m_active;
};

} // namespace papertrail::ui
