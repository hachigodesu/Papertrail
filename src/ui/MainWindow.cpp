#include "ui/MainWindow.h"

#include "core/repo/FileRepository.h"
#include "core/repo/LookupRepository.h"
#include "core/repo/PublicationMerge.h"
#include "metadata/Matching.h"
#include "ui/BulkChanges.h"
#include "ui/CoverCache.h"
#include "ui/Delegates.h"
#include "ui/DetailsPanel.h"
#include "ui/FilterSidebar.h"
#include "ui/FolderWatcher.h"
#include "ui/ImportThread.h"
#include "ui/Labels.h"
#include "ui/LibraryFoldersDialog.h"
#include "ui/LibrarySession.h"
#include "ui/LookupThread.h"
#include "ui/OnlineLookupDialog.h"
#include "core/repo/VolumeArticles.h"
#include "core/query/SearchParser.h"
#include "ui/IndexThread.h"
#include "ui/ReviewTab.h"
#include "ui/VolumeDialogs.h"
#include "ui/MetadataEditor.h"
#include "ui/PublicationListModel.h"
#include "ui/ReaderTab.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCursor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QCheckBox>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QApplication>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace papertrail::ui {

using namespace core;

namespace {

constexpr qint64 kMostToCheckCollectionsFor = 1000;

// In a menu, & marks a keyboard shortcut, and a name's own has to be doubled to appear.
QString escapedForMenu(QString name)
{
    return name.replace(u'&', QStringLiteral("&&"));
}

constexpr int kMaxRechecks = 20; // A file locked for this long isn't simply still being copied.
constexpr qint64 kMaxToCombine = 20; // More than this at once is much more likely to be a slip.

const QList<LibraryTab> kTabs = {
    LibraryTab::All,       LibraryTab::JournalVolumes,         LibraryTab::JournalArticles,
    LibraryTab::Preprints, LibraryTab::ConferencePapers,       LibraryTab::Textbooks,
    LibraryTab::ThesesAndDissertations, LibraryTab::TechnicalReports, LibraryTab::Standards,
    LibraryTab::Other,
};

struct RowRange {
    int first;
    int last;
};

// The selection can cover 100,000 rows, so this works from its ranges rather than listing every
// index. The grid selects single cells and the list selects whole rows, so the rows are merged.
QList<RowRange> selectedRowRanges(const QItemSelectionModel *selection)
{
    QList<RowRange> ranges;
    const QItemSelection items = selection->selection();
    for (const QItemSelectionRange &range : items)
        ranges << RowRange{range.top(), range.bottom()};
    std::sort(ranges.begin(), ranges.end(), [](const RowRange &a, const RowRange &b) { return a.first < b.first; });

    QList<RowRange> merged;
    for (const RowRange &range : ranges) {
        if (!merged.isEmpty() && range.first <= merged.last().last + 1)
            merged.last().last = qMax(merged.last().last, range.last);
        else
            merged << range;
    }
    return merged;
}

QString sortFieldSetting(SortField field)
{
    switch (field) {
    case SortField::FirstAuthor: return QStringLiteral("author");
    case SortField::Year: return QStringLiteral("year");
    case SortField::Added: return QStringLiteral("added");
    default: return QStringLiteral("title");
    }
}

SortField sortFieldFromSetting(const QString &value)
{
    if (value == QLatin1StringView("author"))
        return SortField::FirstAuthor;
    if (value == QLatin1StringView("year"))
        return SortField::Year;
    if (value == QLatin1StringView("added"))
        return SortField::Added;
    return SortField::Title;
}

QString encodeState(const QByteArray &state)
{
    return QString::fromLatin1(state.toBase64());
}

QByteArray decodeState(const QJsonValue &value)
{
    return QByteArray::fromBase64(value.toString().toLatin1());
}

} // namespace

MainWindow::MainWindow(LibrarySession &session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Papertrail"));
    resize(1280, 800);

    m_covers = new CoverCache(session.paths().cacheDirectory, this);
    m_model = new PublicationListModel(session.paths().database, this);
    m_sidebarThread = new SidebarQueryThread(session.paths().database, this);
    m_import = new ImportThread(session.paths(), this);
    m_watcher = new FolderWatcher(this);
    m_bulk = new BulkChanges(session.paths().database, this);
    m_lookup = new LookupThread(session.paths(), this);
    m_index = new IndexThread(session.paths(), this);

    const QString sortSetting = session.settings().value(QStringLiteral("library.sort_field")).toString();
    m_sortField = sortFieldFromSetting(sortSetting);
    m_sortAscending = session.settings().value(QStringLiteral("library.sort_ascending")).toBool();
    m_watcher->setEnabled(session.settings().value(QStringLiteral("library.watch_folders")).toBool());

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(200);
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::applyFilters);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(150);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshNow);

    // Many covers can finish in one frame, and there's no use in repainting for each.
    m_coverRepaint = new QTimer(this);
    m_coverRepaint->setSingleShot(true);
    m_coverRepaint->setInterval(30);
    connect(m_coverRepaint, &QTimer::timeout, this, [this] { activeView()->viewport()->update(); });
    connect(m_covers, &CoverCache::coverReady, this, [this] {
        if (!m_coverRepaint->isActive())
            m_coverRepaint->start();
    });

    buildToolbar();
    buildCentre();
    buildStatusBar();
    buildMenus();
    connectImport();
    restoreWindowState();

    connect(m_model, &PublicationListModel::resultsChanged, this, &MainWindow::onResultsChanged);
    connect(m_model, &PublicationListModel::rowsLoaded, this, &MainWindow::onRowsLoaded);
    connect(m_model, &PublicationListModel::queryFailed, this, [this](const QString &message) {
        statusBar()->showMessage(tr("The library couldn't be read: %1").arg(message), 10000);
    });

    connect(m_sidebarThread, &SidebarQueryThread::ready, this, [this](const SidebarData &data) {
        if (data.generation != m_sidebarGeneration)
            return;
        if (!data.error.isEmpty()) {
            statusBar()->showMessage(tr("The filter counts couldn't be read: %1").arg(data.error), 10000);
            return;
        }
        m_sidebar->setData(data);
        emit sidebarUpdated();
    });
    connect(m_bulk, &BulkChanges::progress, this, [this](const QString &what, qint64 done, qint64 total) {
        statusBar()->showMessage(tr("%1: %2 of %3").arg(what, QLocale().toString(done), QLocale().toString(total)));
    });
    connect(m_bulk, &BulkChanges::finished, this, [this](const QString &what, qint64 total) {
        statusBar()->showMessage(tr("%1: finished, %2.").arg(what, publicationCount(total)), 5000);
        m_details->reload();
        if (m_foldersDialog)
            m_foldersDialog->reload();
        refreshNow();
    });
    connect(m_bulk, &BulkChanges::failed, this, [this](const QString &what, const QString &error) {
        statusBar()->showMessage(tr("%1 didn't finish: %2").arg(what, error), 15000);
        refreshNow();
    });

    m_sidebarThread->start();
    m_import->start();
    m_bulk->start();
    m_lookup->start();
    m_index->start();
    {
        const QString mode = lookupMode();
        m_import->setLookupMode(mode == QLatin1StringView("offline") ? QString() : mode);
    }

    applyFilters();

    // Started once the window is up, so a large library never delays its appearance.
    QTimer::singleShot(0, this, [this] {
        // Whatever was left from last time. In offline mode, there's nothing to carry on with.
        updateReviewButton();
        guarded(tr("Reading the lookups"), [&] {
            // What's already waiting has been left for later, so it's only the toolbar's to mention.
            m_reviewCountSeen = LookupRepository(m_session.database()).waitingForReviewCount();
            if (LookupRepository(m_session.database()).pendingCount() > 0)
                m_lookup->kick();
        });
        const QList<LibraryRoot> roots = m_session.files().roots();
        if (m_session.settings().value(QStringLiteral("library.rescan_on_startup")).toBool() && !roots.isEmpty())
            rescanAll();
        else
            m_import->resumeQueue();
        kickIndexing();
    });
}

MainWindow::~MainWindow()
{
    m_details->saveNotes();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_details->saveNotes();
    for (int i = 1; i < m_documentTabs->count(); ++i) {
        if (auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->widget(i)))
            reader->saveState();
    }
    saveWindowState();
    event->accept();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_watcher)
        m_watcher->windowActivated();
    if (event->type() == QEvent::WindowStateChange && m_lookup && !isMinimized())
        offerReviewIfDue();
    QMainWindow::changeEvent(event);
}

