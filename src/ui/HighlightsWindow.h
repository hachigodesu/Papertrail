#pragma once

#include "core/repo/AnnotationRepository.h"

#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

class LibrarySession;

// Every highlight and note in the library in one window, kept open beside the library and its documents, filtered by
// words, colour, kind, collection, tag or publication, each opening its document at that place. Its list is read on a
// connection of its own, so the window never waits, and changes made here reach any document showing them.
class HighlightsWindow : public QWidget {
    Q_OBJECT

public:
    explicit HighlightsWindow(LibrarySession &session, QWidget *parent = nullptr);
    ~HighlightsWindow() override;

    // Only those made in this publication, or all again with 0.
    void showOnlyPublication(qint64 publicationId, const QString &title);
    // Reads the list again with the choices as they are now, as after a change elsewhere.
    void reload();
    bool isLoading() const { return m_thread != nullptr; }

    // How many are listed at most. More than this asks for something to narrow them.
    static constexpr int kShownAtMost = 2000;

    // For tests.
    QLineEdit *searchBox() const { return m_search; }
    QComboBox *colourChoice() const { return m_colour; }
    QComboBox *kindChoice() const { return m_kind; }
    QComboBox *collectionChoice() const { return m_collection; }
    QComboBox *tagChoice() const { return m_tag; }
    QCheckBox *withNoteChoice() const { return m_withNote; }
    QTreeWidget *list() const { return m_list; }
    QLabel *countLabel() const { return m_count; }
    QList<qint64> shownIds() const;
    // Writes what's shown as Markdown, grouped by publication. Returns false and says why if it can't.
    bool exportShown(const QString &path, QString *error = nullptr);
    void setNoteOf(qint64 annotationId, const QString &note);
    void setColourOf(qint64 annotationId, const QString &colour);
    void deleteWithoutAsking(qint64 annotationId);

signals:
    // The user wants to see this highlight or note in its document.
    void openRequested(qint64 annotationId);
    // Highlights on the file with these contents have changed here, so documents showing them should read them again.
    void annotationsChanged(const QString &contentHash);
    void loaded();

protected:
    void changeEvent(QEvent *event) override;

private:
    core::AnnotationFilter currentFilter() const;
    void fillChoices();
    void showFound(const QList<core::AnnotationListing> &found, qint64 total);
    void showPreview();
    void showMenu(const QPoint &position);
    void openChosen();
    void askToExport();
    void changed(qint64 annotationId);
    qint64 chosenId() const;
    std::optional<core::AnnotationListing> listingOf(qint64 annotationId) const;

    LibrarySession &m_session;
    QLineEdit *m_search = nullptr;
    QComboBox *m_colour = nullptr;
    QComboBox *m_kind = nullptr;
    QComboBox *m_collection = nullptr;
    QComboBox *m_tag = nullptr;
    QComboBox *m_sort = nullptr;
    QCheckBox *m_withNote = nullptr;
    QLabel *m_only = nullptr;
    QPushButton *m_showAll = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_count = nullptr;
    QPushButton *m_open = nullptr;
    QTimer *m_typing = nullptr;

    qint64 m_onlyPublication = 0;
    QList<core::AnnotationListing> m_found;
    QThread *m_thread = nullptr;
    bool m_again = false;
    QByteArray m_stamp; // The library's change stamp when last read, to read again only after a change.
};

// Where a picture of an area is kept once a document has drawn it, by the highlight and when it last changed, so the
// highlights window can show it without opening the document.
QString areaPicturePath(const QString &cacheDirectory, const core::Annotation &annotation);

} // namespace papertrail::ui
