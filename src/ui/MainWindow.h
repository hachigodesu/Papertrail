#pragma once

#include "core/model/Publication.h"
#include "core/query/PublicationFilter.h"
#include "core/repo/FileRepository.h"
#include "import/ImportService.h"
#include "metadata/LookupEngine.h"
#include "ui/QueryThreads.h"

#include <QHash>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>

#include <functional>
#include <memory>
#include <optional>

class QAbstractItemView;
class QAction;
class QCheckBox;
class QComboBox;
class QItemSelectionModel;
class QLabel;
class QLineEdit;
class QMenu;
class QListView;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTableView;
class QTimer;
class QToolButton;

namespace papertrail::ui {

class BulkChanges;
class CoverCache;
class DetailsPanel;
class FilterSidebar;
class FolderWatcher;
class ImportThread;
class LibraryFoldersDialog;
class LibrarySession;
class PublicationListModel;
class LookupThread;
class ReaderTab;
class ApiKeyStore;
class IndexThread;
class ArticleCoverThread;
class ReviewTab;

// The library: tabs, search, filters, the grid or list of publications, and their details.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(LibrarySession &session, QWidget *parent = nullptr);
    ~MainWindow() override;

    enum class ViewMode { Grid, List };

    // Adds a folder to the library and imports it. A folder inside one already in the library
    // just scans it again. `lookupMode` overrides how new publications get looked up for this import.
    void addFolder(const QString &path, const std::optional<QString> &lookupMode = std::nullopt);

    // What online lookups are set to: "offline" until someone has chosen otherwise.
    QString lookupMode();

    // Chooses how lookups work from now on. Going offline also drops anything still waiting.
    void setLookupMode(const QString &mode, const QString &email);

    // Queues lookups for these, and starts on them. Does nothing while offline.
    void lookUp(const QList<qint64> &ids);

    void showReviewTab();
    ReviewTab *reviewTab() const { return m_reviewTab; }
    LookupThread *lookupThread() const { return m_lookup; }
    IndexThread *indexThread() const { return m_index; }
    QCheckBox *searchInsideBox() const { return m_searchInside; }
    QAction *indexContentsAction() const { return m_indexContents; }
    QPushButton *reviewButton() const { return m_reviewButton; }
    QMessageBox *reviewOffer() const { return m_reviewOffer; }

    // Stops importing until something asks for it again by hand, or until the next start.
    void stopImport();

    void setTab(core::LibraryTab tab);
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_viewMode; }

    // The ids of everything selected, in display order.
    QList<qint64> selectedIds();
    qint64 selectedCount() const;

    // Changes for everything selected. These happen in the background, and bulkChanges() says when.
    void tagSelected(const QString &path);
    void setSelectedReadStatus(core::ReadStatus status);
    void markSelectedChecked();

    // Opens a publication's main file in a tab of its own, or goes to its tab if it already has one.
    // An article shows only its own pages unless `articleOnly` is false, when it's shown in its volume's whole file.
    ReaderTab *openPublication(qint64 id, bool articleOnly = true);

    // Asks what to do with the folder's publications, then does it.
    void removeRoot(qint64 rootId);

    // What each choice for removing a folder does, once it's been made.
    void removeRootKeepingDetails(qint64 rootId);
    void removeRootAndDetails(qint64 rootId);
    void forgetHiddenPublications();

    // The same, for one particular file. Each format of a publication can have a tab of its own.
    ReaderTab *openPublicationFile(qint64 id, qint64 fileId, bool articleOnly = true);

    // One file per format that's available now, starting with the main file. More than one is a choice.
    QList<core::FileRecord> formatChoices(qint64 id);

    // Makes everything selected into one publication.
    void combineSelected();

    // Changes for everything selected, in the background like the others.
    void addSelectedToCollection(qint64 id);
    void removeSelectedFromCollection(qint64 id);

    // Asks Claude for a summary of this. It shows what will be sent and what it may cost before anything happens.
    void showSummaryDialog(qint64 publicationId);

    // Articles inside a volume. These ask what they need to first.
    void showContentsDialog(qint64 volumeId);
    void showAddArticleDialog(qint64 volumeId);
    void askToDeleteArticle(qint64 articleId);

    // Adds what can be done with collections to a menu, going by what's selected and what's in them.
    void addCollectionActions(QMenu *menu);

    // Shows the library again, or a document's tab.
    QTabWidget *documentTabs() const { return m_documentTabs; }
    void closeDocumentTab(int index);

    // Applies a finished edit: the same as pressing Save in the editor.
    void saveEdit(core::Publication publication);

    // For tests and previews.
    PublicationListModel *model() const { return m_model; }
    FilterSidebar *sidebar() const { return m_sidebar; }
    DetailsPanel *details() const { return m_details; }
    QLineEdit *searchBox() const { return m_search; }
    QTabBar *tabBar() const { return m_tabs; }
    QComboBox *sortBox() const { return m_sortBox; }
    QListView *gridView() const { return m_grid; }
    QTableView *listView() const { return m_list; }
    ImportThread *importThread() const { return m_import; }
    BulkChanges *bulkChanges() const { return m_bulk; }
    FolderWatcher *folderWatcher() const { return m_watcher; }
    QLabel *resultCount() const { return m_count; }
    QStackedWidget *viewStack() const { return m_stack; }
    void applyFiltersNow() { applyFilters(); }