void MainWindow::buildToolbar()
{
    auto *bar = addToolBar(tr("Library"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);

    auto *container = new QWidget(bar);
    auto *row = new QHBoxLayout(container);
    row->setContentsMargins(6, 2, 6, 2);

    m_search = new QLineEdit(container);
    m_search->setPlaceholderText(tr("Search: fem, \"exact phrase\", author:knuth, year:>2010, tag:maths, doi:10.1000"));
    m_search->setAccessibleName(tr("Search the library"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    row->addWidget(m_search, 1);

    m_searchInside = new QCheckBox(tr("Inside documents"), container);
    m_searchInside->setToolTip(tr("Also looks for your words in the text of documents. Double-click a place under "
                                  "Found inside in the details to open it there."));
    m_searchInside->setChecked(m_session.settings().value(QStringLiteral("library.search_contents")).toBool());
    connect(m_searchInside, &QCheckBox::toggled, this, [this](bool checked) {
        guarded(tr("Saving the setting"), [&] { m_session.settings().setValue(QStringLiteral("library.search_contents"), checked); });
        applyFilters();
    });
    row->addWidget(m_searchInside);

    m_reviewButton = new QPushButton(container);
    m_reviewButton->setToolTip(tr("Matches found online that are waiting for you to check them."));
    m_reviewButton->hide();
    connect(m_reviewButton, &QPushButton::clicked, this, &MainWindow::showReviewTab);
    row->addWidget(m_reviewButton);

    auto *add = new QPushButton(tr("Add folder..."), container);
    connect(add, &QPushButton::clicked, this, &MainWindow::chooseFolderToAdd);
    row->addWidget(add);
    bar->addWidget(container);
}

void MainWindow::buildCentre()
{
    auto *central = new QWidget(this);
    auto *column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto *strip = new QHBoxLayout;
    strip->setContentsMargins(6, 0, 6, 0);
    m_tabs = new QTabBar(central);
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setAccessibleName(tr("Publication types"));
    for (const LibraryTab tab : kTabs)
        m_tabs->addTab(tabLabel(tab));
    m_tabs->setTabToolTip(int(kTabs.indexOf(LibraryTab::Preprints)),
                          tr("Publications you've marked as preprints. Papertrail doesn't sort these by itself."));
    connect(m_tabs, &QTabBar::currentChanged, this, &MainWindow::applyFilters);
    strip->addWidget(m_tabs, 1);

    strip->addWidget(new QLabel(tr("Sort:"), central));
    m_sortBox = new QComboBox(central);
    m_sortBox->setAccessibleName(tr("Sort by"));
    m_sortBox->addItem(tr("Title"), int(SortField::Title));
    m_sortBox->addItem(tr("First author"), int(SortField::FirstAuthor));
    m_sortBox->addItem(tr("Year"), int(SortField::Year));
    m_sortBox->addItem(tr("Date added"), int(SortField::Added));
    m_sortBox->addItem(tr("Best match"), int(SortField::Relevance));
    connect(m_sortBox, &QComboBox::activated, this, &MainWindow::chooseSort);
    strip->addWidget(m_sortBox);

    m_sortDirection = new QToolButton(central);
    m_sortDirection->setAccessibleName(tr("Sort direction"));
    connect(m_sortDirection, &QToolButton::clicked, this, [this] {
        m_sortAscending = !m_sortAscending;
        m_session.settings().setValue(QStringLiteral("library.sort_ascending"), m_sortAscending);
        syncSortControls();
        applyFilters();
    });
    strip->addWidget(m_sortDirection);

    m_gridButton = new QToolButton(central);
    m_gridButton->setText(tr("Grid"));
    m_gridButton->setCheckable(true);
    m_listButton = new QToolButton(central);
    m_listButton->setText(tr("List"));
    m_listButton->setCheckable(true);
    connect(m_gridButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Grid); });
    connect(m_listButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::List); });
    strip->addWidget(m_gridButton);
    strip->addWidget(m_listButton);
    column->addLayout(strip);

    m_splitter = new QSplitter(Qt::Horizontal, central);
    column->addWidget(m_splitter, 1);

    m_sidebar = new FilterSidebar(m_session.tags(), m_session.collections(), m_splitter);
    connect(m_sidebar, &FilterSidebar::filterChanged, this, &MainWindow::applyFilters);
    connect(m_sidebar, &FilterSidebar::tagsChanged, this, &MainWindow::refreshNow);
    connect(m_sidebar, &FilterSidebar::collectionsChanged, this, [this] {
        refreshNow();
        m_details->reload();
    });
    m_splitter->addWidget(m_sidebar);

    m_stack = new QStackedWidget(m_splitter);

    m_grid = new QListView(m_stack);
    m_grid->setModel(m_model);
    m_grid->setModelColumn(PublicationListModel::TitleColumn);
    // List mode with wrapping, rather than icon mode, because icon mode's layout gets slow with
    // very many items.
    m_grid->setViewMode(QListView::ListMode);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows); // So the list shows whole rows selected.
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->verticalScrollBar()->setSingleStep(40);
    m_grid->setItemDelegate(new GridDelegate(*m_covers, m_grid));
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setAccessibleName(tr("Publications"));
    m_stack->addWidget(m_grid);

    m_list = new QTableView(m_stack);
    m_list->setModel(m_model);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setShowGrid(false);
    m_list->setWordWrap(false);
    m_list->verticalHeader()->hide();
    m_list->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_list->verticalHeader()->setDefaultSectionSize(ListTitleDelegate::rowHeight());
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setItemDelegateForColumn(PublicationListModel::TitleColumn, new ListTitleDelegate(*m_covers, m_list));
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setAccessibleName(tr("Publications"));
    QHeaderView *header = m_list->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(PublicationListModel::TitleColumn, QHeaderView::Stretch);
    header->resizeSection(PublicationListModel::AuthorsColumn, 160);
    header->resizeSection(PublicationListModel::YearColumn, 56);
    header->resizeSection(PublicationListModel::ContainerColumn, 140);
    header->resizeSection(PublicationListModel::TypeColumn, 110);
    header->resizeSection(PublicationListModel::FormatColumn, 70);
    header->resizeSection(PublicationListModel::ReadColumn, 64);
    connect(header, &QHeaderView::sectionClicked, this, &MainWindow::onHeaderClicked);
    m_stack->addWidget(m_list);

    // Both views share one selection, so switching between them keeps it.
    m_selection = m_grid->selectionModel();
    QItemSelectionModel *listSelection = m_list->selectionModel();
    m_list->setSelectionModel(m_selection);
    delete listSelection;
    connect(m_selection, &QItemSelectionModel::selectionChanged, this, &MainWindow::onSelectionChanged);

    for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(m_grid), static_cast<QAbstractItemView *>(m_list)}) {
        connect(view, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
            if (const auto id = m_model->idAt(index.row()))
                openOrChooseFormat(*id);
        });
        connect(view, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    }

    m_messagePage = new QWidget(m_stack);
    auto *messageColumn = new QVBoxLayout(m_messagePage);
    messageColumn->addStretch();
    m_message = new QLabel(m_messagePage);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    messageColumn->addWidget(m_message);
    m_messageButton = new QPushButton(m_messagePage);
    messageColumn->addWidget(m_messageButton, 0, Qt::AlignCenter);
    messageColumn->addStretch();
    connect(m_messageButton, &QPushButton::clicked, this, [this] {
        if (m_session.files().roots().isEmpty()) {
            chooseFolderToAdd();
        } else {
            m_search->clear();
            m_sidebar->clearSelection();
            m_tabs->setCurrentIndex(0);
            applyFilters();
        }
    });
    m_stack->addWidget(m_messagePage);
    m_splitter->addWidget(m_stack);

    m_details = new DetailsPanel(m_session, *m_covers, m_splitter);
    m_details->showNothing();
    connect(m_details, &DetailsPanel::libraryEdited, this, &MainWindow::scheduleRefresh);
    connect(m_details, &DetailsPanel::editRequested, this, &MainWindow::editPublication);
    connect(m_details, &DetailsPanel::openRequested, this, &MainWindow::openPublication);
    connect(m_details, &DetailsPanel::openFileRequested, this, &MainWindow::openPublicationFile);
    connect(m_details, &DetailsPanel::showInFolderRequested, this, &MainWindow::showPublicationInFolder);
    connect(m_details, &DetailsPanel::openMatchRequested, this, &MainWindow::openAtMatch);
    connect(m_details, &DetailsPanel::articlesFromContentsRequested, this, &MainWindow::showContentsDialog);
    connect(m_details, &DetailsPanel::addArticleRequested, this, &MainWindow::showAddArticleDialog);
    connect(m_details, &DetailsPanel::deleteArticleRequested, this, &MainWindow::askToDeleteArticle);
    connect(m_details, &DetailsPanel::bulkTagRequested, this, &MainWindow::tagSelected);
    connect(m_details, &DetailsPanel::bulkReadStatusRequested, this, &MainWindow::setSelectedReadStatus);
    connect(m_details, &DetailsPanel::bulkMarkCheckedRequested, this, &MainWindow::markSelectedChecked);
    m_splitter->addWidget(m_details);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({240, 760, 280});
    m_splitter->setCollapsible(1, false);

    // The library is always the first tab. The tab bar only appears once a document is open.
    m_documentTabs = new QTabWidget(this);
    m_documentTabs->setDocumentMode(true);
    m_documentTabs->setTabBarAutoHide(true);
    m_documentTabs->setTabsClosable(true);
    m_documentTabs->setMovable(false);
    m_documentTabs->addTab(central, tr("Library"));
    for (const QTabBar::ButtonPosition side : {QTabBar::LeftSide, QTabBar::RightSide}) {
        if (QWidget *close = m_documentTabs->tabBar()->tabButton(0, side)) {
            m_documentTabs->tabBar()->setTabButton(0, side, nullptr);
            close->deleteLater();
        }
    }
    connect(m_documentTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeDocumentTab);
    connect(m_documentTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0 && m_lookup)
            offerReviewIfDue();
    });
    setCentralWidget(m_documentTabs);
    syncSortControls();
}

