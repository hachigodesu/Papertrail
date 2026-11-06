#include "ui/MainWindow.h"

#include "core/repo/FileRepository.h"
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
#include "ui/MetadataEditor.h"
#include "ui/PublicationListModel.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
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

constexpr int kMaxRechecks = 20; // A file locked for this long isn't simply still being copied.

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
        refreshNow();
    });
    connect(m_bulk, &BulkChanges::failed, this, [this](const QString &what, const QString &error) {
        statusBar()->showMessage(tr("%1 didn't finish: %2").arg(what, error), 15000);
        refreshNow();
    });

    m_sidebarThread->start();
    m_import->start();
    m_bulk->start();

    applyFilters();

    // Started once the window is up, so a large library never delays its appearance.
    QTimer::singleShot(0, this, [this] {
        const QList<LibraryRoot> roots = m_session.files().roots();
        if (m_session.settings().value(QStringLiteral("library.rescan_on_startup")).toBool() && !roots.isEmpty())
            rescanAll();
        else
            m_import->resumeQueue();
    });
}

MainWindow::~MainWindow()
{
    m_details->saveNotes();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_details->saveNotes();
    saveWindowState();
    event->accept();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_watcher)
        m_watcher->windowActivated();
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

    m_sidebar = new FilterSidebar(m_session.tags(), m_splitter);
    connect(m_sidebar, &FilterSidebar::filterChanged, this, &MainWindow::applyFilters);
    connect(m_sidebar, &FilterSidebar::tagsChanged, this, &MainWindow::refreshNow);
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
                openPublication(*id);
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
    connect(m_details, &DetailsPanel::showInFolderRequested, this, &MainWindow::showPublicationInFolder);
    connect(m_details, &DetailsPanel::bulkTagRequested, this, &MainWindow::tagSelected);
    connect(m_details, &DetailsPanel::bulkReadStatusRequested, this, &MainWindow::setSelectedReadStatus);
    connect(m_details, &DetailsPanel::bulkMarkCheckedRequested, this, &MainWindow::markSelectedChecked);
    m_splitter->addWidget(m_details);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({240, 760, 280});
    m_splitter->setCollapsible(1, false);

    setCentralWidget(central);
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
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    QAction *find = view->addAction(tr("&Search"), QKeySequence::Find, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
    Q_UNUSED(find)
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
    if (m_session.files().roots().isEmpty()) {
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

void MainWindow::openPublication(qint64 id)
{
    guarded(tr("Opening the file"), [&] {
        // Until Papertrail has its own reader, files open in whatever the system uses for them.
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
            menu.addAction(tr("Open"), this, [this, publication] { openPublication(publication); });
            menu.addAction(tr("Show in folder"), this, [this, publication] { showPublicationInFolder(publication); });
            menu.addAction(tr("Edit..."), this, [this, publication] { editPublication(publication); });
            menu.addSeparator();
        }
    }
    menu.addAction(tr("Add tag..."), this, &MainWindow::askForBulkTag);
    QMenu *status = menu.addMenu(tr("Set status"));
    for (const ReadStatus value : {ReadStatus::Unread, ReadStatus::Reading, ReadStatus::Read})
        status->addAction(readStatusLabel(value), this, [this, value] { setSelectedReadStatus(value); });
    menu.addAction(tr("Mark as checked"), this, &MainWindow::markSelectedChecked);
    menu.exec(view->viewport()->mapToGlobal(position));
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
    if (!folder.isEmpty())
        addFolder(folder);
}

void MainWindow::addFolder(const QString &path)
{
    guarded(tr("Adding the folder"), [&] {
        const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
        m_session.settings().setValue(QStringLiteral("library.last_import_folder"), folder);
        m_importStoppedByHand = false;

        for (const LibraryRoot &root : m_session.files().roots()) {
            if (import::isPathInside(folder, root.path)) {
                statusBar()->showMessage(tr("That folder is already in the library, so it's being scanned again."), 8000);
                m_import->scanRoots({root.id});
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
    dialog.exec();
    m_foldersDialog = nullptr;
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
        QMessageBox box(QMessageBox::Question, tr("Remove folder"),
                        tr("Stop indexing %1?\n\nNothing is deleted from your disk. Its publications stay in the library "
                           "with their tags and notes, marked as having no file linked.")
                            .arg(QDir::toNativeSeparators(root->path)),
                        QMessageBox::Yes | QMessageBox::Cancel, m_foldersDialog ? static_cast<QWidget *>(m_foldersDialog) : this);
        box.setTextFormat(Qt::PlainText);
        if (box.exec() != QMessageBox::Yes)
            return;
        m_watcher->removeRoot(rootId);
        m_session.files().removeRoot(rootId);
        if (m_foldersDialog)
            m_foldersDialog->reload();
        refreshNow();
    });
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
    if (report.stoppedEarly) {
        statusBar()->showMessage(tr("Import stopped. It carries on from the same place next time."), 10000);
        return;
    }
    if (report.publicationsCreated == 0 && report.duplicates == 0 && report.moved == 0 && report.updated == 0
        && report.unreadable == 0 && report.reclassified == 0) {
        return;
    }
    QStringList parts;
    parts << tr("%1 added").arg(report.publicationsCreated);
    if (report.duplicates > 0)
        parts << tr("%1 duplicate files linked").arg(report.duplicates);
    if (report.moved > 0)
        parts << tr("%1 moved").arg(report.moved);
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
