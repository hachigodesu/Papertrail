#pragma once

#include "core/repo/AnnotationRepository.h"
#include "ui/DocumentClient.h"
#include "ui/PageView.h"

#include <QList>
#include <QRectF>
#include <QHash>
#include <QList>
#include <QPair>
#include <QWidget>

#include <optional>

class QListWidget;
class QMenu;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

class LibrarySession;
class ThumbnailModel;

// One open document: its pages, table of contents, thumbnails and search.
class ReaderTab : public QWidget {
    Q_OBJECT

public:
    ReaderTab(LibrarySession &session, qint64 publicationId, qint64 fileId, const QString &format, const QString &path,
              const QString &convertedPath, QWidget *parent = nullptr);
    ~ReaderTab() override;

    qint64 publicationId() const { return m_publicationId; }
    qint64 fileId() const { return m_fileId; }

    // Remembers the place in the document for next time.
    void saveState();

    // Puts the cursor in this document's search box.
    void focusSearch();

    // For an article in a volume's file: whether only its own pages are shown, or the whole file. Anything
    // else always shows the whole file. Opened from the library, an article shows only its own pages.
    void setArticleOnly(bool only);
    bool isArticleOnly() const { return m_articleOnly; }
    bool isArticle() const { return m_articlePages.has_value(); }
    QToolButton *wholeFileButton() const { return m_wholeFile; }

    // For tests.
    PageView *pageView() const { return m_view; }
    DocumentClient *client() const { return m_client; }
    QTreeWidget *contents() const { return m_contents; }
    QListView *thumbnails() const { return m_thumbnails; }
    QLineEdit *searchBox() const { return m_search; }
    QLabel *searchStatus() const { return m_searchStatus; }
    QLabel *problemLabel() const { return m_problem; }
    QComboBox *layoutBox() const { return m_layoutBox; }
    QComboBox *coloursBox() const { return m_coloursBox; }
    QComboBox *zoomBox() const { return m_zoomBox; }
    QLineEdit *pageBox() const { return m_pageBox; }
    bool isReady() const { return m_ready; }
    bool isShowingProblem() const;
    int hitCount() const { return int(m_hits.size()); }
    int currentHitIndex() const { return m_currentHit; }
    bool isReflowable() const { return m_reflowable; }
    double fontSize() const { return m_layout.fontSize; }
    void changeFontSize(double change);
    void setSidePanelVisible(bool visible);
    void followExternalLink(const QString &uri);
    void setOpenLinkHandler(const std::function<void(const QUrl &)> &handler) { m_openLink = handler; }
    void nextHit();
    void previousHit();
    void applySearch();

    // Goes to where a search across the library found something, and looks for it here too. For a file
    // whose pages move with its layout, the page can't be relied on, so it goes to the first match instead.
    void showMatch(int page, const QString &text);
    // Goes to a highlight or note, once the document is ready if it isn't yet.
    void showAnnotation(qint64 id);

    // Highlights and notes. These are kept in the library's database, and never in the file.
    void annotateSelection(core::AnnotationType type, const QString &note = QString(), const QString &colour = QString());
    void setAnnotationNote(qint64 id, const QString &note);
    void setAnnotationColour(qint64 id, const QString &colour);
    void deleteAnnotation(qint64 id);
    bool exportAnnotations(const QString &path);
    QListWidget *annotationList() const { return m_annotationList; }
    QList<int> markedPages() const;

signals:
    // The user asked to open this file with something else, because it can't be shown here.
    void openElsewhereRequested(const QString &path);

    // Reports what's happening with this tab's search and problems, for the window's status bar.
    void statusMessage(const QString &message);

    // A highlight or note has been added, changed or removed.
    void annotationsChanged();

private:
    struct Hit {
        int page = 0;
        QRectF rect;
    };

    void buildToolbar();
    void buildSidePanel();
    void connectClient();
    void onOpened(int pageCount, bool reflowable);
    void showProblem(DocumentClient::Failure failure, const QString &message);
    void updatePageBox();
    void updateZoomBox();
    void restorePlace();
    void onOutline(const QList<papertrail::ui::ReaderOutlineItem> &items);
    void onSearchHits(int page, const QList<QRectF> &rects);
    void updateSearchStatus();
    void showCurrentHit();
    void onContentsActivated(QTreeWidgetItem *item);
    void applyArticleRange();
    void ensurePageShown(int page);
    bool pageIsShown(int page) const;

