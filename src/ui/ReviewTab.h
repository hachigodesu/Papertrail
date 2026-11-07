#pragma once

#include <QList>
#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace papertrail::core {
struct LookupCandidate;
}

namespace papertrail::ui {

class LibrarySession;

// Matches that weren't certain, each compared with what's in the library, for the user to accept or turn down.
class ReviewTab : public QWidget {
    Q_OBJECT

public:
    explicit ReviewTab(LibrarySession &session, QWidget *parent = nullptr);

    void reload();

    // The publication being compared, and the table of what would change. For tests too.
    qint64 currentPublication() const;
    QTableWidget *comparison() const { return m_comparison; }
    QListWidget *list() const { return m_list; }
    QComboBox *candidateBox() const { return m_candidates; }

    void acceptCurrent();
    void rejectCurrent();

signals:
    // Something in the library has changed as a result.
    void libraryEdited();
    void openRequested(qint64 publicationId);

private:
    void showPublication();
    void showCandidate();
    void moveOn(int row);

    LibrarySession &m_session;
    QListWidget *m_list = nullptr;
    QLabel *m_heading = nullptr;
    QComboBox *m_candidates = nullptr;
    QLabel *m_matchReason = nullptr;
    QTableWidget *m_comparison = nullptr;
    QPushButton *m_accept = nullptr;
    QPushButton *m_reject = nullptr;
    QPushButton *m_open = nullptr;
    QWidget *m_detail = nullptr;
    QLabel *m_empty = nullptr;
};

} // namespace papertrail::ui
