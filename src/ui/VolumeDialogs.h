#pragma once

#include "core/repo/VolumeArticles.h"
#include "ui/DocumentClient.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace papertrail::ui {

class LibrarySession;

// Proposes articles from a volume's table of contents, for the user to check and choose from. Nothing is
// created until they've said so. The file is read by a worker process, like anything else that opens one.
class ContentsDialog : public QDialog {
    Q_OBJECT

public:
    ContentsDialog(LibrarySession &session, qint64 volumeId, QWidget *parent = nullptr);

    // What's been chosen, as the table now stands. Rows that don't make sense are in `problem`.
    QList<core::ArticleDraft> chosenDrafts(QString *problem = nullptr) const;
    int createdCount() const { return m_created; }

    // Creates what's chosen, and returns whether it could.
    bool createChosen();

    QTableWidget *table() const { return m_table; }
    QComboBox *levelBox() const { return m_level; }
    QLabel *messageLabel() const { return m_message; }

signals:
    // The contents have been read, and the table filled in.
    void contentsReady();
    void contentsUnavailable();

private:
    void onOpened(int pageCount, bool reflowable);
    void onOutline(const QList<papertrail::ui::ReaderOutlineItem> &items);
    void fillTable();
    void cantOffer(const QString &message);
    void updateCreateButton();

    LibrarySession &m_session;
    qint64 m_volumeId;
    DocumentClient *m_client = nullptr;
    QList<core::ContentsEntry> m_entries;
    int m_pageCount = 0;
    int m_created = 0;
    QLabel *m_message = nullptr;
    QComboBox *m_level = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_create = nullptr;
};

// Adds one article to a volume by hand.
class AddArticleDialog : public QDialog {
    Q_OBJECT

public:
    // `pageCount` limits the pages, if the file's length is known.
    explicit AddArticleDialog(const std::optional<int> &pageCount, QWidget *parent = nullptr);

    core::ArticleDraft draft() const;

    QLineEdit *titleEdit() const { return m_title; }
    QLineEdit *authorsEdit() const { return m_authors; }
    QLineEdit *doiEdit() const { return m_doi; }
    QSpinBox *firstPageBox() const { return m_first; }
    QSpinBox *lastPageBox() const { return m_last; }

private:
    QLineEdit *m_title = nullptr;
    QLineEdit *m_authors = nullptr;
    QLineEdit *m_doi = nullptr;
    QSpinBox *m_first = nullptr;
    QSpinBox *m_last = nullptr;
    QPushButton *m_add = nullptr;
};

} // namespace papertrail::ui