void MainWindow::buildStatusBar()
{
    m_count = new QLabel(this);
    statusBar()->addWidget(m_count);

    m_importLabel = new QLabel(this);
    m_importLabel->setTextFormat(Qt::PlainText);
    m_importBar = new QProgressBar(this);
    m_importBar->setMaximumWidth(220);
    m_importBar->setAccessibleName(tr("Import progress"));
    m_importStop = new QPushButton(tr("Stop"), this);
    m_importStop->setToolTip(tr("Stops importing. It carries on from the same place next time."));
    connect(m_importStop, &QPushButton::clicked, this, &MainWindow::stopImport);
    m_importShowDelay = new QTimer(this);
    m_importShowDelay->setSingleShot(true);
    m_importShowDelay->setInterval(400);
    connect(m_importShowDelay, &QTimer::timeout, this, [this] {
        m_importLabel->show();
        m_importBar->show();
        m_importStop->show();
    });

    m_indexLabel = new QLabel(this);
    m_indexLabel->setTextFormat(Qt::PlainText);
    m_indexLabel->hide();
    m_indexStop = new QPushButton(tr("Stop indexing"), this);
    m_indexStop->setToolTip(tr("Stops reading documents' text for now. It carries on from the same place the next time it starts."));
    m_indexStop->hide();
    connect(m_indexStop, &QPushButton::clicked, this, [this] { m_index->stopIndexing(); });
    statusBar()->addPermanentWidget(m_indexLabel);
    statusBar()->addPermanentWidget(m_indexStop);

    m_lookupLabel = new QLabel(this);
    m_lookupLabel->setTextFormat(Qt::PlainText);
    m_lookupStop = new QPushButton(tr("Stop lookups"), this);
    m_lookupStop->setToolTip(tr("Stops looking up details. It carries on from the same place next time."));
    connect(m_lookupStop, &QPushButton::clicked, m_lookup, &LookupThread::stopLookups);
    statusBar()->addPermanentWidget(m_lookupLabel);
    statusBar()->addPermanentWidget(m_lookupStop);
    m_lookupLabel->hide();
    m_lookupStop->hide();

    statusBar()->addPermanentWidget(m_importLabel);
    statusBar()->addPermanentWidget(m_importBar);
    statusBar()->addPermanentWidget(m_importStop);
    m_importLabel->hide();
    m_importBar->hide();
    m_importStop->hide();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Add folder..."), QKeySequence(Qt::CTRL | Qt::Key_O), this, &MainWindow::chooseFolderToAdd);
    file->addAction(tr("Library &folders..."), this, &MainWindow::showFoldersDialog);
    file->addAction(tr("&Scan folders again"), QKeySequence(Qt::Key_F5), this, &MainWindow::rescanAll);
    file->addSeparator();
    m_indexContents = file->addAction(tr("Index document text"));
    m_indexContents->setCheckable(true);
    m_indexContents->setChecked(m_session.settings().value(QStringLiteral("library.index_contents")).toBool());
    m_indexContents->setToolTip(tr("Reads documents' text so that it can be searched. It stays on this computer, and your files aren't changed."));
    connect(m_indexContents, &QAction::toggled, this, [this](bool on) {
        guarded(tr("Saving the setting"), [&] { m_session.settings().setValue(QStringLiteral("library.index_contents"), on); });
        if (on)
            m_index->kick();
        else
            m_index->stopIndexing();
    });
    m_rescanOnStartup = file->addAction(tr("Scan folders when Papertrail starts"));
    m_rescanOnStartup->setCheckable(true);
    m_rescanOnStartup->setChecked(m_session.settings().value(QStringLiteral("library.rescan_on_startup")).toBool());
    connect(m_rescanOnStartup, &QAction::toggled, this, [this](bool on) {
        m_session.settings().setValue(QStringLiteral("library.rescan_on_startup"), on);
    });
    m_watchFolders = file->addAction(tr("Watch folders for changes"));
    m_watchFolders->setCheckable(true);
    m_watchFolders->setChecked(m_watcher->isEnabled());
    connect(m_watchFolders, &QAction::toggled, this, [this](bool on) {
        m_session.settings().setValue(QStringLiteral("library.watch_folders"), on);
        m_watcher->setEnabled(on);
    });
    file->addSeparator();
    file->addAction(tr("&Online lookups..."), this, &MainWindow::showLookupSettings);
    file->addAction(tr("Look up details for the whole library..."), this, &MainWindow::askAndLookUpLibrary);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("&Search"), QKeySequence::Find, this, [this] {
        if (auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->currentWidget())) {
            reader->focusSearch();
        } else {
            m_search->setFocus();
            m_search->selectAll();
        }
    });
    view->addAction(tr("Close &tab"), QKeySequence(Qt::CTRL | Qt::Key_W), this, [this] {
        if (m_documentTabs->currentIndex() > 0)
            closeDocumentTab(m_documentTabs->currentIndex());
    });
    view->addAction(tr("Next tab"), QKeySequence(Qt::CTRL | Qt::Key_Tab), this, [this] {
        m_documentTabs->setCurrentIndex((m_documentTabs->currentIndex() + 1) % m_documentTabs->count());
    });
    view->addAction(tr("Previous tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this, [this] {
        const int count = m_documentTabs->count();
        m_documentTabs->setCurrentIndex((m_documentTabs->currentIndex() + count - 1) % count);
    });
    view->addSeparator();
    view->addAction(tr("&Grid"), QKeySequence(Qt::CTRL | Qt::Key_1), this, [this] { setViewMode(ViewMode::Grid); });
    view->addAction(tr("&List"), QKeySequence(Qt::CTRL | Qt::Key_2), this, [this] { setViewMode(ViewMode::List); });
    view->addSeparator();
    m_showFilters = view->addAction(tr("Show &filters"));
    m_showFilters->setCheckable(true);
    m_showFilters->setChecked(true);
    connect(m_showFilters, &QAction::toggled, this, [this](bool on) {
        m_sidebar->setVisible(on);
        m_session.settings().setValue(QStringLiteral("library.show_filters"), on);
    });
    m_showDetails = view->addAction(tr("Show &details"));
    m_showDetails->setCheckable(true);
    m_showDetails->setChecked(true);
    connect(m_showDetails, &QAction::toggled, this, [this](bool on) {
        m_details->setVisible(on);
        m_session.settings().setValue(QStringLiteral("library.show_details"), on);
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Papertrail"), this, &MainWindow::showAbout);
}

void MainWindow::connectImport()
{
    connect(m_import, &ImportThread::busyChanged, this, &MainWindow::onImportBusyChanged);
    connect(m_import, &ImportThread::progress, this, &MainWindow::onImportProgress);
    connect(m_import, &ImportThread::rootScanned, this, &MainWindow::onRootScanned);
    connect(m_import, &ImportThread::finished, this, &MainWindow::onImportFinished);
    connect(m_import, &ImportThread::libraryChanged, this, &MainWindow::scheduleRefresh);
    connect(m_import, &ImportThread::libraryChanged, this, [this] {
        // New publications may have joined the queue. If there's none, this finds that and stops.
        guarded(tr("Reading the lookups"), [&] {
            if (LookupRepository(m_session.database()).pendingCount() > 0)
                m_lookup->kick();
        });
    });

    connect(m_lookup, &LookupThread::busyChanged, this, [this](bool busy) {
        m_lookupLabel->setVisible(busy);
        m_lookupStop->setVisible(busy);
        if (busy)
            m_lookupLabel->setText(tr("Looking up details..."));
        else
            offerReviewIfDue();
    });
    connect(m_index, &IndexThread::busyChanged, this, [this](bool busy) {
        m_indexLabel->setVisible(busy);
        m_indexStop->setVisible(busy);
        if (busy)
            m_indexLabel->setText(tr("Indexing document text..."));
    });
    connect(m_index, &IndexThread::progress, this, [this](qint64, qint64 remaining, const QString &) {
        m_indexLabel->setText(tr("Indexing document text: %1 to go").arg(QLocale().toString(remaining)));
    });
    connect(m_index, &IndexThread::finished, this, [this](const import::FullTextReport &report) {
        if (report.documents > 0)
            refreshNow(); // More searches can find things now, and which files have no text is better known.
    });
    connect(m_index, &IndexThread::problem, this,
            [this](const QString &message) { statusBar()->showMessage(tr("Indexing problem: %1").arg(message), 15000); });
    connect(m_lookup, &LookupThread::progress, this, &MainWindow::onLookupProgress);
    connect(m_lookup, &LookupThread::publicationChanged, this, [this](qint64 id) {
        scheduleRefresh();
        if (m_details->currentId() == id)
            m_details->reload();
    });
    connect(m_lookup, &LookupThread::reviewChanged, this, &MainWindow::updateReviewButton);
    connect(m_lookup, &LookupThread::lookupsFinished, this, &MainWindow::onLookupsFinished);
    connect(m_lookup, &LookupThread::problem, this,
            [this](const QString &message) { statusBar()->showMessage(tr("%1 Trying again shortly.").arg(message), 15000); });
    connect(m_import, &ImportThread::failed, this, [this](const QString &message) {
        statusBar()->showMessage(tr("Import problem: %1").arg(message), 15000);
    });
    connect(m_watcher, &FolderWatcher::rootsChanged, this, [this](const QList<qint64> &roots) {
        if (!m_importStoppedByHand)
            m_import->scanRoots(roots);
    });
    connect(m_watcher, &FolderWatcher::pollDue, this, [this](const QList<qint64> &roots) {
        if (!m_importStoppedByHand && !m_import->isBusy())
            m_import->scanRoots(roots);
    });
}

void MainWindow::stopImport()
{
    m_importStoppedByHand = true;
    m_import->requestStop();
    m_importLabel->setText(tr("Stopping..."));
}

void MainWindow::restoreWindowState()
{
    SettingsStore &settings = m_session.settings();
    const QByteArray geometry = decodeState(settings.value(QStringLiteral("window.geometry")));
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    const QByteArray splitter = decodeState(settings.value(QStringLiteral("window.splitter")));
    if (!splitter.isEmpty())
        m_splitter->restoreState(splitter);
    const QByteArray columns = decodeState(settings.value(QStringLiteral("window.list_columns")));
    if (!columns.isEmpty())
        m_list->horizontalHeader()->restoreState(columns);

    m_sidebar->setVisible(settings.value(QStringLiteral("library.show_filters")).toBool());
    m_details->setVisible(settings.value(QStringLiteral("library.show_details")).toBool());
    m_showFilters->setChecked(!m_sidebar->isHidden());
    m_showDetails->setChecked(!m_details->isHidden());

    setViewMode(settings.value(QStringLiteral("library.view")).toString() == QLatin1StringView("list") ? ViewMode::List
                                                                                                        : ViewMode::Grid);
}

void MainWindow::saveWindowState()
{
    guarded(tr("Saving the window layout"), [this] {
        SettingsStore &settings = m_session.settings();
        settings.setValue(QStringLiteral("window.geometry"), encodeState(saveGeometry()));
        settings.setValue(QStringLiteral("window.splitter"), encodeState(m_splitter->saveState()));
        settings.setValue(QStringLiteral("window.list_columns"), encodeState(m_list->horizontalHeader()->saveState()));
    });
}

PublicationFilter MainWindow::currentFilter() const
{
    PublicationFilter filter;
    filter.searchText = m_search->text().trimmed();
    filter.tabTypes = typesForTab(kTabs.value(m_tabs->currentIndex(), LibraryTab::All));
    m_sidebar->applyTo(filter);
    filter.searchContents = m_searchInside->isChecked();
    return filter;
}

PublicationSort MainWindow::currentSort() const
{
    PublicationSort sort;
    sort.field = m_sortField;
    sort.ascending = m_sortAscending;
    return sort;
}

void MainWindow::applyFilters()
{
    m_searchTimer->stop();
    const PublicationFilter filter = currentFilter();
    m_model->setQuery(filter, currentSort());
    m_sidebarThread->request(++m_sidebarGeneration, filter);
    if (filter.searchContents) {
        const ParsedSearch parsed = parseSearch(filter.searchText);
        m_details->setContentsSearch(parsed.contentsMatch, parsed.contentsText);
    } else {
        m_details->setContentsSearch(QString(), QString());
    }
}

void MainWindow::scheduleRefresh()
{
    if (!m_refreshTimer->isActive())
        m_refreshTimer->start();
}

void MainWindow::refreshNow()
{
    m_refreshTimer->stop();
    m_model->refresh();
    m_sidebarThread->request(++m_sidebarGeneration, m_model->filter());
    m_covers->forgetMissing();
}

void MainWindow::onSearchTextChanged()
{
    // A search shows the best matches first, unless a sort has been picked by hand for this
    // search. Clearing the search goes back to the sort from before.
    const bool searching = !m_search->text().trimmed().isEmpty();
    if (!searching) {
        m_sortChosenForSearch = false;
        const SortField remembered
            = sortFieldFromSetting(m_session.settings().value(QStringLiteral("library.sort_field")).toString());
        m_sortField = remembered;
    } else if (!m_sortChosenForSearch) {
        m_sortField = SortField::Relevance;
    }
    syncSortControls();
    m_searchTimer->start();
}

void MainWindow::chooseSort(int comboIndex)
{
    const auto field = SortField(m_sortBox->itemData(comboIndex).toInt());
    if (field == m_sortField)
        return;
    m_sortField = field;
    if (field != SortField::Relevance) {
        if (m_search->text().trimmed().isEmpty())
            m_session.settings().setValue(QStringLiteral("library.sort_field"), sortFieldSetting(field));
        m_sortChosenForSearch = !m_search->text().trimmed().isEmpty();
    } else {
        m_sortChosenForSearch = false;
    }
    syncSortControls();
    applyFilters();
}

void MainWindow::onHeaderClicked(int section)
{
    std::optional<SortField> field;
    if (section == PublicationListModel::TitleColumn)
        field = SortField::Title;
    else if (section == PublicationListModel::AuthorsColumn)
        field = SortField::FirstAuthor;
    else if (section == PublicationListModel::YearColumn)
        field = SortField::Year;

    if (!field) {
        syncSortControls(); // Those columns can't be sorted on: put the arrow back where it was.
        return;
    }
    if (*field == m_sortField) {
        m_sortAscending = !m_sortAscending;
        m_session.settings().setValue(QStringLiteral("library.sort_ascending"), m_sortAscending);
        syncSortControls();
        applyFilters();
    } else {
        chooseSort(m_sortBox->findData(int(*field)));
    }
}

void MainWindow::syncSortControls()
{
    m_sortBox->setCurrentIndex(m_sortBox->findData(int(m_sortField)));
    const bool hasDirection = m_sortField != SortField::Relevance;
    m_sortDirection->setEnabled(hasDirection);
    m_sortDirection->setText(m_sortAscending ? QStringLiteral("↑") : QStringLiteral("↓"));
    m_sortDirection->setToolTip(m_sortAscending ? tr("Ascending") : tr("Descending"));

    QHeaderView *header = m_list->horizontalHeader();
    const QSignalBlocker blocker(header);
    const Qt::SortOrder order = m_sortAscending ? Qt::AscendingOrder : Qt::DescendingOrder;
    switch (m_sortField) {
    case SortField::Title: header->setSortIndicator(PublicationListModel::TitleColumn, order); break;
    case SortField::FirstAuthor: header->setSortIndicator(PublicationListModel::AuthorsColumn, order); break;
    case SortField::Year: header->setSortIndicator(PublicationListModel::YearColumn, order); break;
    default: header->setSortIndicator(-1, order); break;
    }
}

void MainWindow::setTab(LibraryTab tab)
{
    m_tabs->setCurrentIndex(int(kTabs.indexOf(tab)));
}

void MainWindow::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    m_gridButton->setChecked(mode == ViewMode::Grid);
    m_listButton->setChecked(mode == ViewMode::List);
    guarded(tr("Saving the view"), [&] {
        m_session.settings().setValue(QStringLiteral("library.view"),
                                      mode == ViewMode::List ? QStringLiteral("list") : QStringLiteral("grid"));
    });
    updateStack();

    const QModelIndex current = m_selection->currentIndex();
    if (current.isValid())
        activeView()->scrollTo(current);
}

