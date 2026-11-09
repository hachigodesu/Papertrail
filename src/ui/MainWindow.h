#pragma once

#include "core/model/Publication.h"
#include "core/query/PublicationFilter.h"
#include "core/repo/FileRepository.h"
#include "import/ImportService.h"
#include "metadata/LookupEngine.h"
#include "ui/QueryThreads.h"

#include <QHash>
#include <QJsonObject>
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

namespace papertrail::summary {
class SummaryBatches;
}

namespace papertrail::ui {

class BackupsDialog;
class BulkChanges;
class DailyBackupThread;
class MarkedCopyActions;
class RemovedPublicationsDialog;
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
class CitationActions;
class LibraryTransferActions;
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
    ReviewTab *reviewTab() const;
    LookupThread *lookupThread() const { return m_lookup; }
    IndexThread *indexThread() const { return m_index; }
    QCheckBox *searchInsideBox() const { return m_searchInside; }
    QToolButton *searchHelpButton() const { return m_searchHelp; }
    QAction *indexContentsAction() const { return m_indexContents; }
    QPushButton *reviewButton() const { return m_reviewButton; }
    QMessageBox *reviewOffer() const { return m_reviewOffer; }
    // New retractions and concerns: the count in the status bar, the offer to show them, and showing them.
    QPushButton *retractionStatusButton() const { return m_retractionStatus; }
    QMessageBox *retractionOffer() const { return m_retractionOffer; }
    void updateRetractionWarning();
    void showNewRetractions();

    // Stops importing until something asks for it again by hand, or until the next start.
    void stopImport();

    void setTab(core::LibraryTab tab);
    // Lists the articles of an issue, proceedings or book in the library, through an in: search.
    void showArticlesOf(qint64 holder);

    // The search box, tab and filters, kept by name and shown again from the sidebar.
    QJsonObject currentSearchDefinition();
    std::optional<qint64> saveCurrentSearch(const QString &name);
    void showSavedSearch(qint64 id);
    // Lists the saved searches in the sidebar again, after they change.
    void refreshSavedSearches();
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
    // Asks once for all of them, and removes them together.
    void removeRoots(const QList<qint64> &rootIds);
    // Imported publications still waiting for their files, deleted after asking.
    void confirmForgetWaiting();

    // What each choice for removing a folder does, once it's been made.
    void removeRootKeepingDetails(qint64 rootId) { removeRootsKeepingDetails({rootId}); }
    void removeRootAndDetails(qint64 rootId) { removeRootsAndDetails({rootId}); }
    void removeRootsKeepingDetails(const QList<qint64> &rootIds);
    void removeRootsAndDetails(const QList<qint64> &rootIds);
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
    CitationActions *citations() const { return m_citations; }

    // Exports citations for what's selected, or everything shown if nothing is.
    void exportCitations();

    // Looks at the library afresh, as after a change made elsewhere.
    void refreshNow();
    // Scans every library folder for new, changed and missing files.
    void rescanAll();

    // Removes publications from the library after asking. Their files aren't touched, and are kept out when
    // found again, until brought back.
    void askToRemoveSelected();
    void removePublications(const QList<qint64> &ids);
    void showRemovedPublications();
    RemovedPublicationsDialog *removedDialog() const { return m_removedDialog; }

    MarkedCopyActions *markedCopy() const { return m_markedCopy; }

    // Summaries at half price for what's chosen, after confirming, and collecting them as they come back.
    void showBatchSummaryDialog();
    void checkSummaryBatches();
    summary::SummaryBatches *summaryBatches() const { return m_batches; }
    // Summaries on their way, to review, and what they've cost, opened at one publication's if it's given.
    void showSummariesDialog(std::optional<qint64> publicationId = std::nullopt);
    QPushButton *summaryStatusButton() const { return m_summaryStatus; }
    // What an empty library says, for tests.
    QPushButton *welcomeImportButton() const { return m_welcomeImport; }
    QLabel *welcomePlaceLabel() const { return m_welcomePlace; }

