#include "ui/MainWindow.h"

#include "import/JournalIssues.h"
#include "core/db/Backups.h"
#include "core/repo/FileRepository.h"
#include "core/repo/RemovedContent.h"
#include "core/repo/LookupRepository.h"
#include "core/repo/NoticeRepository.h"
#include "core/repo/PublicationMerge.h"
#include "metadata/Matching.h"
#include "ui/BulkChanges.h"
#include "ui/CoverCache.h"
#include "ui/Delegates.h"
#include "ui/DetailsPanel.h"
#include "ui/SummariesDialog.h"
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
#include "metadata/Transport.h"
#include "ui/ApiKeyStore.h"
#include "ui/IndexThread.h"
#include "ui/ArticleCoverThread.h"
#include "ui/BackupsDialog.h"
#include "ui/BatchSummaryDialog.h"
#include "summary/SummaryBatches.h"
#include "ui/MarkedCopy.h"
#include "ui/NotesDialog.h"
#include "ui/CitationActions.h"
#include "ui/TransferDialogs.h"
#include "ui/GridView.h"
#include "ui/SummaryDialog.h"
#include "ui/ReviewTab.h"
#include "ui/VolumeDialogs.h"
#include "ui/MetadataEditor.h"
#include "ui/PublicationListModel.h"
#include "ui/ReaderTab.h"
#include "core/repo/SavedSearchRepository.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"