QAbstractItemView *MainWindow::activeView() const
{
    if (m_viewMode == ViewMode::List)
        return m_list;
    return m_grid;
}

void MainWindow::updateStack()
{
    if (!m_model->hasResults() || m_model->rowCount() > 0) {
        m_stack->setCurrentWidget(activeView());
        return;
    }
    if (m_sidebar->onlyCollectionsChosen() && m_search->text().isEmpty() && m_tabs->currentIndex() == 0) {
        m_message->setText(tr("Nothing has been added to this yet.\n\nSelect some publications in the library, "
                              "right-click them and choose Add to collection."));
        m_messageButton->setText(tr("Clear search and filters"));
        m_messageButton->show();
    } else if (m_session.files().roots().isEmpty()) {
        m_message->setText(tr("Your library is empty.\n\nAdd a folder of papers and books. "
                              "They stay where they are, and Papertrail never changes them."));
        m_messageButton->setText(tr("Add folder..."));
        m_messageButton->show();
    } else if (m_import->isBusy() && !m_sidebar->hasSelection() && m_search->text().isEmpty()) {
        m_message->setText(tr("Importing your files. They'll appear here as they're found."));
        m_messageButton->hide();
    } else {
        m_message->setText(tr("No publications match."));
        m_messageButton->setText(tr("Clear search and filters"));
        m_messageButton->show();
    }
    m_stack->setCurrentWidget(m_messagePage);
}

void MainWindow::onResultsChanged()
{
    const int count = m_model->rowCount();
    m_count->setText(publicationCount(count));
    updateStack();

    // Rows may have moved since the selection was made. A selection that can't be vouched for is
    // cleared, rather than risking changes to the wrong publications.
    const qint64 selected = selectedCount();
    if (selected > 1 && count != m_lastCount) {
        m_selection->clear();
    } else if (selected == 1 && m_selectedId) {
        const int row = selectedRowRanges(m_selection).first().first;
        const std::optional<qint64> id = m_model->idAt(row);
        if (id && *id != *m_selectedId) {
            const QSignalBlocker blocker(m_selection);
            m_selection->clear();
            activeView()->viewport()->update();
        }
    }
    m_lastCount = count;
    onSelectionChanged();
}

void MainWindow::onRowsLoaded()
{
    if (m_waitingForRow >= 0 && m_model->idAt(m_waitingForRow))
        onSelectionChanged();
}

qint64 MainWindow::selectedCount() const
{
    qint64 total = 0;
    for (const RowRange &range : selectedRowRanges(m_selection))
        total += range.last - range.first + 1;
    return total;
}

