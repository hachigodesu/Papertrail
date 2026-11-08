#pragma once

#include "core/repo/VolumeArticles.h"
#include "ui/DocumentClient.h"

#include <QDialog>
#include <QHash>
#include <QSet>

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
    int namedCount() const { return m_named; }
    // The articles made or named, for looking up their details, which a table of contents doesn't give.
    QList<qint64> articleIds() const { return m_articleIds; }

    // Whether the volume's numbers were filled in from its pages, which can happen without creating anything.
    bool filledNumbering() const { return m_filledNumbering; }

    // Creates what's chosen, and returns whether it could.
    bool createChosen();

    QTableWidget *table() const { return m_table; }
    QComboBox *levelBox() const { return m_level; }
    QLabel *messageLabel() const { return m_message; }

    // How many pages are still to be read for titles the contents didn't give.
    int headingsPending() const { return int(m_headingsWanted.size()); }
    // Whether, with no contents to go on, the articles were found by reading every page.
    bool foundFromPages() const { return m_fromPages; }

    // The most pages read one by one for a file with no contents.
    static constexpr int kMostPagesToRead = 2000;

signals:
    // The contents have been read, and the table filled in.
    void contentsReady();
    void contentsUnavailable();

    // Every title that had to be read from a page has been.
    void headingsRead();

private:
    void onOpened(int pageCount, bool reflowable);
    void onOutline(const QList<papertrail::ui::ReaderOutlineItem> &items);
    void onHeading(int page, const papertrail::ui::ReaderHeading &heading);
    void requestHeadings();
    void readEveryPage();
    void showRow(int row);
    void updateMessage();
    void fillTable();
    void cantOffer(const QString &message);
    void updateCreateButton();

    LibrarySession &m_session;
    qint64 m_volumeId;
    DocumentClient *m_client = nullptr;
    QList<core::ContentsEntry> m_entries;
    QList<core::ArticleDraft> m_drafts; // One for each row, as proposed.

    // What's printed on the pages that start an entry the contents only numbered, by page counting from 0.
    struct Heading {
        QString title;
        QString doi;
        QStringList dois;
    };
    QHash<int, Heading> m_headings;
    QSet<int> m_headingsWanted;
    int m_pageCount = 0;
    int m_created = 0;
    int m_named = 0;
    QList<qint64> m_articleIds;
    bool m_numberingRead = false;
    bool m_fromPages = false;
    bool m_pagesRead = false;
    bool m_filledNumbering = false;
    QString m_numberingNote;
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