    LibrarySession &m_session;
    void applyPendingMatch();
    void loadAnnotations();
    void locateAnnotations();
    void showMark(const core::Annotation &annotation);
    void refreshAnnotationList();
    void goToAnnotation(qint64 id);
    void onAnchored(qint64 token, int part, const QString &place, int occurrence);
    void onLocated(qint64 token, int part, int page, const QList<QRectF> &rects);
    void addAnnotationActions(QMenu *menu, const QPoint &viewportPoint);
    static QStringList colourNames();

public:
    // Adds a menu for a highlight's colour, with its current one ticked. For tests too.
    QMenu *addColourMenu(QMenu *menu, qint64 id);

private:
    void askForNoteAndAnnotate();
    void askToEditNote(qint64 id);
    void askToExportAnnotations();

    std::optional<std::pair<int, QString>> m_pendingMatch;
    std::optional<qint64> m_pendingAnnotation;

    // What's on this file's contents, and what's on other copies or formats of this publication.
    QString m_contentHash;
    QList<core::Annotation> m_annotations;
    QList<core::Annotation> m_annotationsElsewhere;
    QHash<qint64, QList<QPair<int, QList<QRectF>>>> m_locatedParts; // By annotation and part, in text that moves.
    qint64 m_locateRound = 0; // Answers from before the layout last changed are out of date.

    struct DraftAnnotation {
        core::Annotation annotation;
        int waitingFor = 0;
    };
    QHash<qint64, DraftAnnotation> m_drafts;
    qint64 m_nextDraft = 1;

    QWidget *m_annotationPanel = nullptr;
    QListWidget *m_annotationList = nullptr;
    QAction *m_highlightAction = nullptr;

    qint64 m_publicationId;
    qint64 m_fileId;

    // An article's first and last pages in its volume's file, counting from 0.
    std::optional<std::pair<int, int>> m_articlePages;
    bool m_articleOnly = false;
    QString m_wholeFileLabel;
    QToolButton *m_wholeFile = nullptr;
    QAction *m_wholeFileAction = nullptr;
    QString m_format;
    QString m_path;
    ReaderLayout m_layout;
    DocumentClient *m_client = nullptr;
    PageView *m_view = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_problem = nullptr;
    QPushButton *m_openElsewhere = nullptr;

    QToolBar *m_toolbar = nullptr;
    QLineEdit *m_pageBox = nullptr;
    QLabel *m_pageTotal = nullptr;
    QComboBox *m_zoomBox = nullptr;
    QComboBox *m_layoutBox = nullptr;
    QComboBox *m_coloursBox = nullptr;
    QToolButton *m_backButton = nullptr;
    QToolButton *m_smallerText = nullptr;
    QToolButton *m_largerText = nullptr;
    QAction *m_smallerTextAction = nullptr;
    QAction *m_largerTextAction = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_searchStatus = nullptr;
    QToolButton *m_searchPrevious = nullptr;
    QToolButton *m_searchNext = nullptr;

    QSplitter *m_splitter = nullptr;
    QTabWidget *m_side = nullptr;
    QTreeWidget *m_contents = nullptr;
    QListView *m_thumbnails = nullptr;
    ThumbnailModel *m_thumbnailModel = nullptr;
    QToolButton *m_sideToggle = nullptr;

    QTimer *m_saveTimer = nullptr;
    QTimer *m_searchTimer = nullptr;

    bool m_ready = false;
    bool m_reflowable = false;
    bool m_failedToOpen = false;
    QString m_bookmark;
    int m_bookmarkPage = -1;
    bool m_restoring = false;
    std::function<void(const QUrl &)> m_openLink;

    QList<Hit> m_hits;
    int m_currentHit = -1;
    bool m_searchRunning = false;
    QString m_activeSearch;
    int m_searchStartPage = 0;
};

} // namespace papertrail::ui