void MainWindow::onSelectionChanged()
{
    m_waitingForRow = -1;
    const qint64 count = selectedCount();
    if (count > 1) {
        m_selectedId.reset();
        m_details->showSelectionCount(count);
    } else if (count == 1) {
        const int row = selectedRowRanges(m_selection).first().first;
        if (const std::optional<qint64> id = m_model->idAt(row)) {
            if (id != m_selectedId || m_details->currentId() != id) {
                m_selectedId = id;
                m_details->showPublication(*id);
            }
        } else {
            m_waitingForRow = row;
        }
    } else if (!m_details->currentId()) {
        // With nothing selected, the last publication stays on show if there was just one.
        m_details->showNothing();
    }
}

QList<qint64> MainWindow::selectedIds()
{
    const QList<RowRange> ranges = selectedRowRanges(m_selection);
    if (ranges.isEmpty())
        return {};
    if (ranges.size() == 1 && ranges.first().first == ranges.first().last) {
        if (const auto id = m_model->idAt(ranges.first().first))
            return {*id};
    }

    const QList<qint64> all = m_session.query().ids(m_model->filter(), m_model->sort());
    QList<qint64> ids;
    for (const RowRange &range : ranges) {
        for (int row = range.first; row <= range.last && row < all.size(); ++row)
            ids << all.at(row);
    }
    return ids;
}

bool MainWindow::guarded(const QString &what, const std::function<void()> &action)
{
    try {
        action();
        return true;
    } catch (const std::exception &error) {
        QMessageBox box(QMessageBox::Warning, tr("Papertrail"), tr("%1 failed: %2").arg(what, QString::fromUtf8(error.what())),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return false;
    }
}

void MainWindow::tagSelected(const QString &path)
{
    guarded(tr("Adding the tag"), [&] {
        const QList<qint64> ids = selectedIds();
        if (ids.isEmpty())
            return;
        const qint64 tag = m_session.tags().ensurePath(path);
        m_bulk->enqueue(tr("Tagging"), ids, [tag](Database &db, const QList<qint64> &batch) {
            TagRepository(db).assign(batch, tag);
        });
    });
}

void MainWindow::setSelectedReadStatus(ReadStatus status)
{
    guarded(tr("Setting the status"), [&] {
        m_bulk->enqueue(tr("Setting the status"), selectedIds(), [status](Database &db, const QList<qint64> &batch) {
            PublicationRepository(db).setReadStatus(batch, status);
        });
    });
}

void MainWindow::markSelectedChecked()
{
    guarded(tr("Marking as checked"), [&] {
        m_bulk->enqueue(tr("Marking as checked"), selectedIds(), [](Database &db, const QList<qint64> &batch) {
            PublicationRepository(db).setNeedsReview(batch, false);
        });
    });
}

void MainWindow::saveEdit(Publication publication)
{
    guarded(tr("Saving the changes"), [&] {
        m_session.publications().updateWithProvenance(publication, QStringLiteral("manual"));
        m_details->reload();
        scheduleRefresh();
    });
}

void MainWindow::editPublication(qint64 id)
{
    std::optional<Publication> publication;
    if (!guarded(tr("Reading the publication"), [&] { publication = m_session.publications().get(id); }) || !publication)
        return;
    MetadataEditor editor(*publication, this);
    if (editor.exec() == QDialog::Accepted)
        saveEdit(editor.result());
}

std::optional<QString> MainWindow::firstFilePath(qint64 publicationId)
{
    const std::optional<Publication> publication = m_session.publications().get(publicationId);
    if (!publication)
        return std::nullopt;
    const qint64 owner = publication->parentId.value_or(publicationId);
    for (const FileRecord &file : m_session.files().filesFor(owner)) {
        const QString path = m_session.files().absolutePath(file.id);
        if (QFileInfo::exists(path))
            return path;
    }
    return std::nullopt;
}

QList<FileRecord> MainWindow::formatChoices(qint64 id)
{
    QList<FileRecord> choices;
    QStringList seen;
    guarded(tr("Reading the files"), [&] {
        const std::optional<Publication> publication = m_session.publications().get(id);
        if (!publication)
            return;
        for (const FileRecord &file : m_session.files().filesFor(publication->parentId.value_or(id))) {
            // The first copy that's there now stands for its format.
            if (!seen.contains(file.format) && QFileInfo::exists(m_session.files().absolutePath(file.id))) {
                seen << file.format;
                choices << file;
            }
        }
    });
    return choices;
}

void MainWindow::openOrChooseFormat(qint64 id)
{
    const QList<FileRecord> choices = formatChoices(id);
    if (choices.size() <= 1) {
        openPublication(id);
        return;
    }

    QMenu menu(this);
    menu.setTitle(tr("Open as"));
    for (const FileRecord &file : choices) {
        const QString label = file.pageCount ? tr("%1 (%2 pages)").arg(file.format.toUpper()).arg(*file.pageCount)
                                             : file.format.toUpper();
        const qint64 fileId = file.id;
        menu.addAction(label, this, [this, id, fileId] { openPublicationFile(id, fileId); });
    }
    menu.exec(QCursor::pos());
}

ReaderTab *MainWindow::openPublication(qint64 id)
{
    const QList<FileRecord> choices = formatChoices(id);
    if (choices.isEmpty()) {
        statusBar()->showMessage(tr("This publication's file can't be found right now."), 8000);
        return nullptr;
    }
    return openPublicationFile(id, choices.first().id);
}

ReaderTab *MainWindow::openPublicationFile(qint64 id, qint64 fileId)
{
    for (int i = 1; i < m_documentTabs->count(); ++i) {
        auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->widget(i));
        if (reader && reader->publicationId() == id && reader->fileId() == fileId) {
            m_documentTabs->setCurrentIndex(i);
            return reader;
        }
    }

    ReaderTab *reader = nullptr;
    guarded(tr("Opening the file"), [&] {
        const std::optional<FileRecord> file = m_session.files().get(fileId);
        const std::optional<Publication> publication = m_session.publications().get(id);
        const QString path = file ? m_session.files().absolutePath(fileId) : QString();
        if (!file || !publication || !QFileInfo::exists(path)) {
            statusBar()->showMessage(tr("This publication's file can't be found right now."), 8000);
            return;
        }
        const QString converted = file->sha256.isEmpty()
            ? QString()
            : import::convertedPathFor(m_session.paths().cacheDirectory, file->sha256);
        reader = new ReaderTab(m_session, id, fileId, file->format, path, converted, m_documentTabs);
        connect(reader, &ReaderTab::openElsewhereRequested, this,
                [](const QString &file) { QDesktopServices::openUrl(QUrl::fromLocalFile(file)); });
        connect(reader, &ReaderTab::statusMessage, this,
                [this](const QString &message) { statusBar()->showMessage(message, 8000); });

        // In a tab bar, & marks a shortcut key, and a long title would crowd out the other tabs.
        QString title = publication->title;
        title.replace(u'&', QStringLiteral("&&"));
        QString shortTitle = QFontMetrics(font()).elidedText(title, Qt::ElideRight, 220);
        if (formatChoices(id).size() > 1)
            shortTitle += QStringLiteral(" (%1)").arg(file->format.toUpper());
        const int index = m_documentTabs->addTab(reader, shortTitle);
        m_documentTabs->setTabToolTip(index, publication->title);
        m_documentTabs->setCurrentIndex(index);
    });
    return reader;
}

void MainWindow::closeDocumentTab(int index)
{
    if (index <= 0)
        return; // The library stays.
    QWidget *tab = m_documentTabs->widget(index);
    m_documentTabs->removeTab(index);
    if (tab == m_reviewTab)
        m_reviewTab = nullptr;
    delete tab;
}

void MainWindow::openPublicationElsewhere(qint64 id)
{
    guarded(tr("Opening the file"), [&] {
        const std::optional<QString> path = firstFilePath(id);
        if (path)
            QDesktopServices::openUrl(QUrl::fromLocalFile(*path));
        else
            statusBar()->showMessage(tr("This publication's file can't be found right now."), 8000);
    });
}

void MainWindow::showPublicationInFolder(qint64 id)
{
    guarded(tr("Showing the folder"), [&] {
        const std::optional<QString> path = firstFilePath(id);
        if (path)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(*path).absolutePath()));
        else
            statusBar()->showMessage(tr("This publication's file can't be found right now."), 8000);
    });
}