    // Every highlight and note in the library, each opening its document there.
    void showNotesDialog();
    // Opens the document a highlight or note is in, at its place. Returns whether it could.
    bool openAnnotation(qint64 annotationId);

    // Lists backups, makes one, and restores one.
    void showBackupsDialog();
    BackupsDialog *backupsDialog() const { return m_backupsDialog; }
    // Replaces the library with a backup's contents, after quietening everything else. Returns whether it did.
    bool restoreBackup(const QString &backupPath);
    // Makes the day's backup in the background, if it's wanted and not yet made.
    void startDailyBackup();
    DailyBackupThread *dailyBackupThread() const { return m_dailyBackup; }

    LibraryTransferActions *libraryTransfer() const { return m_transfer; }
    // Quietens everything else that writes to the library, and closes open documents, before an import.
    void prepareForImport();
    void showSuggestedFiles();

    // Says which file is a publication's, for one with none, such as one imported that's still waiting for its
    // file. The file must be in one of the library's folders. If it's already another publication's, the two
    // become one. Returns whether it could.
    bool linkFileByHand(qint64 publicationId, const QString &path);
    QAction *checkSuggestedFilesAction() const { return m_checkSuggestedFiles; }
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
    void updateSuggestedFilesAction();
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
    void changeRootLocation(qint64 rootId);
    void confirmForgetHidden();
    void afterFoldersChanged();
    void letGoOfHidden();
    void offerReviewIfDue();
    void offerRetractionsIfDue();
    void offerWhatsDue(); // Retractions first, then details to review.
    void deletePublications(const QList<qint64> &ids);
    void askForBulkTag();
    void askToAddSelectedToNewCollection();
    void editPublication(qint64 id);
    void openOrChooseFormat(qint64 id);
    void openPublicationElsewhere(qint64 id);
    void showPublicationInFolder(qint64 id);
    std::optional<QString> firstFilePath(qint64 publicationId);

public:
    // What the metadata editor suggests while typing: values already in the library for `field`, or names of
    // people for a people field.
    QStringList metadataSuggestions(const QString &field, const QString &typed);

private:
    void showAbout();
    // Where the licence texts of everything bundled were installed, or nothing in a build that isn't installed.
    static QString licencesFolder();
    void showLookupSettings();
    void showSettings();
    void askToSaveSearch();
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
    CitationActions *m_citations = nullptr;
    DailyBackupThread *m_dailyBackup = nullptr;
    MarkedCopyActions *m_markedCopy = nullptr;
    summary::SummaryBatches *m_batches = nullptr;
    QTimer *m_batchTimer = nullptr;
    QPushButton *m_summaryStatus = nullptr;
    void refreshSummaryStatus();
    BackupsDialog *m_backupsDialog = nullptr;
    RemovedPublicationsDialog *m_removedDialog = nullptr;
    QToolButton *m_searchHelp = nullptr;
    LibraryTransferActions *m_transfer = nullptr;
    QAction *m_checkSuggestedFiles = nullptr;
    QWidget *m_libraryPage = nullptr; // Always the first tab.
    ArticleCoverThread *m_articleCovers = nullptr;
    std::unique_ptr<ApiKeyStore> m_keys;
    std::unique_ptr<metadata::Transport> m_summaryTransport;
    QCheckBox *m_searchInside = nullptr;
    QAction *m_indexContents = nullptr;
    QLabel *m_indexLabel = nullptr;
    QPushButton *m_indexStop = nullptr;
    LookupThread *m_lookup = nullptr;
    QPointer<ReviewTab> m_reviewTab; // Cleared when its tab is closed, even while the window itself is closing.
    QPointer<QMessageBox> m_reviewOffer;
    qint64 m_reviewCountSeen = 0; // Offers are only for what's been added since the last one.
    QPointer<QMessageBox> m_retractionOffer;
    QPushButton *m_retractionStatus = nullptr;
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
    QObject *m_documentTabBar = nullptr; // What the event filter watches, kept so it never has to ask the tabs.
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
    QPushButton *m_welcomeImport = nullptr;
    QLabel *m_welcomePlace = nullptr;
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