signals:
    // The sidebar's counts have arrived for the current filter.
    void sidebarUpdated();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildMenus();
    void keepLibraryFirst();
    void buildToolbar();
    void buildCentre();
    void buildStatusBar();
    void connectImport();
    void restoreWindowState();
    void saveWindowState();

    core::PublicationFilter currentFilter() const;
    core::PublicationSort currentSort() const;
    void applyFilters();
    void scheduleRefresh();
    void refreshNow();
    void onResultsChanged();
    void onRowsLoaded();
    void onSelectionChanged();
    void updateStack();
    void chooseSort(int comboIndex);
    void onHeaderClicked(int section);
    void syncSortControls();
    void onSearchTextChanged();

    QAbstractItemView *activeView() const;
    void showContextMenu(const QPoint &position);
    void showFoldersDialog();
    void chooseFolderToAdd();
    void rescanAll();
    void changeRootLocation(qint64 rootId);
    void confirmForgetHidden();
    void afterFoldersChanged();
    void letGoOfHidden();
    void offerReviewIfDue();
    void deletePublications(const QList<qint64> &ids);
    void askForBulkTag();
    void askToAddSelectedToNewCollection();
    void editPublication(qint64 id);
    void openOrChooseFormat(qint64 id);
    void openPublicationElsewhere(qint64 id);
    void showPublicationInFolder(qint64 id);
    std::optional<QString> firstFilePath(qint64 publicationId);
    void showAbout();
    void showLookupSettings();
    void askAndLookUpLibrary();
    void askAndLookUpSelected();
    bool makeSureLookupModeIsChosen();
    void updateReviewButton();
    void onLookupProgress(qint64 done, qint64 remaining, const QString &source);
    void onLookupsFinished(const papertrail::metadata::LookupSummary &summary);

    void kickIndexing();
    void openAtMatch(qint64 publicationId, qint64 fileId, int page, const QString &text);

    void onImportBusyChanged(bool busy);
    void onImportProgress(qint64 done, qint64 remaining, const QString &file);
    void onRootScanned(qint64 rootId, const papertrail::import::ScanReport &report, const QStringList &folders);
    void onImportFinished(const papertrail::import::ImportReport &report);

    bool guarded(const QString &what, const std::function<void()> &action);

    LibrarySession &m_session;
    ImportThread *m_import = nullptr;
    IndexThread *m_index = nullptr;
    QWidget *m_libraryPage = nullptr; // Always the first tab.
    ArticleCoverThread *m_articleCovers = nullptr;
    std::unique_ptr<ApiKeyStore> m_keys;
    std::unique_ptr<metadata::Transport> m_summaryTransport;
    QCheckBox *m_searchInside = nullptr;
    QAction *m_indexContents = nullptr;
    QLabel *m_indexLabel = nullptr;
    QPushButton *m_indexStop = nullptr;
    LookupThread *m_lookup = nullptr;
    ReviewTab *m_reviewTab = nullptr;
    QPointer<QMessageBox> m_reviewOffer;
    qint64 m_reviewCountSeen = 0; // Offers are only for what's been added since the last one.
    QPushButton *m_reviewButton = nullptr;
    QLabel *m_lookupLabel = nullptr;
    QPushButton *m_lookupStop = nullptr;
    std::optional<QString> m_lookupOverride;
    BulkChanges *m_bulk = nullptr;
    FolderWatcher *m_watcher = nullptr;
    CoverCache *m_covers = nullptr;
    PublicationListModel *m_model = nullptr;
    SidebarQueryThread *m_sidebarThread = nullptr;
    quint64 m_sidebarGeneration = 0;

    QTabWidget *m_documentTabs = nullptr;
    QLineEdit *m_search = nullptr;
    QTabBar *m_tabs = nullptr;
    QComboBox *m_sortBox = nullptr;
    QToolButton *m_sortDirection = nullptr;
    QToolButton *m_gridButton = nullptr;
    QToolButton *m_listButton = nullptr;
    QSplitter *m_splitter = nullptr;
    FilterSidebar *m_sidebar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QListView *m_grid = nullptr;
    QTableView *m_list = nullptr;
    QWidget *m_messagePage = nullptr;
    QLabel *m_message = nullptr;
    QPushButton *m_messageButton = nullptr;
    DetailsPanel *m_details = nullptr;
    QItemSelectionModel *m_selection = nullptr;

    QLabel *m_count = nullptr;
    QLabel *m_importLabel = nullptr;
    QProgressBar *m_importBar = nullptr;
    QPushButton *m_importStop = nullptr;
    LibraryFoldersDialog *m_foldersDialog = nullptr;

    QTimer *m_searchTimer = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QTimer *m_coverRepaint = nullptr;
    QTimer *m_importShowDelay = nullptr;
    QAction *m_showFilters = nullptr;
    QAction *m_showDetails = nullptr;
    QAction *m_watchFolders = nullptr;
    QAction *m_rescanOnStartup = nullptr;

    ViewMode m_viewMode = ViewMode::Grid;
    core::SortField m_sortField = core::SortField::Title;
    bool m_sortAscending = true;
    bool m_sortChosenForSearch = false; // The sort was picked by hand during this search.
    std::optional<qint64> m_selectedId;
    int m_waitingForRow = -1;
    int m_lastCount = -1;
    bool m_importStoppedByHand = false;
    QHash<qint64, int> m_recheckCounts;
};

} // namespace papertrail::ui
