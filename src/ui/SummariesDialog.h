#pragma once

#include "summary/SummaryBatches.h"

#include <QDialog>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace papertrail::ui {

class LibrarySession;

// Summaries from Claude in one place: those asked for at half price that are still on their way or failed, those that
// have come back and wait to be reviewed, and what asking has cost.
class SummariesDialog : public QDialog {
    Q_OBJECT

public:
    enum class Page { OnTheirWay, ToReview, Spending };

    SummariesDialog(LibrarySession &session, summary::SummaryBatches &batches, QWidget *parent = nullptr);

    void showPage(Page page);
    // Shows this publication's summary on its way or to review, if it has one.
    void showPublication(qint64 publicationId);
    void reload();

    QTabWidget *tabs() const { return m_tabs; }
    QTreeWidget *waitingList() const { return m_waiting; }
    QListWidget *reviewList() const { return m_review; }
    QPlainTextEdit *reviewText() const { return m_reviewText; }
    QLabel *reviewInfo() const { return m_reviewInfo; }
    QPushButton *keepButton() const { return m_keep; }
    QPushButton *keepAllButton() const { return m_keepAll; }
    QPushButton *discardButton() const { return m_discard; }
    QLabel *totalsLabel() const { return m_totals; }
    QLabel *spendingNote() const { return m_spendingNote; }
    QTreeWidget *spendingList() const { return m_spending; }

signals:
    // The user wants Anthropic asked now rather than at the next regular check.
    void checkRequested();

private:
    QWidget *buildWaitingPage();
    QWidget *buildReviewPage();
    QWidget *buildSpendingPage();
    void showChosenReview();
    std::optional<summary::BatchEntry> chosenReview() const;
    void keepChosen();
    void keepAll();
    void discardChosen();
    void dismissChosen();
    void reloadSpending();
    void problem(const QString &doing, const std::exception &error);

    LibrarySession &m_session;
    summary::SummaryBatches &m_batches;
    QList<summary::BatchEntry> m_entries;
    QTabWidget *m_tabs = nullptr;
    QTreeWidget *m_waiting = nullptr;
    QLabel *m_waitingNote = nullptr;
    QPushButton *m_check = nullptr;
    QPushButton *m_dismiss = nullptr;
    QListWidget *m_review = nullptr;
    QLabel *m_reviewTitle = nullptr;
    QLabel *m_reviewInfo = nullptr;
    QPlainTextEdit *m_reviewText = nullptr;
    QPushButton *m_keep = nullptr;
    QPushButton *m_keepAll = nullptr;
    QPushButton *m_discard = nullptr;
    QLabel *m_totals = nullptr;
    QTreeWidget *m_spending = nullptr;
    QLabel *m_spendingNote = nullptr;
};

} // namespace papertrail::ui
