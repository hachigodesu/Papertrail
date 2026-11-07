#pragma once

#include "core/model/Publication.h"

#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QCompleter;

namespace papertrail::ui {

class CoverCache;
class LibrarySession;

// Everything known about the selected publication, with its tags, read status and notes editable
// in place. With several selected, it offers changes to all of them.
class DetailsPanel : public QWidget {
    Q_OBJECT

public:
    DetailsPanel(LibrarySession &session, CoverCache &covers, QWidget *parent = nullptr);

    void showPublication(qint64 id);

    // What to show matches for from inside documents. Empty means none are wanted.
    void setContentsSearch(const QString &match, const QString &readerText);
    void showSelectionCount(qint64 count);
    void showNothing();
    void reload(); // Reads the shown publication again, after a change elsewhere.
    void saveNotes(); // Saves any notes still waiting to be saved.

    std::optional<qint64> currentId() const { return m_id; }

    // Adds a tag to the shown publication, creating it if needed. A path such as
    // "Engineering > FEM" creates each level.
    void addTag(const QString &path);
    void removeTag(qint64 tagId);

    // For tests.
    QLabel *titleLabel() const { return m_title; }
    QLabel *authorsLabel() const { return m_authors; }
    QFormLayout *facts() const { return m_facts; }
    QComboBox *readStatusBox() const { return m_readStatus; }
    QCheckBox *preprintBox() const { return m_preprint; }
    QLabel *collectionsLabel() const { return m_collections; }
    QPlainTextEdit *summaryView() const { return m_summaryView; }
    QLabel *summaryInfo() const { return m_summaryInfo; }
    QPushButton *summarizeButton() const { return m_summarize; }
    QPushButton *deleteSummaryButton() const { return m_deleteSummary; }
    QWidget *matchesSection() const { return m_matchesSection; }
    QListWidget *matchList() const { return m_matchList; }
    QWidget *articlesSection() const { return m_articlesSection; }
    QListWidget *articleList() const { return m_articleList; }
    QLabel *partOfLabel() const { return m_partOf; }
    QPushButton *showVolumeButton() const { return m_showVolume; }
    QLabel *preprintHint() const { return m_preprintHint; }
    QWidget *undoRow() const { return m_undoRow; }
    void undoLookup();
    QPlainTextEdit *notesEdit() const { return m_notes; }
    QPushButton *editButton() const { return m_edit; }

signals:
    void libraryEdited();
    void editRequested(qint64 id);
    void openRequested(qint64 id);
    // An article from a volume's list, to be read in the whole of the volume's file.
    void openInVolumeRequested(qint64 articleId);
    void openFileRequested(qint64 id, qint64 fileId);
    void showInFolderRequested(qint64 id);

    // The user wants a summary of this publication from Claude.
    void summarizeRequested(qint64 publicationId);

    // Where a search found something inside a document.
    void openMatchRequested(qint64 publicationId, qint64 fileId, int page, const QString &text);

    // For a volume and the articles inside it.
    void articlesFromContentsRequested(qint64 volumeId);
    void addArticleRequested(qint64 volumeId);
    void deleteArticleRequested(qint64 articleId);

    // Changes for everything selected, from the several-selected page.
    void bulkTagRequested(const QString &path);
    void bulkReadStatusRequested(core::ReadStatus status);
    void bulkMarkCheckedRequested();

private:
    void buildSinglePage();
    void buildManyPage();
    void addFact(const QString &label, const QString &value);
    void addLink(const QString &label, const QString &text, const QUrl &url);
    void refreshTagCompleter(QLineEdit *edit);
    void changePublication(const std::function<void(core::Publication &)> &change);
    void setMarkedAsPreprint(bool marked);
    void updateCover();

    LibrarySession &m_session;
    CoverCache &m_covers;
    std::optional<qint64> m_id;
    QString m_coverSha;
    bool m_loading = false;
    bool m_notesDirty = false;

    QStackedWidget *m_pages = nullptr;
    QLabel *m_cover = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_authors = nullptr;
    QWidget *m_review = nullptr;
    QLabel *m_reviewReason = nullptr;
    QFormLayout *m_facts = nullptr;
    QComboBox *m_readStatus = nullptr;
    QComboBox *m_rating = nullptr;
    QVBoxLayout *m_tagRows = nullptr;
    QLineEdit *m_tagEdit = nullptr;
    QCheckBox *m_preprint = nullptr;
    QLabel *m_summaryInfo = nullptr;
    QPlainTextEdit *m_summaryView = nullptr;
    QPushButton *m_summarize = nullptr;
    QPushButton *m_deleteSummary = nullptr;
    QWidget *m_matchesSection = nullptr;
    QListWidget *m_matchList = nullptr;
    QString m_contentsMatch;
    QString m_contentsText;
    QWidget *m_articlesSection = nullptr;
    QListWidget *m_articleList = nullptr;
    QLabel *m_partOf = nullptr;
    QPushButton *m_showVolume = nullptr;
    QPushButton *m_deleteArticle = nullptr;
    QLabel *m_collections = nullptr;
    QLabel *m_preprintHint = nullptr;
    QWidget *m_undoRow = nullptr;
    QLabel *m_undoLabel = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QTimer *m_notesTimer = nullptr;
    QVBoxLayout *m_fileRows = nullptr;
    QHBoxLayout *m_openButtons = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_showInFolder = nullptr;
    QPushButton *m_edit = nullptr;

    QLabel *m_manyCount = nullptr;
    QLineEdit *m_manyTagEdit = nullptr;
};

} // namespace papertrail::ui