void MainWindow::showContextMenu(const QPoint &position)
{
    QAbstractItemView *view = activeView();
    const QModelIndex index = view->indexAt(position);
    if (!index.isValid())
        return;
    if (!m_selection->isSelected(index) && !m_selection->rowIntersectsSelection(index.row(), QModelIndex())) {
        const auto flags = m_viewMode == ViewMode::List ? QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
                                                        : QItemSelectionModel::ClearAndSelect;
        m_selection->setCurrentIndex(index, flags);
    }

    QMenu menu(this);
    const qint64 count = selectedCount();
    if (count == 1) {
        if (const auto id = m_model->idAt(index.row())) {
            const qint64 publication = *id;
            const QList<FileRecord> choices = formatChoices(publication);
            if (choices.size() > 1) {
                for (const FileRecord &file : choices) {
                    const qint64 fileId = file.id;
                    menu.addAction(tr("Open %1").arg(file.format.toUpper()), this,
                                   [this, publication, fileId] { openPublicationFile(publication, fileId); });
                }
            } else {
                menu.addAction(tr("Open"), this, [this, publication] { openPublication(publication); });
            }
            menu.addAction(tr("Open in another app"), this, [this, publication] { openPublicationElsewhere(publication); });
            menu.addAction(tr("Show in folder"), this, [this, publication] { showPublicationInFolder(publication); });
            menu.addAction(tr("Edit..."), this, [this, publication] { editPublication(publication); });
            guarded(tr("Reading the publication"), [&] {
                const std::optional<Publication> details = m_session.publications().get(publication);
                if (details && !details->parentId && canHoldArticles(details->type)) {
                    menu.addAction(tr("Create articles from contents..."), this, [this, publication] { showContentsDialog(publication); });
                    menu.addAction(tr("Add an article..."), this, [this, publication] { showAddArticleDialog(publication); });
                } else if (details && details->parentId) {
                    menu.addAction(tr("Delete article..."), this, [this, publication] { askToDeleteArticle(publication); });
                }
            });
            menu.addSeparator();
        }
    }
    menu.addAction(tr("Add tag..."), this, &MainWindow::askForBulkTag);
    QMenu *status = menu.addMenu(tr("Set status"));
    for (const ReadStatus value : {ReadStatus::Unread, ReadStatus::Reading, ReadStatus::Read})
        status->addAction(readStatusLabel(value), this, [this, value] { setSelectedReadStatus(value); });
    menu.addAction(tr("Mark as checked"), this, &MainWindow::markSelectedChecked);
    menu.addAction(tr("Look up details online"), this, &MainWindow::askAndLookUpSelected);
    menu.addSeparator();
    addCollectionActions(&menu);
    if (count >= 2 && count <= kMaxToCombine) {
        menu.addSeparator();
        menu.addAction(tr("Combine into one publication..."), this, [this] {
            QMessageBox box(QMessageBox::Question, tr("Combine publications"),
                            tr("Treat these as one publication, such as the same book in different formats?\n\n"
                               "The first one's details are kept, and the others only fill in what it's missing. "
                               "Tags, notes and highlights all carry over. Your files aren't changed."),
                            QMessageBox::Yes | QMessageBox::Cancel, this);
            box.setTextFormat(Qt::PlainText);
            if (box.exec() == QMessageBox::Yes)
                combineSelected();
        });
    }
    menu.exec(view->viewport()->mapToGlobal(position));
}

void MainWindow::showContentsDialog(qint64 volumeId)
{
    ContentsDialog dialog(m_session, volumeId, this);
    if (dialog.exec() == QDialog::Accepted) {
        statusBar()->showMessage(tr("Created %1.").arg(publicationCount(dialog.createdCount())), 8000);
        m_details->reload();
        refreshNow();
    }
}

void MainWindow::showAddArticleDialog(qint64 volumeId)
{
    guarded(tr("Adding the article"), [&] {
        std::optional<int> pageCount;
        for (const FileRecord &file : m_session.files().filesFor(volumeId)) {
            if (file.pageCount) {
                pageCount = file.pageCount;
                break;
            }
        }
        AddArticleDialog dialog(pageCount, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        createArticles(m_session.database(), volumeId, {dialog.draft()}, false);
        m_details->reload();
        refreshNow();
    });
}

void MainWindow::askToDeleteArticle(qint64 articleId)
{
    guarded(tr("Deleting the article"), [&] {
        const std::optional<Publication> article = m_session.publications().get(articleId);
        if (!article || !article->parentId)
            return;
        QMessageBox box(QMessageBox::Question, tr("Delete article"),
                        tr("Delete the article \"%1\"? Its tags and notes will be gone for good. The volume and its file aren't touched.")
                            .arg(article->title),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Yes)
            return;

        // These save as they go, so they come first.
        for (int i = m_documentTabs->count() - 1; i > 0; --i) {
            const auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->widget(i));
            if (reader && reader->publicationId() == articleId)
                closeDocumentTab(i);
        }
        if (m_details->currentId() == articleId) {
            m_selectedId.reset();
            m_details->showNothing();
        }
        m_session.publications().remove(articleId);
        refreshNow();
    });
}

void MainWindow::kickIndexing()
{
    if (m_indexContents->isChecked() && !m_import->isBusy())
        m_index->kick();
}

void MainWindow::openAtMatch(qint64 publicationId, qint64 fileId, int page, const QString &text)
{
    if (ReaderTab *reader = openPublicationFile(publicationId, fileId))
        reader->showMatch(page, text);
}

void MainWindow::addCollectionActions(QMenu *menu)
{
    QMenu *addTo = menu->addMenu(tr("Add to collection"));
    QMenu *removeFrom = nullptr;
    guarded(tr("Reading the collections"), [&] {
        // Working out what's in what means listing what's selected, which is too slow to do just to open
        // a menu if that's a large part of a big library. Then, every collection is offered both ways.
        const qint64 selected = selectedCount();
        const bool known = selected <= kMostToCheckCollectionsFor;
        const QHash<qint64, qint64> holding
            = known ? m_session.collections().membershipCounts(selectedIds()) : QHash<qint64, qint64>();

        for (const Collection &collection : m_session.collections().all()) {
            const qint64 id = collection.id;
            const QString name = escapedForMenu(collection.name);
            const qint64 inIt = holding.value(id);
            const bool partly = known && inIt > 0 && inIt < selected;
            QAction *add = addTo->addAction(partly ? tr("%1 (%2 of %3 already in it)").arg(name).arg(inIt).arg(selected) : name, this,
                                            [this, id] { addSelectedToCollection(id); });
            if (known && inIt == selected) {
                add->setCheckable(true);
                add->setChecked(true);
                add->setEnabled(false);
            }
            if (!known || inIt > 0) {
                if (!removeFrom)
                    removeFrom = menu->addMenu(tr("Remove from collection"));
                removeFrom->addAction(partly ? tr("%1 (%2 of %3 in it)").arg(name).arg(inIt).arg(selected) : name, this,
                                      [this, id] { removeSelectedFromCollection(id); });
            }
        }
    });
    if (!addTo->isEmpty())
        addTo->addSeparator();
    addTo->addAction(tr("New collection..."), this, &MainWindow::askToAddSelectedToNewCollection);
}

void MainWindow::addSelectedToCollection(qint64 id)
{
    guarded(tr("Adding to the collection"), [&] {
        const std::optional<Collection> collection = m_session.collections().get(id);
        if (!collection)
            return;
        m_bulk->enqueue(tr("Adding to %1").arg(collection->name), selectedIds(), [id](Database &db, const QList<qint64> &batch) {
            CollectionRepository(db).add(id, batch);
        });
    });
}

void MainWindow::removeSelectedFromCollection(qint64 id)
{
    guarded(tr("Removing from the collection"), [&] {
        const std::optional<Collection> collection = m_session.collections().get(id);
        if (!collection)
            return;
        m_bulk->enqueue(tr("Removing from %1").arg(collection->name), selectedIds(), [id](Database &db, const QList<qint64> &batch) {
            CollectionRepository(db).removeFrom(id, batch);
        });
    });
}

void MainWindow::askToAddSelectedToNewCollection()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New collection"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    guarded(tr("Creating the collection"), [&] {
        const qint64 id = m_session.collections().create(name);
        addSelectedToCollection(id);
    });
}

void MainWindow::combineSelected()
{
    guarded(tr("Combining the publications"), [&] {
        const QList<qint64> ids = selectedIds();
        if (ids.size() < 2)
            return;
        Transaction transaction(m_session.database());
        for (qsizetype i = 1; i < ids.size(); ++i)
            mergePublications(m_session.database(), ids.first(), ids.at(i), QStringLiteral("manual"));
        transaction.commit();
        m_selection->clear();
        m_details->showPublication(ids.first());
        statusBar()->showMessage(tr("Combined %1 into one.").arg(publicationCount(ids.size())), 5000);
        refreshNow();
    });
}

void MainWindow::askForBulkTag()
{
    QStringList paths;
    guarded(tr("Reading the tags"), [&] {
        for (const Tag &tag : m_session.tags().all())
            paths << m_session.tags().path(tag.id);
    });
    bool ok = false;
    const QString path = QInputDialog::getItem(this, tr("Add tag"), tr("Tag, such as Engineering > FEM:"), paths, 0, true, &ok);
    if (ok && !path.trimmed().isEmpty())
        tagSelected(path.trimmed());
}

void MainWindow::chooseFolderToAdd()
{
    QString start;
    guarded(tr("Reading settings"), [&] {
        start = m_session.settings().value(QStringLiteral("library.last_import_folder")).toString();
    });
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Add a folder to the library"), start);
    if (folder.isEmpty())
        return;

    // The first time, this sets how lookups work from now on. After that, it's only for this folder.
    bool chosen = false;
    guarded(tr("Reading settings"), [&] { chosen = m_session.settings().value(QStringLiteral("metadata.mode_chosen")).toBool(); });
    OnlineLookupDialog dialog(tr("Adding %1.").arg(QDir::toNativeSeparators(folder)),
                              chosen ? lookupMode() : QStringLiteral("online_review"), QString(), false, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!chosen)
        setLookupMode(dialog.mode(), QString());
    addFolder(folder, dialog.mode());
}

QString MainWindow::lookupMode()
{
    QString mode = QStringLiteral("offline");
    guarded(tr("Reading settings"), [&] {
        if (m_session.settings().value(QStringLiteral("metadata.mode_chosen")).toBool())
            mode = m_session.settings().value(QStringLiteral("metadata.mode")).toString();
    });
    return mode;
}

void MainWindow::setLookupMode(const QString &mode, const QString &email)
{
    guarded(tr("Saving the setting"), [&] {
        SettingsStore &settings = m_session.settings();
        settings.setValue(QStringLiteral("metadata.mode"), mode);
        settings.setValue(QStringLiteral("metadata.mode_chosen"), true);
        if (!email.isNull())
            settings.setValue(QStringLiteral("metadata.crossref_email"), email);
        m_import->setLookupMode(mode == QLatin1StringView("offline") ? QString() : mode);
        if (mode == QLatin1StringView("offline")) {
            m_lookup->stopLookups();
            LookupRepository(m_session.database()).clearPending();
        }
    });
}

