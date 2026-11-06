#pragma once

#include "ui/DocumentClient.h"
#include "ui/PageView.h"

#include <QList>
#include <QRectF>
#include <QWidget>

#include <optional>

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
              QWidget *parent = nullptr);
    ~ReaderTab() override;

    qint64 publicationId() const { return m_publicationId; }
    qint64 fileId() const { return m_fileId; }

    // Remembers the place in the document for next time.
    void saveState();

    // Puts the cursor in this document's search box.
    void focusSearch();

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

signals:
    // The user asked to open this file with something else, because it can't be shown here.
    void openElsewhereRequested(const QString &path);

    // Reports what's happening with this tab's search and problems, for the window's status bar.
    void statusMessage(const QString &message);

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

    LibrarySession &m_session;
    qint64 m_publicationId;
    qint64 m_fileId;
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