#include <QStyleHints>
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
#include <QJsonArray>
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
    LibraryTab::All,       LibraryTab::JournalIssues,         LibraryTab::JournalArticles,
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
    m_articleCovers = new ArticleCoverThread(session.paths(), this);
    m_citations = new CitationActions(session, this);
    m_markedCopy = new MarkedCopyActions(session, this);
    connect(m_markedCopy, &MarkedCopyActions::finished, this, [this](bool ok, const QString &message) {
        if (ok) {
            statusBar()->showMessage(message, 15000);
            return;
        }
        QMessageBox box(QMessageBox::Warning, tr("Save a copy with highlights"), message, QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
    });
    connect(m_citations, &CitationActions::message, this, [this](const QString &text) { statusBar()->showMessage(text, 8000); });
    m_transfer = new LibraryTransferActions(session, this);
    if (session.paths().dailyBackupDelayMs >= 0)
        QTimer::singleShot(session.paths().dailyBackupDelayMs, this, &MainWindow::startDailyBackup);
    connect(m_transfer, &LibraryTransferActions::message, this, [this](const QString &text) { statusBar()->showMessage(text, 8000); });
    connect(m_transfer, &LibraryTransferActions::foldersRequested, this, &MainWindow::showFoldersDialog);
    connect(m_transfer, &LibraryTransferActions::importFinished, this,
            [this](bool ok, const transfer::ImportReport &report, const QString &text) {
                // Whatever happened, the library is looked at afresh, and anything paused carries on.
                m_covers->forgetMissing();
                refreshNow();
                refreshSavedSearches();
                m_details->reload();
                rescanAll(); // Finds the files that imported publications are waiting for.
                if (m_session.settings().value(QStringLiteral("library.index_contents")).toBool())
                    m_index->kick();
                updateSuggestedFilesAction();
                QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning, tr("Import a library"),
                                ok ? describeImport(report) : tr("The library couldn't be imported: %1").arg(text),
                                QMessageBox::Ok, this);
                box.setTextFormat(Qt::PlainText);
                QPushButton *check = nullptr;
                if (ok && report.proposed > 0)
                    check = box.addButton(tr("Check suggested files..."), QMessageBox::ActionRole);
                box.exec();
                if (check && box.clickedButton() == check)
                    showSuggestedFiles();
            });
    m_keys = session.paths().keyStoreFactory ? session.paths().keyStoreFactory() : createSystemKeyStore();
    m_summaryTransport = session.paths().transportFactory ? session.paths().transportFactory() : std::make_unique<metadata::NetworkTransport>();
    m_batches = new summary::SummaryBatches(session.database(), *m_summaryTransport, session.paths().providerUrls.anthropic, this);
    connect(m_batches, &summary::SummaryBatches::submitted, this, [this](int count) {
        statusBar()->showMessage(count == 1 ? tr("Sent 1 publication to be summarized at half price. Its summary waits for you to "
                                                 "review when it arrives, usually within the hour.")
                                            : tr("Sent %1 publications to be summarized at half price. Each summary waits for you to "
                                                 "review when it arrives, usually within the hour.")
                                                  .arg(count),
                                 15000);
        m_batchTimer->start();
    });
    connect(m_batches, &summary::SummaryBatches::submitFailed, this, [this](summary::SummaryFailure, const QString &message) {
        QMessageBox box(QMessageBox::Warning, tr("Summarize later at half price"), tr("Nothing was sent. %1").arg(message), QMessageBox::Ok,
                        this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
    });
    connect(m_batches, &summary::SummaryBatches::summariesArrived, this, [this](const QList<qint64> &ids) {
        statusBar()->showMessage(ids.size() == 1 ? tr("A summary from Claude has arrived. Review it in View > Summaries from Claude.")
                                                 : tr("%1 summaries from Claude have arrived. Review them in View > Summaries from Claude.")
                                                       .arg(ids.size()),
                                 20000);
    });
    // Whatever's on its way or waiting to be reviewed is shown on its publication and in the status bar.
    m_summaryStatus = new QPushButton(this);
    m_summaryStatus->setFlat(true);
    m_summaryStatus->setToolTip(tr("Summaries from Claude, on their way or waiting for you to review them."));
    connect(m_summaryStatus, &QPushButton::clicked, this, [this] { showSummariesDialog(); });
    statusBar()->addPermanentWidget(m_summaryStatus);
    // New retractions and concerns stay counted here until they've been looked at, even across restarts.
    m_retractionStatus = new QPushButton(this);
    m_retractionStatus->setObjectName(QStringLiteral("retractionStatus"));
    m_retractionStatus->setFlat(true);
    m_retractionStatus->setToolTip(tr("Publications in the library newly retracted, withdrawn or of concern. "
                                      "Click to show them."));
    m_retractionStatus->hide();
    connect(m_retractionStatus, &QPushButton::clicked, this, &MainWindow::showNewRetractions);
    statusBar()->addPermanentWidget(m_retractionStatus);
    connect(m_batches, &summary::SummaryBatches::changed, this, [this] {
        refreshSummaryStatus();
        if (m_details->currentId())
            m_details->reload();
    });
    refreshSummaryStatus();
    connect(m_batches, &summary::SummaryBatches::summariesFailed, this, [this](int count, const QString &problem) {
        statusBar()->showMessage(count == 1 ? tr("A summary couldn't be made: %1").arg(problem)
                                            : tr("%1 summaries couldn't be made. The first: %2").arg(count).arg(problem),
                                 20000);
    });
    connect(m_batches, &summary::SummaryBatches::checkProblem, this,
            [this](const QString &message) { statusBar()->showMessage(tr("Summaries on their way couldn't be checked: %1").arg(message), 15000); });
    // While any are on their way, now and then, as Anthropic suggests asking.
    m_batchTimer = new QTimer(this);
    m_batchTimer->setInterval(3 * 60000);
    connect(m_batchTimer, &QTimer::timeout, this, &MainWindow::checkSummaryBatches);
    QTimer::singleShot(15000, this, &MainWindow::checkSummaryBatches);

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
    connect(m_covers, &CoverCache::articleCoverWanted, m_articleCovers, &ArticleCoverThread::request);
    connect(m_articleCovers, &ArticleCoverThread::made, m_covers, &CoverCache::coverMade);

    buildToolbar();
    buildCentre();
    buildStatusBar();
    buildMenus();
    refreshSavedSearches();
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
    m_articleCovers->start();
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
        updateRetractionWarning();
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
    if (m_dailyBackup)
        m_dailyBackup->wait();
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
        offerWhatsDue();
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
    // The results are in the library, so typing a search while reading goes there to show them.
    connect(m_search, &QLineEdit::textEdited, this, [this] {
        if (m_documentTabs && m_documentTabs->currentIndex() != 0)
            m_documentTabs->setCurrentIndex(0);
    });
    row->addWidget(m_search, 1);

    // Every way of searching, each with an example that's added to the search when chosen.
    m_searchHelp = new QToolButton(container);
    m_searchHelp->setText(QStringLiteral("?"));
    m_searchHelp->setToolTip(tr("Ways to search"));
    m_searchHelp->setAccessibleName(tr("Ways to search"));
    m_searchHelp->setPopupMode(QToolButton::InstantPopup);
    auto *help = new QMenu(m_searchHelp);
    const auto example = [this, help](const QString &text, const QString &meaning) {
        // A tab puts the explanation in a column of its own.
        QAction *action = help->addAction(text + u'\t' + meaning);
        connect(action, &QAction::triggered, this, [this, text] {
            QString current = m_search->text().trimmed();
            m_search->setText(current.isEmpty() ? text : current + u' ' + text);
            m_search->setFocus();
            // Placed ready to type over the example's value, which follows the last colon or quote.
            const qsizetype colon = text.lastIndexOf(u':');
            if (colon >= 0 && colon + 1 < text.size())
                m_search->setSelection(int(m_search->text().size() - (text.size() - colon - 1)), int(text.size() - colon - 1));
        });
        return action;
    };
    const auto heading = [help](const QString &text) {
        QAction *action = help->addSection(text);
        action->setEnabled(false);
    };
    heading(tr("Words"));
    example(QStringLiteral("fatigue beams"), tr("Every word, anywhere in the details"));
    example(QStringLiteral("\"steel beams\""), tr("These words together, in this order"));
    heading(tr("In one detail"));
    example(QStringLiteral("title:fatigue"), tr("In the title"));
    example(QStringLiteral("author:knuth"), tr("By an author or editor"));
    example(QStringLiteral("author:\"van der berg\""), tr("A name of several words, in quotes"));
    example(QStringLiteral("abstract:corrosion"), tr("In the abstract"));
    example(QStringLiteral("journal:nature"), tr("In a journal, or other source"));
    example(QStringLiteral("conference:siggraph"), tr("At a conference"));
    example(QStringLiteral("publisher:wiley"), tr("From a publisher"));
    example(QStringLiteral("institution:leeds"), tr("From a university or other institution"));
    example(QStringLiteral("tag:maths"), tr("With a tag, or one inside it"));
    example(QStringLiteral("note:welds"), tr("In your highlights and notes"));
    example(QStringLiteral("in:\"nature 2024\""), tr("Articles in an issue, proceedings or book with this title"));
    heading(tr("Kind and date"));
    example(QStringLiteral("type:thesis"), tr("Of a type: thesis, issue, book, standard..."));
    example(QStringLiteral("year:2020"), tr("From a year"));
    example(QStringLiteral("year:>2010"), tr("After a year. Also <, >= and <="));
    example(QStringLiteral("year:2015..2020"), tr("From a range of years"));
    heading(tr("Identifiers"));
    example(QStringLiteral("doi:10.1000"), tr("A DOI, or the start of one"));
    example(QStringLiteral("isbn:9780306406157"), tr("An ISBN, with or without hyphens"));
    example(QStringLiteral("issn:1234-5679"), tr("An ISSN"));
    example(QStringLiteral("arxiv:2101.00001"), tr("An arXiv number"));
    heading(tr("Together"));
    example(QStringLiteral("author:knuth year:>2010"), tr("Several at once: all must match"));
    help->addSeparator();
    help->addAction(tr("Save this search..."), this, &MainWindow::askToSaveSearch);
    QAction *inside = help->addAction(tr("Tick Inside documents to search the text of documents too"));
    connect(inside, &QAction::triggered, this, [this] { m_searchInside->setChecked(true); });
    m_searchHelp->setMenu(help);
    row->addWidget(m_searchHelp);

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
    m_libraryPage = central;
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
    connect(m_sidebar, &FilterSidebar::publicationsDropped, this, [this](FilterSidebar::Section section, qint64 id) {
        if (selectedCount() == 0)
            return;
        if (section == FilterSidebar::Collections) {
            addSelectedToCollection(id);
        } else if (section == FilterSidebar::Tags) {
            QString path;
            guarded(tr("Reading the tags"), [&] { path = m_session.tags().path(id); });
            if (!path.isEmpty())
                tagSelected(path);
        }
    });
    connect(m_sidebar, &FilterSidebar::savedSearchChosen, this, &MainWindow::showSavedSearch);
    connect(m_sidebar, &FilterSidebar::saveSearchRequested, this, &MainWindow::askToSaveSearch);
    connect(m_sidebar, &FilterSidebar::savedSearchReplaceRequested, this, [this](qint64 id) {
        const QJsonObject definition = currentSearchDefinition();
        if (guarded(tr("Saving the search"), [&] { SavedSearchRepository(m_session.database()).setDefinition(id, definition); }))
            statusBar()->showMessage(tr("The saved search now shows what's shown here."), 8000);
    });
    connect(m_sidebar, &FilterSidebar::savedSearchRenameRequested, this, [this](qint64 id) {
        std::optional<SavedSearch> search;
        guarded(tr("Reading the saved search"), [&] { search = SavedSearchRepository(m_session.database()).get(id); });
        if (!search)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename saved search"), tr("Name:"), QLineEdit::Normal, search->name, &ok);
        if (ok && guarded(tr("Renaming the saved search"), [&] { SavedSearchRepository(m_session.database()).rename(id, name); }))
            refreshSavedSearches();
    });
    connect(m_sidebar, &FilterSidebar::savedSearchDeleteRequested, this, [this](qint64 id) {
        std::optional<SavedSearch> search;
        guarded(tr("Reading the saved search"), [&] { search = SavedSearchRepository(m_session.database()).get(id); });
        if (!search)
            return;
        // Only the search goes: nothing it found is touched.
        if (QMessageBox::question(this, tr("Delete saved search"),
                                  tr("Delete the saved search \"%1\"? What it finds isn't affected.").arg(search->name))
            != QMessageBox::Yes)
            return;
        if (guarded(tr("Deleting the saved search"), [&] { SavedSearchRepository(m_session.database()).remove(id); }))
            refreshSavedSearches();
    });
    m_splitter->addWidget(m_sidebar);

    m_stack = new QStackedWidget(m_splitter);

    m_grid = new GridView(m_stack);
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
    // Without the band, a list-mode view only follows a drag while the pointer is over a card, so ending in the
    // empty space beside a short last row left that row out. Set after the view mode, which it belongs to.
    m_grid->setSelectionRectVisible(true);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->verticalScrollBar()->setSingleStep(40);
    m_grid->setItemDelegate(new GridDelegate(*m_covers, m_grid));
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setAccessibleName(tr("Publications"));
    m_stack->addWidget(m_grid);

    m_list = new PublicationTableView(m_stack);
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
    // Delete removes what's chosen, after asking, in either view.
    for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(m_grid), static_cast<QAbstractItemView *>(m_list)}) {
        auto *remove = new QAction(tr("Remove from library..."), view);
        remove->setShortcut(QKeySequence::Delete);
        remove->setShortcutContext(Qt::WidgetShortcut);
        connect(remove, &QAction::triggered, this, &MainWindow::askToRemoveSelected);
        view->addAction(remove);
    }
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
    // Only on a library with nothing in it yet: starting from an export, and where the library is kept.
    m_welcomeImport = new QPushButton(tr("Import a library from another computer..."), m_messagePage);
    m_welcomeImport->setToolTip(tr("Brings in a .papertrail file exported from Papertrail elsewhere, with its tags, notes and settings."));
    connect(m_welcomeImport, &QPushButton::clicked, this, [this] { m_transfer->askAndImport([this] { prepareForImport(); }); });
    messageColumn->addWidget(m_welcomeImport, 0, Qt::AlignCenter);
    m_welcomePlace = new QLabel(m_messagePage);
    m_welcomePlace->setTextFormat(Qt::PlainText);
    m_welcomePlace->setAlignment(Qt::AlignCenter);
    m_welcomePlace->setWordWrap(true);
    m_welcomePlace->setTextInteractionFlags(Qt::TextSelectableByMouse);
    {
        const QString folder = QDir::toNativeSeparators(QFileInfo(m_session.paths().database).absolutePath());
        const bool portable = QFileInfo(m_session.paths().database).absolutePath()
            == QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(LibraryPaths::kPortableFolder));
        m_welcomePlace->setText(portable ? tr("This is a portable copy, so its library is kept beside it, in %1.").arg(folder)
                                         : tr("Your library is kept in %1.").arg(folder));
    }
    messageColumn->addSpacing(12);
    messageColumn->addWidget(m_welcomePlace);
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
    connect(m_details, &DetailsPanel::openRequested, this, [this](qint64 id) { openPublication(id); });
    connect(m_details, &DetailsPanel::openInVolumeRequested, this, [this](qint64 id) { openPublication(id, false); });
    connect(m_details, &DetailsPanel::openFileRequested, this, [this](qint64 id, qint64 fileId) { openPublicationFile(id, fileId); });
    connect(m_details, &DetailsPanel::showInFolderRequested, this, &MainWindow::showPublicationInFolder);
    connect(m_details, &DetailsPanel::openMatchRequested, this, &MainWindow::openAtMatch);
    connect(m_details, &DetailsPanel::summarizeRequested, this, &MainWindow::showSummaryDialog);
    connect(m_details, &DetailsPanel::summariesRequested, this, [this](qint64 id) { showSummariesDialog(id); });
    connect(m_details, &DetailsPanel::findFileRequested, this, [this](qint64 id) {
        QString start;
        const QList<LibraryRoot> roots = m_session.files().roots();
        if (!roots.isEmpty())
            start = roots.first().path;
        const QString path = QFileDialog::getOpenFileName(this, tr("Find its file"), start);
        if (!path.isEmpty())
            linkFileByHand(id, path);
    });
    connect(m_details, &DetailsPanel::citeMenuRequested, this, [this](QMenu *menu, qint64 id) {
        m_citations->fillCiteMenu(menu, 1, [id] { return QList<qint64>{id}; });
    });
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

    // The library is always the first tab. The tab bar only appears once a document is open. Documents can be
    // dragged into any order after it.
    m_documentTabs = new QTabWidget(this);
    m_documentTabs->setDocumentMode(true);
    m_documentTabs->setTabBarAutoHide(true);
    m_documentTabs->setTabsClosable(true);
    m_documentTabs->setMovable(true);
    connect(m_documentTabs->tabBar(), &QTabBar::tabMoved, this, [this] {
        // Put back once the drag is over, since moving a tab in the middle of one confuses the tab bar.
        QTimer::singleShot(0, this, [this] {
            if (QApplication::mouseButtons() != Qt::NoButton) {
                m_documentTabs->tabBar()->update();
                return;
            }
            keepLibraryFirst();
        });
    });
    m_documentTabBar = m_documentTabs->tabBar();
    m_documentTabBar->installEventFilter(this);
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
            offerWhatsDue();
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
    // These switches are in the Settings window, which passes changes to them.
    m_indexContents = new QAction(tr("Index document text"), this);
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
    m_rescanOnStartup = new QAction(tr("Scan folders when Papertrail starts"), this);
    m_rescanOnStartup->setCheckable(true);
    m_rescanOnStartup->setChecked(m_session.settings().value(QStringLiteral("library.rescan_on_startup")).toBool());
    connect(m_rescanOnStartup, &QAction::toggled, this, [this](bool on) {
        m_session.settings().setValue(QStringLiteral("library.rescan_on_startup"), on);
    });
    m_watchFolders = new QAction(tr("Watch folders for changes"), this);
    m_watchFolders->setCheckable(true);
    m_watchFolders->setChecked(m_watcher->isEnabled());
    connect(m_watchFolders, &QAction::toggled, this, [this](bool on) {
        m_session.settings().setValue(QStringLiteral("library.watch_folders"), on);
        m_watcher->setEnabled(on);
    });
    file->addAction(tr("Se&ttings..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this, &MainWindow::showSettings);
    file->addSeparator();
    file->addAction(tr("&Export citations..."), this, &MainWindow::exportCitations);
    file->addSeparator();
    file->addAction(tr("Export library..."), this, [this] { m_transfer->askAndExport(); });
    file->addAction(tr("Import library..."), this, [this] { m_transfer->askAndImport([this] { prepareForImport(); }); });
    m_checkSuggestedFiles = file->addAction(tr("Check suggested files..."), this, &MainWindow::showSuggestedFiles);
    updateSuggestedFilesAction();
    file->addSeparator();
    file->addAction(tr("&Backups..."), this, &MainWindow::showBackupsDialog);
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
    view->addAction(tr("&Highlights and notes..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this, &MainWindow::showNotesDialog);
    view->addAction(tr("Summaries from &Claude..."), this, [this] { showSummariesDialog(); });
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

    // Following the system means following it while open, too, where the style has to change with it.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        static bool applying = false;
        if (applying)
            return;
        QString theme;
        guarded(tr("Reading settings"), [&] { theme = m_session.settings().value(QStringLiteral("appearance.theme")).toString(); });
        if (themeFromSetting(theme) != Theme::System)
            return;
        applying = true;
        applyTheme(Theme::System);
        applying = false;
    });
}

void MainWindow::showSettings()
{
    SettingsActions actions;
    actions.rescanOnStartup = m_rescanOnStartup;
    actions.watchFolders = m_watchFolders;
    actions.indexContents = m_indexContents;
    actions.showFolders = [this] { showFoldersDialog(); };
    actions.showOnlineLookups = [this] { showLookupSettings(); };
    actions.showApiKey = [this] {
        ApiKeyDialog dialog(*m_keys, this);
        dialog.exec();
    };
    actions.showBackups = [this] { showBackupsDialog(); };
    SettingsDialog dialog(m_session, actions, this);
    dialog.exec();
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
            offerWhatsDue();
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
    connect(m_lookup, &LookupThread::noticesChanged, this, [this](const QList<qint64> &ids) {
        refreshNow(); // The sidebar's count of retractions.
        if (const std::optional<qint64> shown = m_details->currentId(); shown && ids.contains(*shown))
            m_details->reload();
    });
    connect(m_lookup, &LookupThread::noticesChecked, this, [this](int, int) { updateRetractionWarning(); });
    // Notices come out long after a paper does, so the library's checked now and then: a little after opening,
    // then every few hours, each publication at most once a month.
    if (m_session.paths().noticeCheckIntervalMs < 0) {
        QTimer::singleShot(60000, this, [this] { m_lookup->checkNotices(); });
        auto *noticeTimer = new QTimer(this);
        noticeTimer->setInterval(6 * 3600 * 1000);
        connect(noticeTimer, &QTimer::timeout, this, [this] { m_lookup->checkNotices(); });
        noticeTimer->start();
    }
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

namespace {

// How a saved search names a tab and each part of the sidebar, which mustn't change between versions.
const QList<QPair<LibraryTab, QString>> kTabKeys = {
    {LibraryTab::All, QStringLiteral("all")},
    {LibraryTab::JournalIssues, QStringLiteral("journal_issues")},
    {LibraryTab::JournalArticles, QStringLiteral("journal_articles")},
    {LibraryTab::Preprints, QStringLiteral("preprints")},
    {LibraryTab::ConferencePapers, QStringLiteral("conference_papers")},
    {LibraryTab::Textbooks, QStringLiteral("textbooks")},
    {LibraryTab::ThesesAndDissertations, QStringLiteral("theses_and_dissertations")},
    {LibraryTab::TechnicalReports, QStringLiteral("technical_reports")},
    {LibraryTab::Standards, QStringLiteral("standards")},
    {LibraryTab::Other, QStringLiteral("other")},
};

const QList<QPair<FilterSidebar::Section, QString>> kSectionKeys = {
    {FilterSidebar::Tags, QStringLiteral("tags")},
    {FilterSidebar::Status, QStringLiteral("status")},
    {FilterSidebar::Collections, QStringLiteral("collections")},
    {FilterSidebar::Authors, QStringLiteral("authors")},
    {FilterSidebar::Journals, QStringLiteral("journals")},
    {FilterSidebar::Publishers, QStringLiteral("publishers")},
    {FilterSidebar::Conferences, QStringLiteral("conferences")},
    {FilterSidebar::Languages, QStringLiteral("languages")},
    {FilterSidebar::Formats, QStringLiteral("formats")},
    {FilterSidebar::ReadStatuses, QStringLiteral("read_statuses")},
};

// Tags, collections and people are chosen by id here, but kept by what names them in any library.
bool keptByName(FilterSidebar::Section section)
{
    return section == FilterSidebar::Tags || section == FilterSidebar::Collections || section == FilterSidebar::Authors;
}

} // namespace

QJsonObject MainWindow::currentSearchDefinition()
{
    QJsonObject definition;
    definition.insert(QStringLiteral("search"), m_search->text().trimmed());
    definition.insert(QStringLiteral("inside"), m_searchInside->isChecked());
    const LibraryTab tab = kTabs.value(m_tabs->currentIndex(), LibraryTab::All);
    for (const auto &[value, key] : kTabKeys) {
        if (value == tab)
            definition.insert(QStringLiteral("tab"), key);
    }

    const SidebarChoices choices = m_sidebar->choices();
    QJsonObject filters;
    guarded(tr("Reading the filters"), [&] {
        SavedSearchRepository searches(m_session.database());
        for (const auto &[section, name] : kSectionKeys) {
            QJsonArray values;
            for (const auto &[key, label] : choices.values.value(section)) {
                QString kept = key;
                if (section == FilterSidebar::Tags)
                    kept = searches.tagKey(key.toLongLong());
                else if (section == FilterSidebar::Collections)
                    kept = searches.collectionKey(key.toLongLong());
                else if (section == FilterSidebar::Authors)
                    kept = searches.personKey(key.toLongLong());
                if (!kept.isEmpty())
                    values.append(QJsonObject{{QStringLiteral("key"), kept}, {QStringLiteral("label"), label}});
            }
            if (!values.isEmpty())
                filters.insert(name, values);
        }
    });
    definition.insert(QStringLiteral("filters"), filters);
    if (!choices.yearFrom.isEmpty())
        definition.insert(QStringLiteral("year_from"), choices.yearFrom);
    if (!choices.yearTo.isEmpty())
        definition.insert(QStringLiteral("year_to"), choices.yearTo);
    return definition;
}

std::optional<qint64> MainWindow::saveCurrentSearch(const QString &name)
{
    const QJsonObject definition = currentSearchDefinition();
    std::optional<qint64> id;
    if (!guarded(tr("Saving the search"), [&] { id = SavedSearchRepository(m_session.database()).create(name, definition); }))
        return std::nullopt;
    refreshSavedSearches();
    return id;
}

void MainWindow::askToSaveSearch()
{
    const QString search = m_search->text().trimmed();
    if (search.isEmpty() && !m_sidebar->hasSelection() && m_tabs->currentIndex() == 0) {
        QMessageBox::information(this, tr("Save this search"),
                                 tr("Search, or choose a tab or filters, first. A saved search shows them all again."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save this search"), tr("A name for the search, tab and filters shown now:"),
                                               QLineEdit::Normal, search.left(60), &ok);
    if (ok && saveCurrentSearch(name))
        statusBar()->showMessage(tr("Saved. It's in the sidebar, under Saved searches."), 8000);
}

void MainWindow::showSavedSearch(qint64 id)
{
    std::optional<SavedSearch> search;
    SidebarChoices choices;
    guarded(tr("Reading the saved search"), [&] {
        SavedSearchRepository searches(m_session.database());
        search = searches.get(id);
        if (!search)
            return;
        // Anything deleted since, such as a tag, is left out.
        const QJsonObject filters = search->definition.value(QStringLiteral("filters")).toObject();
        for (const auto &[section, name] : kSectionKeys) {
            for (const QJsonValue &value : filters.value(name).toArray()) {
                const QString kept = value.toObject().value(QStringLiteral("key")).toString();
                const QString label = value.toObject().value(QStringLiteral("label")).toString();
                QString key = kept;
                if (keptByName(section)) {
                    std::optional<qint64> local;
                    if (section == FilterSidebar::Tags)
                        local = searches.tagFromKey(kept);
                    else if (section == FilterSidebar::Collections)
                        local = searches.collectionFromKey(kept);
                    else
                        local = searches.personFromKey(kept);
                    if (!local)
                        continue;
                    key = QString::number(*local);
                }
                if (!key.isEmpty())
                    choices.values[section] << qMakePair(key, label.isEmpty() ? key : label);
            }
        }
    });
    if (!search)
        return;
    const QJsonObject &definition = search->definition;
    choices.yearFrom = definition.value(QStringLiteral("year_from")).toString();
    choices.yearTo = definition.value(QStringLiteral("year_to")).toString();

    if (m_documentTabs)
        m_documentTabs->setCurrentIndex(0);
    const QString tab = definition.value(QStringLiteral("tab")).toString();
    LibraryTab chosenTab = LibraryTab::All;
    for (const auto &[value, key] : kTabKeys) {
        if (key == tab)
            chosenTab = value;
    }
    setTab(chosenTab);
    m_searchInside->setChecked(definition.value(QStringLiteral("inside")).toBool());
    m_sidebar->setChoices(choices);
    m_search->setText(definition.value(QStringLiteral("search")).toString());
}

void MainWindow::refreshSavedSearches()
{
    QList<QPair<qint64, QString>> listed;
    guarded(tr("Reading the saved searches"), [&] {
        for (const SavedSearch &search : SavedSearchRepository(m_session.database()).all())
            listed << qMakePair(search.id, search.name);
    });
    m_sidebar->setSavedSearches(listed);
}

void MainWindow::showArticlesOf(qint64 holder)
{
    guarded(tr("Reading the publication"), [&] {
        const std::optional<Publication> publication = m_session.publications().get(holder);
        if (!publication)
            return;
        // A quote would end the search's own quotes early, so it's left out: the words still match.
        QString title = publication->title.simplified();
        title.remove(u'"');
        // Everything else is cleared, since a tab or filter for issues would leave out their articles.
        m_sidebar->clearSelection();
        setTab(LibraryTab::All);
        if (m_documentTabs)
            m_documentTabs->setCurrentIndex(0);
        m_search->setText(QStringLiteral("in:\"%1\"").arg(title));
    });
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
    bool welcome = false;
    if (m_sidebar->onlyCollectionsChosen() && m_search->text().isEmpty() && m_tabs->currentIndex() == 0) {
        m_message->setText(tr("Nothing has been added to this yet.\n\nSelect some publications in the library, "
                              "right-click them and choose Add to collection."));
        m_messageButton->setText(tr("Clear search and filters"));
        m_messageButton->show();
    } else if (m_session.files().roots().isEmpty()) {
        m_message->setText(tr("Welcome to Papertrail.\n\nAdd a folder of papers and books to start your library. "
                              "They stay where they are, and Papertrail never changes them.\n\n"
                              "Or bring in a library you've exported from Papertrail on another computer."));
        m_messageButton->setText(tr("Add folder..."));
        m_messageButton->show();
        welcome = true;
    } else if (m_import->isBusy() && !m_sidebar->hasSelection() && m_search->text().isEmpty()) {
        m_message->setText(tr("Importing your files. They'll appear here as they're found."));
        m_messageButton->hide();
    } else {
        m_message->setText(tr("No publications match."));
        m_messageButton->setText(tr("Clear search and filters"));
        m_messageButton->show();
    }
    m_welcomeImport->setVisible(welcome);
    m_welcomePlace->setVisible(welcome);
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
        // Made an issue, its title may only describe it. A title typed here is never replaced.
        if (publication.type == PublicationType::JournalIssue)
            import::tidyJournalIssue(m_session.database(), publication.id);
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
    editor.setSuggestions([this](const QString &field, const QString &typed) { return metadataSuggestions(field, typed); });
    if (editor.exec() == QDialog::Accepted)
        saveEdit(editor.result());
}

QStringList MainWindow::metadataSuggestions(const QString &field, const QString &typed)
{
    static const QHash<QString, QByteArray> columns = {
        {QStringLiteral("container"), "container_title"},     {QStringLiteral("publisher"), "publisher"},
        {QStringLiteral("place"), "place"},                   {QStringLiteral("series"), "series"},
        {QStringLiteral("conference"), "conference_name"},    {QStringLiteral("conference_location"), "conference_location"},
        {QStringLiteral("institution"), "institution"},       {QStringLiteral("department"), "department"},
        {QStringLiteral("degree"), "degree"},                 {QStringLiteral("standard_body"), "standard_body"},
        {QStringLiteral("language"), "language"}};
    static const QHash<QString, QByteArray> roles = {{QStringLiteral("authors"), "author"},
                                                     {QStringLiteral("editors"), "editor"},
                                                     {QStringLiteral("translators"), "translator"},
                                                     {QStringLiteral("advisors"), "advisor"}};
    QString escaped = typed;
    escaped.replace(u'\\', QStringLiteral("\\\\")).replace(u'%', QStringLiteral("\\%")).replace(u'_', QStringLiteral("\\_"));
    QStringList found;
    try {
        if (const auto column = columns.constFind(field); column != columns.cend()) {
            auto select = m_session.database().prepare(
                "SELECT " + *column + ", count(*) AS used FROM publication WHERE " + *column + " LIKE ? ESCAPE '\\' AND "
                + *column + " <> '' GROUP BY " + *column + " ORDER BY used DESC, " + *column + " LIMIT 15");
            select->bind(1, u'%' + escaped + u'%');
            while (select->step())
                found << select->textAt(0);
        } else if (roles.contains(field) || field == QLatin1StringView("people")) {
            // Anyone named in the library, in any role, by family or given name, most often named first.
            auto select = m_session.database().prepare(
                "SELECT p.family_name, p.given_name, count(*) AS used FROM person p JOIN publication_person pp ON pp.person_id = p.id "
                "WHERE p.family_name LIKE ?1 ESCAPE '\\' OR p.given_name LIKE ?1 ESCAPE '\\' OR "
                "(p.family_name || ', ' || p.given_name) LIKE ?2 ESCAPE '\\' "
                "GROUP BY p.id ORDER BY used DESC, p.family_name LIMIT 15");
            select->bind(1, escaped + u'%');
            select->bind(2, escaped + u'%');
            while (select->step()) {
                const QString given = select->textAt(1);
                found << (given.isEmpty() ? select->textAt(0) : select->textAt(0) + QStringLiteral(", ") + given);
            }
        }
    } catch (const std::exception &) {
        // Only suggestions: typing carries on without them.
    }
    return found;
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

ReaderTab *MainWindow::openPublication(qint64 id, bool articleOnly)
{
    const QList<FileRecord> choices = formatChoices(id);
    if (choices.isEmpty()) {
        statusBar()->showMessage(tr("This publication's file can't be found right now."), 8000);
        return nullptr;
    }
    return openPublicationFile(id, choices.first().id, articleOnly);
}

ReaderTab *MainWindow::openPublicationFile(qint64 id, qint64 fileId, bool articleOnly)
{
    for (int i = 1; i < m_documentTabs->count(); ++i) {
        auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->widget(i));
        if (reader && reader->publicationId() == id && reader->fileId() == fileId) {
            if (reader->isArticle())
                reader->setArticleOnly(articleOnly);
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
        reader->setArticleOnly(articleOnly);
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

void MainWindow::keepLibraryFirst()
{
    const int index = m_documentTabs->indexOf(m_libraryPage);
    if (index > 0)
        m_documentTabs->tabBar()->moveTab(index, 0);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Compared, not asked for: this sees the tab bar's events while the window is being destroyed.
    if (watched == m_documentTabBar && event->type() == QEvent::MouseButtonRelease)
        QTimer::singleShot(0, this, &MainWindow::keepLibraryFirst);
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::prepareForImport()
{
    stopImport();
    m_index->stopIndexing();
    m_lookup->stopLookups();
    // Documents open on publications that an import may change or, replacing the library, remove.
    while (m_documentTabs->count() > 1)
        closeDocumentTab(m_documentTabs->count() - 1);
}

void MainWindow::startDailyBackup()
{
    if (m_dailyBackup && m_dailyBackup->isRunning())
        return;
    if (!m_session.settings().value(QStringLiteral("library.daily_backups")).toBool())
        return;
    if (!m_dailyBackup) {
        m_dailyBackup = new DailyBackupThread(m_session.paths().database, this);
        connect(m_dailyBackup, &DailyBackupThread::failed, this, [this](const QString &error) {
            statusBar()->showMessage(tr("Today's backup couldn't be made: %1").arg(error), 15000);
        });
        connect(m_dailyBackup, &DailyBackupThread::made, this, [this] {
            if (m_backupsDialog)
                m_backupsDialog->reload();
        });
    }
    m_dailyBackup->start(QThread::LowPriority);
}

void MainWindow::showNotesDialog()
{
    NotesDialog dialog(m_session, this);
    connect(&dialog, &NotesDialog::openRequested, this, [this, &dialog](qint64 id) {
        if (openAnnotation(id))
            dialog.accept();
    });
    dialog.exec();
}

bool MainWindow::openAnnotation(qint64 annotationId)
{
    std::optional<Annotation> annotation;
    std::optional<qint64> fileId;
    guarded(tr("Reading the highlight"), [&] {
        annotation = AnnotationRepository(m_session.database()).get(annotationId);
        if (!annotation)
            return;
        // The file with the contents it was made on, wherever that is now.
        for (const FileRecord &file : m_session.files().withHash(annotation->contentHash)) {
            if (file.status == FileStatus::Ok && QFileInfo::exists(m_session.files().absolutePath(file.id))) {
                fileId = file.id;
                break;
            }
        }
    });
    if (!annotation)
        return false;
    if (!fileId) {
        statusBar()->showMessage(tr("The file this was made on can't be found right now."), 8000);
        return false;
    }
    ReaderTab *reader = openPublicationFile(annotation->publicationId, *fileId);
    if (!reader)
        return false;
    reader->showAnnotation(annotationId);
    return true;
}

void MainWindow::showBackupsDialog()
{
    BackupsDialog dialog(m_session, this);
    m_backupsDialog = &dialog;
    connect(&dialog, &BackupsDialog::restoreRequested, this, [this](const QString &path) {
        if (restoreBackup(path))
            m_backupsDialog->accept();
    });
    dialog.exec();
    m_backupsDialog = nullptr;
}

bool MainWindow::restoreBackup(const QString &backupPath)
{
    prepareForImport(); // Nothing else writes while the library's replaced.
    if (m_dailyBackup)
        m_dailyBackup->wait();
    m_details->saveNotes();
    const QString made = QFileInfo(backupPath).fileName();
    const bool done = guarded(tr("Restoring the backup"), [&] {
        core::restoreBackup(m_session.database(), m_session.paths().database, backupPath);
    });
    // Whatever happened, the library is looked at afresh.
    m_covers->forgetMissing();
    afterFoldersChanged();
    refreshSavedSearches();
    m_details->reload();
    updateSuggestedFilesAction();
    refreshSummaryStatus();
    rescanAll(); // Files the backup didn't know about are found again.
    if (m_session.settings().value(QStringLiteral("library.index_contents")).toBool())
        m_index->kick();
    if (done)
        statusBar()->showMessage(tr("The library was restored from %1. Its previous state was backed up first.").arg(made), 15000);
    return done;
}

void MainWindow::updateSuggestedFilesAction()
{
    if (!m_checkSuggestedFiles)
        return;
    qint64 count = 0;
    try {
        count = transfer::fileProposalCount(m_session.database());
    } catch (const std::exception &) {
    }
    m_checkSuggestedFiles->setEnabled(count > 0);
    m_checkSuggestedFiles->setText(count > 0 ? tr("Check suggested files (%1)...").arg(count) : tr("Check suggested files..."));
}

bool MainWindow::linkFileByHand(qint64 publicationId, const QString &path)
{
    const QString cleaned = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    std::optional<LibraryRoot> root;
    for (const LibraryRoot &candidate : m_session.files().roots()) {
        if (import::isPathInside(cleaned, QDir::fromNativeSeparators(candidate.path)))
            root = candidate;
    }
    if (!root) {
        QMessageBox box(QMessageBox::Information, tr("Find its file"),
                        tr("That file isn't in any of the library's folders. Add its folder to the library first, then try again."),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return false;
    }
    const bool done = guarded(tr("Linking the file"), [&] {
        Database &db = m_session.database();
        Transaction transaction(db);
        FileRecord file;
        file.rootId = root->id;
        file.relativePath = QDir(root->path).relativeFilePath(cleaned);
        file.relativePath = QDir::fromNativeSeparators(file.relativePath);
        const QFileInfo info(cleaned);
        file.size = info.size();
        file.modifiedAt = 0; // So the next scan reads it, for its contents, cover and facts.
        file.format = info.suffix().toLower();
        for (const FileRecord &known : m_session.files().filesInRoot(root->id)) {
            if (known.relativePath == file.relativePath)
                file = known;
        }
        file.modifiedAt = 0;
        const qint64 fileId = m_session.files().upsert(file);
        // Already another publication's, from scanning: the user says they're the same.
        for (const qint64 other : m_session.files().publicationsFor(fileId)) {
            if (other != publicationId)
                mergePublications(db, publicationId, other, QStringLiteral("manual"));
        }
        m_session.files().link(publicationId, fileId, true);
        auto waiting = db.prepare("DELETE FROM expected_file WHERE publication_id = ?");
        waiting->bind(1, publicationId);
        waiting->step();
        auto proposals = db.prepare("DELETE FROM file_proposal WHERE publication_id = ?");
        proposals->bind(1, publicationId);
        proposals->step();
        transaction.commit();
    });
    if (done) {
        refreshNow();
        m_details->reload();
        rescanAll();
        updateSuggestedFilesAction();
    }
    return done;
}

void MainWindow::showSuggestedFiles()
{
    FileProposalsDialog dialog(m_session, this);
    dialog.exec();
    if (dialog.changed() > 0) {
        refreshNow();
        m_details->reload();
        rescanAll(); // A file joined to an imported publication is read for its cover.
    }
    updateSuggestedFilesAction();
}

void MainWindow::exportCitations()
{
    QList<qint64> ids;
    guarded(tr("Reading the library"), [&] {
        ids = selectedCount() > 0 ? selectedIds() : m_session.query().ids(currentFilter(), currentSort());
    });
    if (ids.isEmpty()) {
        statusBar()->showMessage(tr("There's nothing to export."), 8000);
        return;
    }
    m_citations->askAndExport(ids);
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
            menu.addAction(tr("Summarize with Claude..."), this, [this, publication] { showSummaryDialog(publication); });
            menu.addAction(tr("Summarize later at half price..."), this, &MainWindow::showBatchSummaryDialog);
            bool marked = false;
            guarded(tr("Reading the highlights"), [&] { marked = markedPdfFor(m_session, publication).has_value(); });
            if (marked) {
                menu.addAction(tr("Save a copy with highlights..."), this,
                               [this, publication] { m_markedCopy->askAndSave(publication); });
            }
            guarded(tr("Reading the publication"), [&] {
                const std::optional<Publication> details = m_session.publications().get(publication);
                if (details && !details->parentId && canHoldArticles(details->type)) {
                    if (!m_session.publications().childIds(publication).isEmpty())
                        menu.addAction(tr("Show its articles"), this, [this, publication] { showArticlesOf(publication); });
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
    m_citations->addCiteMenu(&menu, count, [this] { return selectedIds(); });
    if (count > 1)
        menu.addAction(tr("Summarize %1 later at half price...").arg(count), this, &MainWindow::showBatchSummaryDialog);
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
    menu.addSeparator();
    menu.addAction(count == 1 ? tr("Remove from library...") : tr("Remove %1 from library...").arg(count), this,
                   &MainWindow::askToRemoveSelected);
    menu.exec(view->viewport()->mapToGlobal(position));
}

void MainWindow::showBatchSummaryDialog()
{
    QList<qint64> ids;
    guarded(tr("Reading the library"), [&] { ids = selectedIds(); });
    if (ids.isEmpty())
        return;
    // The key is read first, since without one nothing can be sent.
    m_keys->read([this, ids](const QString &key) {
        if (key.isEmpty()) {
            QMessageBox box(QMessageBox::Information, tr("Summarize later at half price"),
                            tr("This needs your Claude API key. Add it with Summarize with Claude on any publication, then try again."),
                            QMessageBox::Ok, this);
            box.setTextFormat(Qt::PlainText);
            box.exec();
            return;
        }
        BatchSummaryDialog dialog(m_session, ids, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const QList<Publication> chosen = dialog.included();
        if (chosen.isEmpty())
            return;
        m_batches->setApiKey(key);
        m_batches->submit(dialog.model(), dialog.effort(), chosen);
    });
}

void MainWindow::refreshSummaryStatus()
{
    qint64 waiting = 0;
    qint64 arrived = 0;
    try {
        waiting = summary::SummaryBatches::waitingCount(m_session.database());
        arrived = summary::SummaryBatches::arrivedCount(m_session.database());
    } catch (const std::exception &) {
        // Only the reminder is missing. The list says what's wrong.
    }
    QStringList parts;
    if (arrived > 0)
        parts << (arrived == 1 ? tr("1 summary to review") : tr("%1 summaries to review").arg(arrived));
    if (waiting > 0)
        parts << (waiting == 1 ? tr("1 summary on its way") : tr("%1 summaries on their way").arg(waiting));
    m_summaryStatus->setText(parts.join(QStringLiteral(", ")));
    m_summaryStatus->setVisible(!parts.isEmpty());
}

void MainWindow::showSummariesDialog(std::optional<qint64> publicationId)
{
    SummariesDialog dialog(m_session, *m_batches, this);
    connect(&dialog, &SummariesDialog::checkRequested, this, &MainWindow::checkSummaryBatches);
    if (publicationId)
        dialog.showPublication(*publicationId);
    dialog.exec();
    refreshSummaryStatus();
    if (m_details->currentId())
        m_details->reload();
}

void MainWindow::checkSummaryBatches()
{
    qint64 waiting = 0;
    guarded(tr("Reading the summaries on their way"), [&] { waiting = summary::SummaryBatches::waitingCount(m_session.database()); });
    if (waiting == 0) {
        m_batchTimer->stop();
        return;
    }
    if (!m_batchTimer->isActive())
        m_batchTimer->start();
    m_keys->read([this](const QString &key) {
        if (key.isEmpty())
            return; // Kept until there's a key to ask with.
        m_batches->setApiKey(key);
        m_batches->check();
    });
}

void MainWindow::showSummaryDialog(qint64 publicationId)
{
    SummaryDialog dialog(m_session, *m_keys, *m_summaryTransport, publicationId, this);
    dialog.exec();
    if (dialog.kept())
        m_details->reload();
}

void MainWindow::showContentsDialog(qint64 volumeId)
{
    ContentsDialog dialog(m_session, volumeId, this);
    if (dialog.exec() == QDialog::Accepted) {
        QStringList done;
        if (dialog.createdCount() > 0 || dialog.namedCount() == 0)
            done << tr("Created %1.").arg(publicationCount(dialog.createdCount()));
        if (dialog.namedCount() > 0)
            done << tr("Named %1.").arg(publicationCount(dialog.namedCount()));
        statusBar()->showMessage(done.join(u' '), 8000);
        guarded(tr("Tidying the issue"), [&] { import::tidyJournalIssue(m_session.database(), volumeId); });
        // A table of contents gives titles but not authors, so their details are looked up as a new file's would be.
        lookUp(dialog.articleIds());
    }
    if (dialog.result() == QDialog::Accepted || dialog.filledNumbering()) {
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
        const QList<qint64> made = createArticles(m_session.database(), volumeId, {dialog.draft()}, false);
        import::tidyJournalIssue(m_session.database(), volumeId);
        lookUp(made);
        m_details->reload();
        refreshNow();
    });
}

void MainWindow::askToRemoveSelected()
{
    const qint64 count = selectedCount();
    if (count == 0)
        return;
    QList<qint64> ids;
    guarded(tr("Reading the library"), [&] { ids = selectedIds(); });
    if (ids.isEmpty())
        return;
    QString what = count == 1 ? tr("this publication") : tr("these %1 publications").arg(QLocale().toString(count));
    if (count == 1) {
        if (const std::optional<Publication> one = m_session.publications().get(ids.first()))
            what = QStringLiteral("\"%1\"").arg(one->title);
    }
    QMessageBox box(QMessageBox::Question, count == 1 ? tr("Remove publication") : tr("Remove publications"),
                    tr("Remove %1 from the library?\n\nTheir details, tags, notes and highlights are deleted, with any articles "
                       "inside them. Your files aren't touched, and won't be added again when found. They can be brought "
                       "back from Library folders.")
                        .arg(what),
                    QMessageBox::NoButton, this);
    box.setTextFormat(Qt::PlainText);
    QPushButton *remove = box.addButton(tr("Remove"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("removePublications"));
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() == remove)
        removePublications(ids);
}

void MainWindow::removePublications(const QList<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    // Open documents and the details panel let go first, since they save as they close.
    QSet<qint64> gone(ids.cbegin(), ids.cend());
    guarded(tr("Reading the library"), [&] {
        for (const qint64 id : ids) {
            for (const qint64 child : m_session.publications().childIds(id))
                gone.insert(child);
        }
    });
    for (int i = m_documentTabs->count() - 1; i > 0; --i) {
        const auto *reader = qobject_cast<ReaderTab *>(m_documentTabs->widget(i));
        if (reader && gone.contains(reader->publicationId()))
            closeDocumentTab(i);
    }
    if (const std::optional<qint64> shown = m_details->currentId(); shown && gone.contains(*shown)) {
        m_selectedId.reset();
        m_details->showNothing();
    }
    const qint64 when = QDateTime::currentMSecsSinceEpoch();
    m_bulk->enqueue(tr("Removing"), ids, [when](Database &db, const QList<qint64> &batch) {
        RemovedContent(db).remove(batch, when);
    });
}

void MainWindow::showRemovedPublications()
{
    RemovedPublicationsDialog dialog(m_session, m_foldersDialog ? static_cast<QWidget *>(m_foldersDialog) : this);
    m_removedDialog = &dialog;
    dialog.exec();
    m_removedDialog = nullptr;
    if (m_foldersDialog)
        m_foldersDialog->reload();
    if (dialog.filesBroughtBack() > 0) {
        m_importStoppedByHand = false;
        rescanAll(); // Reads them again, as new files.
    }
}

void MainWindow::askToDeleteArticle(qint64 articleId)
{
    guarded(tr("Deleting the article"), [&] {
        const std::optional<Publication> article = m_session.publications().get(articleId);
        if (!article || !article->parentId)
            return;
        QMessageBox box(QMessageBox::Question, tr("Delete article"),
                        tr("Delete the article \"%1\"? Its tags and notes will be gone for good. What it's part of, and its file, aren't touched.")
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
            const QString name = escapedForMenu(collection.path);
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
    bool notices = true;
    guarded(tr("Reading settings"), [&] { notices = m_session.settings().value(QStringLiteral("metadata.check_notices")).toBool(); });
    OnlineLookupDialog dialog(QString(), lookupMode(), email, true, this);
    dialog.setCheckNotices(notices);
    if (dialog.exec() == QDialog::Accepted) {
        guarded(tr("Saving settings"),
                [&] { m_session.settings().setValue(QStringLiteral("metadata.check_notices"), dialog.checkNotices()); });
        setLookupMode(dialog.mode(), dialog.email().isNull() ? QStringLiteral("") : dialog.email());
        if (dialog.checkNotices())
            m_lookup->checkNotices();
    }
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

ReviewTab *MainWindow::reviewTab() const
{
    return m_reviewTab;
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

void MainWindow::offerWhatsDue()
{
    offerRetractionsIfDue();
    offerReviewIfDue();
}

// What's new since the user last looked is counted in the status bar. The first time a library's opened by a version
// that does this, whatever it already had counts as seen: the status bar and covers said so then.
void MainWindow::updateRetractionWarning()
{
    guarded(tr("Reading the notices"), [&] {
        NoticeRepository notices(m_session.database());
        SettingsStore &settings = m_session.settings();
        const QString key = QStringLiteral("notices.seen_until");
        auto seenUntil = qint64(settings.value(key).toDouble());
        if (seenUntil == 0) {
            seenUntil = std::max<qint64>(notices.newestFoundAt(), 1);
            settings.setValue(key, double(seenUntil));
        }
        const qint64 count = notices.unseenWarnings(seenUntil).size();
        m_retractionStatus->setText(count == 1 ? tr("1 new retraction or concern")
                                               : tr("%1 new retractions or concerns").arg(QLocale().toString(count)));
        m_retractionStatus->setVisible(count > 0);
        if (count == 0 && m_retractionOffer)
            m_retractionOffer->close();
    });
    offerWhatsDue();
}

// Like the offer to review, it doesn't block the window or take the keyboard, and waits rather than interrupt someone
// reading. Each one is offered once; after that the status bar keeps count.
void MainWindow::offerRetractionsIfDue()
{
    guarded(tr("Reading the notices"), [&] {
        if (m_retractionOffer || m_reviewOffer)
            return;
        if (m_documentTabs->currentIndex() != 0 || QApplication::activeModalWidget() || isMinimized())
            return;
        SettingsStore &settings = m_session.settings();
        const auto seenUntil = qint64(settings.value(QStringLiteral("notices.seen_until")).toDouble());
        const auto offeredUntil = qint64(settings.value(QStringLiteral("notices.offered_until")).toDouble());
        const QList<NewWarning> warnings = NoticeRepository(m_session.database()).unseenWarnings(seenUntil);
        if (warnings.isEmpty() || warnings.first().foundAt <= offeredUntil)
            return;
        settings.setValue(QStringLiteral("notices.offered_until"), double(warnings.first().foundAt));

        // Titles come from documents, so this is all plain text.
        QStringList lines;
        constexpr int kListed = 5;
        for (int i = 0; i < std::min<int>(kListed, int(warnings.size())); ++i) {
            const NewWarning &warning = warnings.at(i);
            const QString title = warning.title.trimmed().isEmpty() ? tr("Untitled") : warning.title.simplified();
            lines << tr("%1: %2").arg(noticeWarningLabel(warning.notice.kind, warning.notice.date), title);
        }
        if (warnings.size() > kListed)
            lines << tr("And %1 more.").arg(QLocale().toString(warnings.size() - kListed));

        auto *box = new QMessageBox(
            QMessageBox::Warning, tr("New retractions"),
            warnings.size() == 1
                ? tr("A publication in your library has been retracted, withdrawn or had concerns raised about it.")
                : tr("%1 in your library have been retracted, withdrawn or had concerns raised about them.")
                      .arg(publicationCount(warnings.size())),
            QMessageBox::NoButton, this);
        box->setObjectName(QStringLiteral("retractionOffer"));
        box->setTextFormat(Qt::PlainText);
        box->setInformativeText(lines.join(u'\n'));
        box->setWindowModality(Qt::NonModal);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setAttribute(Qt::WA_ShowWithoutActivating);
        QPushButton *show = box->addButton(tr("Show them"), QMessageBox::AcceptRole);
        show->setObjectName(QStringLiteral("showRetractions"));
        QPushButton *later = box->addButton(tr("Later"), QMessageBox::RejectRole);
        later->setObjectName(QStringLiteral("retractionsLater"));
        box->setDefaultButton(show);
        box->setEscapeButton(later);
        connect(show, &QPushButton::clicked, this, &MainWindow::showNewRetractions);
        // The offer to review waits for this one.
        connect(box, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &MainWindow::offerReviewIfDue); });
        m_retractionOffer = box;
        box->show();
    });
}

// Shows everything retracted or of concern, and counts what's new as seen.
void MainWindow::showNewRetractions()
{
    guarded(tr("Reading the notices"), [&] {
        SettingsStore &settings = m_session.settings();
        const QString key = QStringLiteral("notices.seen_until");
        const QList<NewWarning> warnings =
            NoticeRepository(m_session.database()).unseenWarnings(qint64(settings.value(key).toDouble()));
        if (!warnings.isEmpty())
            settings.setValue(key, double(warnings.first().foundAt));
    });
    m_documentTabs->setCurrentIndex(0);
    SidebarChoices choices = m_sidebar->choices();
    QList<QPair<QString, QString>> &status = choices.values[FilterSidebar::Status];
    const bool chosen = std::any_of(status.cbegin(), status.cend(), [](const QPair<QString, QString> &value) {
        return value.first == QLatin1StringView("retracted");
    });
    if (!chosen) {
        status.append({QStringLiteral("retracted"), tr("Retracted or of concern")});
        m_sidebar->setChoices(choices);
        applyFilters();
    }
    updateRetractionWarning();
}

// Comes up once everything has finished, so that a large import doesn't bring up one after another. It
// doesn't block the window or take the keyboard, and it waits rather than interrupt someone reading.
void MainWindow::offerReviewIfDue()
{
    guarded(tr("Reading the lookups"), [&] {
        LookupRepository lookups(m_session.database());
        const qint64 count = lookups.waitingForReviewCount();
        if (count <= m_reviewCountSeen || m_reviewOffer || m_retractionOffer)
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
    connect(&dialog, &LibraryFoldersDialog::rescanRequested, this, [this](const QList<qint64> &roots) {
        m_importStoppedByHand = false;
        if (!roots.isEmpty())
            m_import->scanRoots(roots);
    });
    connect(&dialog, &LibraryFoldersDialog::changeLocationRequested, this, &MainWindow::changeRootLocation);
    connect(&dialog, &LibraryFoldersDialog::removeRequested, this, &MainWindow::removeRoots);
    connect(&dialog, &LibraryFoldersDialog::forgetRequested, this, &MainWindow::confirmForgetHidden);
    connect(&dialog, &LibraryFoldersDialog::forgetWaitingRequested, this, &MainWindow::confirmForgetWaiting);
    connect(&dialog, &LibraryFoldersDialog::removedListRequested, this, &MainWindow::showRemovedPublications);
    dialog.exec();
    m_foldersDialog = nullptr;
    offerWhatsDue();
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
    removeRoots({rootId});
}

void MainWindow::removeRoots(const QList<qint64> &rootIds)
{
    guarded(tr("Removing the folder"), [&] {
        QStringList paths;
        QList<qint64> ids;
        for (const qint64 rootId : rootIds) {
            if (const auto root = m_session.files().root(rootId)) {
                paths << QDir::toNativeSeparators(root->path);
                ids << rootId;
            }
        }
        if (ids.isEmpty())
            return;
        const FileRepository::RemovalImpact impact = m_session.files().removalImpact(ids);
        const QString question = ids.size() == 1
            ? tr("Stop indexing %1?\n\nNothing is deleted from your disk.").arg(paths.first())
            : tr("Stop indexing these %1 folders?\n\n%2\n\nNothing is deleted from your disk.").arg(ids.size()).arg(paths.join(u'\n'));
        QMessageBox box(QMessageBox::Question, ids.size() == 1 ? tr("Remove folder") : tr("Remove folders"), question,
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
            removeRootsKeepingDetails(ids);
        else if (forget && box.clickedButton() == forget)
            removeRootsAndDetails(ids);
    });
}

void MainWindow::removeRootsKeepingDetails(const QList<qint64> &rootIds)
{
    guarded(tr("Removing the folder"), [&] {
        for (const qint64 rootId : rootIds) {
            m_watcher->removeRoot(rootId);
            m_session.files().hideRoot(rootId);
        }
        afterFoldersChanged();
    });
}

void MainWindow::removeRootsAndDetails(const QList<qint64> &rootIds)
{
    guarded(tr("Removing the folder"), [&] {
        // All are hidden first, so that something in two of them is counted as leaving.
        for (const qint64 rootId : rootIds) {
            m_watcher->removeRoot(rootId);
            m_session.files().hideRoot(rootId);
        }
        QList<qint64> ids;
        for (const qint64 rootId : rootIds)
            ids += m_session.files().hiddenIn(rootId);
        letGoOfHidden(); // Readers and notes save as they close, so this has to come first.
        for (const qint64 rootId : rootIds)
            m_session.files().removeRoot(rootId);
        const QSet<qint64> unique(ids.cbegin(), ids.cend());
        deletePublications(QList<qint64>(unique.cbegin(), unique.cend()));
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

void MainWindow::confirmForgetWaiting()
{
    guarded(tr("Deleting details"), [&] {
        const qint64 waiting = m_session.files().waitingPublicationCount();
        QMessageBox box(QMessageBox::Warning, tr("Delete details"),
                        tr("Delete the details of %1 waiting for their files?\n\nTheir tags, notes and highlights will be gone "
                           "for good, and they won't appear if their files are found later. Your files aren't touched.")
                            .arg(publicationCount(waiting)),
                        QMessageBox::NoButton, m_foldersDialog ? static_cast<QWidget *>(m_foldersDialog) : this);
        box.setTextFormat(Qt::PlainText);
        QPushButton *confirm = box.addButton(tr("Delete details"), QMessageBox::DestructiveRole);
        confirm->setObjectName(QStringLiteral("forgetWaiting"));
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != confirm)
            return;
        // Their waiting goes with them.
        deletePublications(m_session.files().waitingPublications());
        afterFoldersChanged();
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
        offerWhatsDue();
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
        && report.unreadable == 0 && report.reclassified == 0 && report.formatsJoined == 0 && report.keptOut == 0) {
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
    if (report.keptOut > 0)
        parts << tr("%1 left out, as you removed them earlier").arg(report.keptOut);
    statusBar()->showMessage(tr("Import finished: %1.").arg(parts.join(QStringLiteral(", "))), 15000);
}

QString MainWindow::licencesFolder()
{
    // Beside the program on Windows, and under share in Linux packages, wherever they're installed.
    const QDir program(QCoreApplication::applicationDirPath());
    for (const QString &candidate : {QStringLiteral("licenses"), QStringLiteral("../share/licenses/papertrail")}) {
        const QString folder = QDir::cleanPath(program.filePath(candidate));
        if (QFileInfo::exists(QDir(folder).filePath(QStringLiteral("THIRD-PARTY.md"))))
            return folder;
    }
    return QString();
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About Papertrail"));
    box.setIconPixmap(windowIcon().pixmap(64, 64));
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.setText(tr("<h3>Papertrail %1</h3>"
                   "<p>A library manager and reader for academic publications.</p>"
                   "<p>Free software under the GNU Affero General Public License, version 3 or later. "
                   "It comes with no warranty. The source code is at <a href=\"%2\">%2</a>.</p>"
                   "<p>It includes Qt, MuPDF, DjVuLibre, libmobi, SQLite, QtKeychain and zlib, among others, "
                   "each under its own licence.</p>")
                    .arg(QCoreApplication::applicationVersion().toHtmlEscaped(), QStringLiteral(PAPERTRAIL_SOURCE_URL)));
    const QString licences = licencesFolder();
    QPushButton *show = nullptr;
    if (!licences.isEmpty())
        show = box.addButton(tr("Show licences"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    // Links in the text open in the browser, without closing the box.
    if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label")))
        label->setOpenExternalLinks(true);
    box.exec();
    if (show && box.clickedButton() == show)
        QDesktopServices::openUrl(QUrl::fromLocalFile(licences));
}

} // namespace papertrail::ui