void MainWindow::showLookupSettings()
{
    QString email;
    guarded(tr("Reading settings"), [&] { email = m_session.settings().value(QStringLiteral("metadata.crossref_email")).toString(); });
    OnlineLookupDialog dialog(QString(), lookupMode(), email, true, this);
    if (dialog.exec() == QDialog::Accepted)
        setLookupMode(dialog.mode(), dialog.email().isNull() ? QStringLiteral("") : dialog.email());
}

bool MainWindow::makeSureLookupModeIsChosen()
{
    if (lookupMode() != QLatin1StringView("offline"))
        return true;
    OnlineLookupDialog dialog(tr("You're currently offline. Looking up details needs one of the online choices."),
                              QStringLiteral("online_review"), QString(), false, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    setLookupMode(dialog.mode(), QString());
    return lookupMode() != QLatin1StringView("offline");
}

void MainWindow::lookUp(const QList<qint64> &ids)
{
    const QString mode = lookupMode();
    if (mode == QLatin1StringView("offline") || ids.isEmpty())
        return;
    guarded(tr("Queueing the lookups"), [&] {
        LookupRepository lookups(m_session.database());
        Transaction transaction(m_session.database());
        for (const qint64 id : ids)
            lookups.resolveCandidates(id);
        lookups.enqueue(ids, mode, true);
        transaction.commit();
        updateReviewButton();
        m_lookup->kick();
    });
}

void MainWindow::askAndLookUpLibrary()
{
    if (!makeSureLookupModeIsChosen())
        return;
    QList<qint64> ids;
    guarded(tr("Reading the library"), [&] { ids = m_session.query().ids({}, {}); });
    QMessageBox box(QMessageBox::Question, tr("Look up details"),
                    tr("Look up details for all %1?\n\nThis goes at the pace each source allows, so a large library can "
                       "take a while. It carries on in the background, and from the same place if it's stopped.")
                        .arg(publicationCount(ids.size())),
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    if (box.exec() == QMessageBox::Yes)
        lookUp(ids);
}

void MainWindow::askAndLookUpSelected()
{
    if (makeSureLookupModeIsChosen())
        lookUp(selectedIds());
}

void MainWindow::showReviewTab()
{
    if (!m_reviewTab) {
        m_reviewTab = new ReviewTab(m_session, m_documentTabs);
        connect(m_reviewTab, &ReviewTab::libraryEdited, this, [this] {
            updateReviewButton();
            m_details->reload();
            scheduleRefresh();
        });
        connect(m_reviewTab, &ReviewTab::openRequested, this, [this](qint64 id) { openOrChooseFormat(id); });
        connect(m_reviewTab, &QObject::destroyed, this, [this] { m_reviewTab = nullptr; });
        m_documentTabs->addTab(m_reviewTab, tr("To review"));
    } else {
        m_reviewTab->reload();
    }
    m_documentTabs->setCurrentWidget(m_reviewTab);
    if (m_reviewOffer)
        m_reviewOffer->close();
}

void MainWindow::updateReviewButton()
{
    guarded(tr("Reading the lookups"), [&] {
        const qint64 count = LookupRepository(m_session.database()).waitingForReviewCount();
        m_reviewButton->setText(tr("To review: %1").arg(QLocale().toString(count)));
        m_reviewButton->setVisible(count > 0);
        m_reviewCountSeen = qMin(m_reviewCountSeen, count);
        if (m_reviewTab)
            m_reviewTab->reload();
        if (m_reviewOffer && count == 0)
            m_reviewOffer->close();
    });
}

// Comes up once everything has finished, so that a large import doesn't bring up one after another. It
// doesn't block the window or take the keyboard, and it waits rather than interrupt someone reading.
void MainWindow::offerReviewIfDue()
{
    guarded(tr("Reading the lookups"), [&] {
        LookupRepository lookups(m_session.database());
        const qint64 count = lookups.waitingForReviewCount();
        if (count <= m_reviewCountSeen || m_reviewOffer)
            return;
        if (m_import->isBusy() || m_lookup->isBusy() || lookups.pendingCount() > 0)
            return;
        if (m_documentTabs->currentWidget() == m_reviewTab && m_reviewTab) {
            m_reviewCountSeen = count; // They're already looking at it.
            return;
        }
        if (m_documentTabs->currentIndex() != 0 || QApplication::activeModalWidget() || isMinimized())
            return;

        m_reviewCountSeen = count;
        auto *box = new QMessageBox(QMessageBox::Question, tr("Details to review"),
                                    tr("Online lookups found details for %1 that need checking.").arg(publicationCount(count)),
                                    QMessageBox::NoButton, this);
        box->setTextFormat(Qt::PlainText);
        box->setInformativeText(tr("You can compare them now, or later using To review in the toolbar."));
        box->setWindowModality(Qt::NonModal);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setAttribute(Qt::WA_ShowWithoutActivating);
        QPushButton *review = box->addButton(tr("Review now"), QMessageBox::AcceptRole);
        review->setObjectName(QStringLiteral("reviewNow"));
        QPushButton *later = box->addButton(tr("Later"), QMessageBox::RejectRole);
        later->setObjectName(QStringLiteral("reviewLater"));
        box->setDefaultButton(review);
        box->setEscapeButton(later);
        connect(review, &QPushButton::clicked, this, &MainWindow::showReviewTab);
        m_reviewOffer = box;
        box->show();
    });
}

void MainWindow::onLookupProgress(qint64 done, qint64 remaining, const QString &source)
{
    const QString from = source.isEmpty() ? QString() : tr(", asking %1").arg(metadata::providerLabel(source));
    m_lookupLabel->setText(tr("Looked up %1, with %2 to go%3").arg(QLocale().toString(done), QLocale().toString(remaining), from));
}

void MainWindow::onLookupsFinished(const metadata::LookupSummary &summary)
{
    refreshNow();
    updateReviewButton();
    if (m_details->currentId())
        m_details->reload();
    if (summary.stopped) {
        statusBar()->showMessage(tr("Lookups stopped. They carry on from the same place next time."), 10000);
        return;
    }
    if (summary.applied + summary.forReview + summary.notFound == 0)
        return;
    QStringList parts;
    parts << tr("%1 filled in").arg(summary.applied);
    if (summary.forReview > 0)
        parts << tr("%1 to review").arg(summary.forReview);
    if (summary.notFound > 0)
        parts << tr("%1 not found").arg(summary.notFound);
    statusBar()->showMessage(tr("Lookups finished: %1.").arg(parts.join(QStringLiteral(", "))), 15000);
}

void MainWindow::addFolder(const QString &path, const std::optional<QString> &lookupOverride)
{
    guarded(tr("Adding the folder"), [&] {
        const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
        if (lookupOverride) {
            m_lookupOverride = lookupOverride;
            m_import->setLookupMode(*lookupOverride == QLatin1StringView("offline") ? QString() : *lookupOverride);
        }
        m_session.settings().setValue(QStringLiteral("library.last_import_folder"), folder);
        m_importStoppedByHand = false;

        for (const LibraryRoot &root : m_session.files().roots()) {
            if (import::isPathInside(folder, root.path)) {
                statusBar()->showMessage(tr("That folder is already in the library, so it's being scanned again."), 8000);
                m_import->scanRoots({root.id});
                return;
            }
        }
        // A folder that was removed comes back with everything it had.
        for (const LibraryRoot &root : m_session.files().removedRoots()) {
            if (import::isPathInside(folder, root.path) && import::isPathInside(root.path, folder)) {
                m_session.files().restoreRoot(root.id);
                m_import->scanRoots({root.id});
                afterFoldersChanged();
                return;
            }
        }
        // The top of a drive has no folder name of its own.
        const QString name = QFileInfo(folder).fileName();
        const qint64 root = m_session.files().addRoot(name.isEmpty() ? folder : name, folder);
        m_import->scanRoots({root});
        if (m_foldersDialog)
            m_foldersDialog->reload();
        updateStack();
    });
}

void MainWindow::rescanAll()
{
    guarded(tr("Scanning"), [&] {
        QList<qint64> ids;
        for (const LibraryRoot &root : m_session.files().roots())
            ids << root.id;
        m_importStoppedByHand = false;
        if (!ids.isEmpty())
            m_import->scanRoots(ids);
    });
}

void MainWindow::showFoldersDialog()
{
    LibraryFoldersDialog dialog(m_session, this);
    m_foldersDialog = &dialog;
    dialog.setImportRunning(m_import->isBusy());
    connect(&dialog, &LibraryFoldersDialog::addRequested, this, &MainWindow::chooseFolderToAdd);
    connect(&dialog, &LibraryFoldersDialog::rescanRequested, this, [this](qint64 root) {
        m_importStoppedByHand = false;
        m_import->scanRoots({root});
    });
    connect(&dialog, &LibraryFoldersDialog::changeLocationRequested, this, &MainWindow::changeRootLocation);
    connect(&dialog, &LibraryFoldersDialog::removeRequested, this, &MainWindow::removeRoot);
    connect(&dialog, &LibraryFoldersDialog::forgetRequested, this, &MainWindow::confirmForgetHidden);
    dialog.exec();
    m_foldersDialog = nullptr;
    offerReviewIfDue();
}

void MainWindow::changeRootLocation(qint64 rootId)
{
    guarded(tr("Changing the location"), [&] {
        const auto root = m_session.files().root(rootId);
        if (!root)
            return;
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Where is this folder now?"), root->path);
        if (folder.isEmpty())
            return;
        m_session.files().repointRoot(rootId, QDir::cleanPath(QDir::fromNativeSeparators(folder)));
        m_watcher->removeRoot(rootId);
        m_import->scanRoots({rootId});
        if (m_foldersDialog)
            m_foldersDialog->reload();
    });
}

void MainWindow::removeRoot(qint64 rootId)
{
    guarded(tr("Removing the folder"), [&] {
        const auto root = m_session.files().root(rootId);
        if (!root)
            return;
        const FileRepository::RemovalImpact impact = m_session.files().removalImpact(rootId);
        QMessageBox box(QMessageBox::Question, tr("Remove folder"),
                        tr("Stop indexing %1?\n\nNothing is deleted from your disk.").arg(QDir::toNativeSeparators(root->path)),
                        QMessageBox::NoButton, m_foldersDialog ? static_cast<QWidget *>(m_foldersDialog) : this);
        box.setTextFormat(Qt::PlainText);

        QString details;
        if (impact.leaving > 0) {
            details = tr("%1 will leave the library.\n\n"
                         "If you keep their details, they're hidden, and their tags, notes and reading places "
                         "come back if you add this folder again.\n\n"
                         "If you delete their details, those are gone for good.")
                          .arg(publicationCount(impact.leaving));
        }
        if (impact.staying > 0) {
            details += (details.isEmpty() ? QString() : QStringLiteral("\n\n"))
                + tr("%1 also in another folder will stay.").arg(publicationCount(impact.staying));
        }
        box.setInformativeText(details);

        QPushButton *remove = box.addButton(impact.leaving > 0 ? tr("Remove, keep details") : tr("Remove"), QMessageBox::AcceptRole);
        remove->setObjectName(QStringLiteral("removeKeep"));
        QPushButton *forget = nullptr;
        if (impact.leaving > 0) {
            forget = box.addButton(tr("Remove, delete details"), QMessageBox::DestructiveRole);
            forget->setObjectName(QStringLiteral("removeForget"));
        }
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();

        if (box.clickedButton() == remove)
            removeRootKeepingDetails(rootId);
        else if (forget && box.clickedButton() == forget)
            removeRootAndDetails(rootId);
    });
}

void MainWindow::removeRootKeepingDetails(qint64 rootId)
{
    guarded(tr("Removing the folder"), [&] {
        m_watcher->removeRoot(rootId);
        m_session.files().hideRoot(rootId);
        afterFoldersChanged();
    });
}

void MainWindow::removeRootAndDetails(qint64 rootId)
{
    guarded(tr("Removing the folder"), [&] {
        m_watcher->removeRoot(rootId);
        m_session.files().hideRoot(rootId);
        const QList<qint64> ids = m_session.files().hiddenIn(rootId);
        letGoOfHidden(); // Readers and notes save as they close, so this has to come first.
        m_session.files().removeRoot(rootId);
        deletePublications(ids);
        afterFoldersChanged();
    });
}

void MainWindow::confirmForgetHidden()
{
    guarded(tr("Deleting details"), [&] {
        const qint64 hidden = m_session.files().hiddenCount();
        QMessageBox box(QMessageBox::Warning, tr("Delete details"),
                        tr("Delete the details of %1 from removed folders?\n\nTheir tags, notes and reading places "
                           "will be gone for good. Your files aren't touched.")
                            .arg(publicationCount(hidden)),
                        QMessageBox::NoButton, m_foldersDialog ? static_cast<QWidget *>(m_foldersDialog) : this);
        box.setTextFormat(Qt::PlainText);
        QPushButton *confirm = box.addButton(tr("Delete details"), QMessageBox::DestructiveRole);
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() == confirm)
            forgetHiddenPublications();
    });
}

void MainWindow::forgetHiddenPublications()
{
    guarded(tr("Deleting details"), [&] {
        const QList<qint64> ids = m_session.files().hiddenPublications();
        letGoOfHidden();
        for (const LibraryRoot &root : m_session.files().removedRoots())
            m_session.files().removeRoot(root.id);
        deletePublications(ids);
        afterFoldersChanged();
    });
}

// They stay hidden until they've gone, so this can take as long as it needs.
void MainWindow::deletePublications(const QList<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    m_bulk->enqueue(tr("Deleting details"), ids, [](Database &db, const QList<qint64> &batch) {
        PublicationRepository publications(db);
        for (const qint64 id : batch)
            publications.remove(id);
    });
}

void MainWindow::letGoOfHidden()
{
    const QList<qint64> hidden = m_session.files().hiddenPublications();
    const QSet<qint64> ids(hidden.cbegin(), hidden.cend());
    for (int i = m_documentTabs->count() - 1; i > 0; --i) {
        const auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->widget(i));
        if (reader && ids.contains(reader->publicationId()))
            closeDocumentTab(i);
    }
    if (const std::optional<qint64> shown = m_details->currentId(); shown && ids.contains(*shown)) {
        m_selectedId.reset();
        m_details->showNothing();
    }
}

void MainWindow::afterFoldersChanged()
{
    letGoOfHidden();
    if (m_foldersDialog)
        m_foldersDialog->reload();
    if (m_reviewTab)
        m_reviewTab->reload();
    updateReviewButton();
    refreshNow();
    updateStack();
}

void MainWindow::onImportBusyChanged(bool busy)
{
    if (busy) {
        m_importLabel->setText(tr("Looking for files..."));
        m_importBar->setRange(0, 0);
        m_importShowDelay->start();
    } else {
        m_importShowDelay->stop();
        m_importLabel->hide();
        m_importBar->hide();
        m_importStop->hide();
    }
    if (m_foldersDialog)
        m_foldersDialog->setImportRunning(busy);
    updateStack();
    if (!busy) {
        offerReviewIfDue();
        kickIndexing();
    }
}

void MainWindow::onImportProgress(qint64 done, qint64 remaining, const QString &file)
{
    const qint64 total = done + remaining;
    m_importBar->setRange(0, int(qMin<qint64>(total, 1 << 30)));
    m_importBar->setValue(int(qMin<qint64>(done, 1 << 30)));
    const QFontMetrics metrics(m_importLabel->font());
    m_importLabel->setText(metrics.elidedText(QFileInfo(file).fileName(), Qt::ElideMiddle, 280));
    m_importLabel->setToolTip(file);
}

void MainWindow::onRootScanned(qint64 rootId, const import::ScanReport &report, const QStringList &folders)
{
    if (report.rootAvailable) {
        m_watcher->setFolders(rootId, folders);
        m_watcher->noteScanDuration(report.elapsedMs);
    }
    if (report.stillChanging > 0 && m_recheckCounts.value(rootId) < kMaxRechecks) {
        m_recheckCounts[rootId] += 1;
        m_watcher->scheduleRecheck(rootId);
    } else if (report.stillChanging == 0) {
        m_recheckCounts.remove(rootId);
    }
    if (!report.rootAvailable)
        statusBar()->showMessage(tr("A library folder isn't available right now. Its files are marked as offline."), 8000);
}

void MainWindow::onImportFinished(const import::ImportReport &report)
{
    refreshNow();
    // A choice made for one folder only lasts as long as that folder's import.
    if (m_lookupOverride) {
        m_lookupOverride.reset();
        const QString mode = lookupMode();
        m_import->setLookupMode(mode == QLatin1StringView("offline") ? QString() : mode);
    }
    guarded(tr("Reading the lookups"), [&] {
        if (LookupRepository(m_session.database()).pendingCount() > 0)
            m_lookup->kick();
    });
    if (report.stoppedEarly) {
        statusBar()->showMessage(tr("Import stopped. It carries on from the same place next time."), 10000);
        return;
    }
    if (report.publicationsCreated == 0 && report.duplicates == 0 && report.moved == 0 && report.updated == 0
        && report.unreadable == 0 && report.reclassified == 0 && report.formatsJoined == 0) {
        return;
    }
    QStringList parts;
    parts << tr("%1 added").arg(report.publicationsCreated);
    if (report.duplicates > 0)
        parts << tr("%1 duplicate files linked").arg(report.duplicates);
    if (report.moved > 0)
        parts << tr("%1 moved").arg(report.moved);
    if (report.formatsJoined > 0)
        parts << tr("%1 recognised as another format of a publication").arg(report.formatsJoined);
    if (report.reclassified > 0)
        parts << tr("%1 moved out of Preprints").arg(report.reclassified);
    if (report.updated > 0)
        parts << tr("%1 updated").arg(report.updated);
    if (report.unreadable > 0)
        parts << tr("%1 can't be read").arg(report.unreadable);
    if (report.needsReview > 0)
        parts << tr("%1 to check").arg(report.needsReview);
    statusBar()->showMessage(tr("Import finished: %1.").arg(parts.join(QStringLiteral(", "))), 15000);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Papertrail"),
                       tr("<h3>Papertrail %1</h3>"
                          "<p>A library manager and reader for academic publications.</p>"
                          "<p>Licensed under the GNU Affero General Public License, version 3 or later. "
                          "It comes with no warranty.</p>")
                           .arg(QCoreApplication::applicationVersion().toHtmlEscaped()));
}

} // namespace papertrail::ui
