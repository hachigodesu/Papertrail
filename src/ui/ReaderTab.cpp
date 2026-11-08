#include "ui/ReaderTab.h"

#include "core/repo/ReadingStateRepository.h"
#include "core/repo/VolumeArticles.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QIcon>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QRegularExpression>
#include <QSaveFile>
#include <QAbstractListModel>
#include <QAction>
#include <QCache>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QActionGroup>
#include <QEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace papertrail::ui {

using namespace core;

namespace {

// A token for finding a highlight holds which round it's from, as well as which highlight.
constexpr qint64 kTokenRound = 1000000000000;

constexpr int kNoPageRole = Qt::UserRole + 1;
constexpr int kPageRole = Qt::UserRole + 2;
constexpr int kPointRole = Qt::UserRole + 3;
constexpr double kActualSize = 96.0 / 72.0;
constexpr double kMinFontSize = 8;
constexpr double kMaxFontSize = 32;
constexpr int kThumbnailWidth = 120;
constexpr int kCustomZoomIndex = 99;

// "Stored" values for how the zoom was set, in the reading state's zoom column.
constexpr double kStoredFitWidth = -1;
constexpr double kStoredFitPage = -2;

QString settingForLayout(PageView::LayoutMode mode)
{
    switch (mode) {
    case PageView::LayoutMode::SinglePage: return QStringLiteral("single");
    case PageView::LayoutMode::TwoPages: return QStringLiteral("two_page");
    default: return QStringLiteral("continuous");
    }
}

PageView::LayoutMode layoutFromSetting(const QString &value)
{
    if (value == QLatin1StringView("single"))
        return PageView::LayoutMode::SinglePage;
    if (value == QLatin1StringView("two_page"))
        return PageView::LayoutMode::TwoPages;
    return PageView::LayoutMode::Continuous;
}

QString settingForColours(PageView::Colours colours)
{
    switch (colours) {
    case PageView::Colours::Dark: return QStringLiteral("dark");
    case PageView::Colours::Sepia: return QStringLiteral("sepia");
    default: return QStringLiteral("light");
    }
}

PageView::Colours coloursFromSetting(const QString &value)
{
    if (value == QLatin1StringView("dark"))
        return PageView::Colours::Dark;
    if (value == QLatin1StringView("sepia"))
        return PageView::Colours::Sepia;
    return PageView::Colours::Light;
}

// Contents entries wrap onto as many lines as they need at the panel's width, with room between them, so a
// long title can be read in full.
class WrappingContentsDelegate : public QStyledItemDelegate {
public:
    explicit WrappingContentsDelegate(QTreeView *view) : QStyledItemDelegate(view), m_view(view) {}

    static constexpr int kPadding = 6; // Above and below each entry.

    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->features |= QStyleOptionViewItem::WrapText;
        option->textElideMode = Qt::ElideNone;
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        int depth = 0;
        for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent())
            ++depth;
        const int indent = m_view->indentation() * (depth + (m_view->rootIsDecorated() ? 1 : 0));
        const int margin = m_view->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, m_view) + 1;
        const int width = qMax(40, m_view->viewport()->width() - indent - 2 * margin);
        const QString text = index.data(Qt::DisplayRole).toString();
        const QRect bounds = option.fontMetrics.boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, text);
        return QSize(width, bounds.height() + 2 * kPadding);
    }

private:
    QTreeView *m_view;
};

// Lays the contents out again when the panel's width changes, since that changes how titles wrap.
class RelayoutOnResize : public QObject {
public:
    explicit RelayoutOnResize(QTreeView *view) : QObject(view), m_viewport(view->viewport())
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(30);
        QObject::connect(&m_timer, &QTimer::timeout, view, [view] { view->doItemsLayout(); });
        view->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // Compared, not asked for: this sees the viewport's events while the view is being destroyed.
        if (watched == m_viewport && event->type() == QEvent::Resize)
            m_timer.start();
        return QObject::eventFilter(watched, event);
    }

private:
    QObject *m_viewport;
    QTimer m_timer;
};

QIcon colourSwatch(const QString &colour)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(QColor(colour));
    return QIcon(pixmap);
}

} // namespace

// Small pictures of the pages, fetched only for the ones in view.
class ThumbnailModel : public QAbstractListModel {
public:
    ThumbnailModel(DocumentClient &client, QObject *parent) : QAbstractListModel(parent), m_client(client)
    {
        m_cache.setMaxCost(400);
    }

    void setPages(const QList<QSizeF> &sizes)
    {
        beginResetModel();
        m_sizes = sizes;
        m_cache.clear();
        m_asked.clear();
        endResetModel();
    }

    void setPageSizes(int start, const QList<QSizeF> &sizes)
    {
        for (qsizetype i = 0; i < sizes.size(); ++i) {
            if (start + i < m_sizes.size() && !sizes.at(i).isEmpty())
                m_sizes[start + i] = sizes.at(i);
        }
    }

    void setThumbnail(int page, const QImage &image)
    {
        m_cache.insert(page, new QPixmap(QPixmap::fromImage(image)));
        emit dataChanged(index(page), index(page), {Qt::DecorationRole});
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_sizes.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return {};
        const int page = index.row();
        if (role == Qt::DisplayRole)
            return QString::number(page + 1);
        if (role == Qt::TextAlignmentRole)
            return int(Qt::AlignHCenter | Qt::AlignTop);
        if (role == Qt::SizeHintRole)
            return QSize(kThumbnailWidth + 24, kThumbnailWidth * 3 / 2 + 28);
        if (role == Qt::DecorationRole) {
            if (const QPixmap *pixmap = m_cache.object(page))
                return *pixmap;
            const double width = m_sizes.value(page).width();
            if (width > 0 && !m_asked.contains(page)) {
                m_asked.insert(page);
                m_client.requestThumbnail(page, double(kThumbnailWidth) / width);
            }
            QPixmap blank(kThumbnailWidth, kThumbnailWidth * 4 / 3);
            blank.fill(Qt::white);
            return blank;
        }
        return {};
    }

private:
    DocumentClient &m_client;
    QList<QSizeF> m_sizes;
    mutable QCache<int, QPixmap> m_cache;
    mutable QSet<int> m_asked;
};

ReaderTab::ReaderTab(LibrarySession &session, qint64 publicationId, qint64 fileId, const QString &format,
                     const QString &path, const QString &convertedPath, QWidget *parent)
    : QWidget(parent), m_session(session), m_publicationId(publicationId), m_fileId(fileId), m_format(format), m_path(path)
{
    m_openLink = [](const QUrl &url) { QDesktopServices::openUrl(url); };
    m_layout.fontSize = session.settings().value(QStringLiteral("reader.font_size")).toDouble(12);
    try {
        const std::optional<Publication> publication = session.publications().get(publicationId);
        if (publication && publication->parentId && publication->pageStart) {
            const int first = *publication->pageStart - 1;
            m_articlePages = std::make_pair(first, qMax(first, publication->pageEnd.value_or(*publication->pageStart) - 1));
            const std::optional<Publication> holder = session.publications().get(*publication->parentId);
            const PublicationType type = holder ? holder->type : PublicationType::Other;
            m_wholeFileLabel = type == PublicationType::JournalIssue ? tr("Whole issue")
                : type == PublicationType::ConferenceProceedings  ? tr("All the proceedings")
                : type == PublicationType::Book || type == PublicationType::Textbook ? tr("Whole book")
                                                                                     : tr("Whole file");
        }
    } catch (const std::exception &) {
        // Shown as a whole file, like anything else.
    }
    m_client = new DocumentClient(session.paths().workerPath, this);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    m_view = new PageView(this);
    m_view->setLayoutMode(layoutFromSetting(session.settings().value(QStringLiteral("reader.page_layout")).toString()));
    m_view->setColours(coloursFromSetting(session.settings().value(QStringLiteral("reader.page_colours")).toString()));

    buildToolbar();
    column->addWidget(m_toolbar);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    buildSidePanel();
    m_splitter->addWidget(m_side);

    m_stack = new QStackedWidget(m_splitter);
    auto *waiting = new QLabel(tr("Opening..."), m_stack);
    waiting->setAlignment(Qt::AlignCenter);
    m_stack->addWidget(waiting);
    m_stack->addWidget(m_view);

    auto *problemPage = new QWidget(m_stack);
    auto *problemColumn = new QVBoxLayout(problemPage);
    problemColumn->addStretch();
    m_problem = new QLabel(problemPage);
    m_problem->setTextFormat(Qt::PlainText);
    m_problem->setAlignment(Qt::AlignCenter);
    m_problem->setWordWrap(true);
    problemColumn->addWidget(m_problem);
    m_openElsewhere = new QPushButton(tr("Open in another app"), problemPage);
    connect(m_openElsewhere, &QPushButton::clicked, this, [this] { emit openElsewhereRequested(m_path); });
    problemColumn->addWidget(m_openElsewhere, 0, Qt::AlignCenter);
    problemColumn->addStretch();
    m_stack->addWidget(problemPage);
    m_splitter->addWidget(m_stack);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(1, false);
    m_splitter->setSizes({220, 1000});
    m_side->hide();
    column->addWidget(m_splitter, 1);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(2000);
    connect(m_saveTimer, &QTimer::timeout, this, &ReaderTab::saveState);

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(350);
    connect(m_searchTimer, &QTimer::timeout, this, &ReaderTab::applySearch);

    connectClient();
    m_toolbar->setEnabled(false);
    m_client->open(path, m_layout, convertedPath);
}

ReaderTab::~ReaderTab()
{
    saveState();
}

void ReaderTab::buildToolbar()
{
    m_toolbar = new QToolBar(this);
    m_toolbar->setMovable(false);
    m_toolbar->setFloatable(false);

    const auto button = [this](const QString &text, const QString &tip, const auto &action) {
        auto *tool = new QToolButton(m_toolbar);
        tool->setText(text);
        tool->setToolTip(tip);
        tool->setAccessibleName(tip);
        connect(tool, &QToolButton::clicked, this, action);
        return tool;
    };

    m_sideToggle = button(tr("Contents"), tr("Show or hide the contents and page thumbnails"), [this] {
        setSidePanelVisible(!m_side->isVisible());
    });
    m_toolbar->addWidget(m_sideToggle);

    // An article can be read on its own, or in the whole of what it's part of.
    m_wholeFile = button(m_wholeFileLabel, tr("Show the rest of the file this article is in, or only this article"),
                         [this] { setArticleOnly(!m_articleOnly); });
    m_wholeFile->setCheckable(true);
    m_wholeFileAction = m_toolbar->addWidget(m_wholeFile);
    m_wholeFileAction->setVisible(false);
    m_toolbar->addSeparator();
    m_highlightAction = m_toolbar->addAction(tr("Highlight"), this, [this] { annotateSelection(AnnotationType::Highlight); });
    m_highlightAction->setToolTip(tr("Highlights the selected text. Right-click it for colours, underlining and notes."));
    m_highlightAction->setEnabled(false);
    m_toolbar->addSeparator();

    m_backButton = button(QStringLiteral("←"), tr("Back to where the last link was followed"), [this] { m_view->goBack(); });
    m_backButton->setEnabled(false);
    m_toolbar->addWidget(m_backButton);
    m_toolbar->addWidget(button(QStringLiteral("▲"), tr("Previous page"), [this] { m_view->previousPage(); }));
    m_toolbar->addWidget(button(QStringLiteral("▼"), tr("Next page"), [this] { m_view->nextPage(); }));

    m_pageBox = new QLineEdit(m_toolbar);
    m_pageBox->setMaximumWidth(60);
    m_pageBox->setAlignment(Qt::AlignRight);
    m_pageBox->setAccessibleName(tr("Page number"));
    connect(m_pageBox, &QLineEdit::returnPressed, this, [this] {
        m_view->goToPage(m_view->firstShownPage() + m_pageBox->text().toInt() - 1);
        m_view->setFocus();
    });
    m_toolbar->addWidget(m_pageBox);
    m_pageTotal = new QLabel(m_toolbar);
    m_pageTotal->setContentsMargins(4, 0, 8, 0);
    m_toolbar->addWidget(m_pageTotal);
    m_toolbar->addSeparator();

    m_toolbar->addWidget(button(QStringLiteral("−"), tr("Zoom out"), [this] { m_view->zoomBy(1 / 1.15); }));
    m_zoomBox = new QComboBox(m_toolbar);
    m_zoomBox->setAccessibleName(tr("Zoom"));
    m_zoomBox->addItem(tr("Fit width"), kStoredFitWidth);
    m_zoomBox->addItem(tr("Fit page"), kStoredFitPage);
    for (const int percent : {50, 75, 100, 125, 150, 200, 300, 400})
        m_zoomBox->addItem(QStringLiteral("%1%").arg(percent), percent / 100.0);
    connect(m_zoomBox, &QComboBox::activated, this, [this](int index) {
        const double value = m_zoomBox->itemData(index).toDouble();
        if (value == kStoredFitWidth)
            m_view->setZoomMode(PageView::ZoomMode::FitWidth);
        else if (value == kStoredFitPage)
            m_view->setZoomMode(PageView::ZoomMode::FitPage);
        else
            m_view->setScale(value * kActualSize);
        m_view->setFocus();
    });
    m_toolbar->addWidget(m_zoomBox);
    m_toolbar->addWidget(button(QStringLiteral("+"), tr("Zoom in"), [this] { m_view->zoomBy(1.15); }));
    m_toolbar->addSeparator();

    m_layoutBox = new QComboBox(m_toolbar);
    m_layoutBox->setAccessibleName(tr("Page layout"));
    m_layoutBox->addItem(tr("Continuous"), int(PageView::LayoutMode::Continuous));
    m_layoutBox->addItem(tr("Single page"), int(PageView::LayoutMode::SinglePage));
    m_layoutBox->addItem(tr("Two pages"), int(PageView::LayoutMode::TwoPages));
    m_layoutBox->setCurrentIndex(m_layoutBox->findData(int(m_view->layoutMode())));
    connect(m_layoutBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto mode = PageView::LayoutMode(m_layoutBox->itemData(index).toInt());
        m_view->setLayoutMode(mode);
        m_session.settings().setValue(QStringLiteral("reader.page_layout"), settingForLayout(mode));
    });
    m_toolbar->addWidget(m_layoutBox);

    m_coloursBox = new QComboBox(m_toolbar);
    m_coloursBox->setAccessibleName(tr("Page colours"));
    m_coloursBox->addItem(tr("Light"), int(PageView::Colours::Light));
    m_coloursBox->addItem(tr("Dark"), int(PageView::Colours::Dark));
    m_coloursBox->addItem(tr("Sepia"), int(PageView::Colours::Sepia));
    m_coloursBox->setCurrentIndex(m_coloursBox->findData(int(m_view->colours())));
    connect(m_coloursBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto colours = PageView::Colours(m_coloursBox->itemData(index).toInt());
        m_view->setColours(colours);
        m_session.settings().setValue(QStringLiteral("reader.page_colours"), settingForColours(colours));
    });
    m_toolbar->addWidget(m_coloursBox);

    m_smallerText = button(tr("A-"), tr("Smaller text"), [this] { changeFontSize(-1); });
    m_largerText = button(tr("A+"), tr("Larger text"), [this] { changeFontSize(1); });
    m_smallerTextAction = m_toolbar->addWidget(m_smallerText);
    m_largerTextAction = m_toolbar->addWidget(m_largerText);

    auto *spacer = new QWidget(m_toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolbar->addWidget(spacer);

    m_search = new QLineEdit(m_toolbar);
    m_search->setPlaceholderText(tr("Search this document"));
    m_search->setAccessibleName(tr("Search this document"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(220);
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchTimer->start(); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (m_searchTimer->isActive())
            applySearch();
        else
            nextHit();
    });
    m_toolbar->addWidget(m_search);
    m_searchStatus = new QLabel(m_toolbar);
    m_searchStatus->setContentsMargins(6, 0, 6, 0);
    m_toolbar->addWidget(m_searchStatus);
    m_searchPrevious = button(QStringLiteral("↑"), tr("Previous match"), [this] { previousHit(); });
    m_searchNext = button(QStringLiteral("↓"), tr("Next match"), [this] { nextHit(); });
    m_toolbar->addWidget(m_searchPrevious);
    m_toolbar->addWidget(m_searchNext);
}

void ReaderTab::buildSidePanel()
{
    m_side = new QTabWidget(m_splitter);
    m_side->setDocumentMode(true);

    m_contents = new QTreeWidget(m_side);
    m_contents->setHeaderHidden(true);
    m_contents->setAccessibleName(tr("Table of contents"));
    m_contents->setExpandsOnDoubleClick(false);
    // Titles wrap at the panel's width rather than running off to the side.
    m_contents->header()->setStretchLastSection(true);
    m_contents->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contents->setWordWrap(true);
    m_contents->setTextElideMode(Qt::ElideNone);
    m_contents->setUniformRowHeights(false);
    m_contents->setItemDelegate(new WrappingContentsDelegate(m_contents));
    new RelayoutOnResize(m_contents);
    connect(m_contents, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) { onContentsActivated(item); });
    connect(m_contents, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { onContentsActivated(item); });
    m_side->addTab(m_contents, tr("Contents"));

    m_thumbnailModel = new ThumbnailModel(*m_client, this);
    m_thumbnails = new QListView(m_side);
    m_thumbnails->setModel(m_thumbnailModel);
    m_thumbnails->setUniformItemSizes(true);
    m_thumbnails->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_thumbnails->setAccessibleName(tr("Page thumbnails"));
    m_thumbnails->setIconSize(QSize(kThumbnailWidth, kThumbnailWidth * 3 / 2));
    connect(m_thumbnails, &QListView::clicked, this, [this](const QModelIndex &index) { m_view->goToPage(index.row()); });
    m_side->addTab(m_thumbnails, tr("Pages"));

    m_annotationPanel = new QWidget(m_side);
    auto *notes = new QVBoxLayout(m_annotationPanel);
    notes->setContentsMargins(0, 0, 0, 0);
    m_annotationList = new QListWidget(m_annotationPanel); // Items show text as it is, so a file's can't become formatting.
    m_annotationList->setAccessibleName(tr("Highlights and notes"));
    m_annotationList->setWordWrap(true);
    m_annotationList->setContextMenuPolicy(Qt::CustomContextMenu);
    // Several can be chosen with Ctrl and Shift, or all with Ctrl+A, and changed, copied, exported or deleted together.
    m_annotationList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_annotationList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { goToAnnotation(item->data(Qt::UserRole).toLongLong()); });
    connect(m_annotationList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        // Choosing several shouldn't jump about: only a single choice goes there.
        if (m_annotationList->selectedItems().size() == 1)
            goToAnnotation(item->data(Qt::UserRole).toLongLong());
    });
    connect(m_annotationList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = m_annotationList->itemAt(position);
        if (!item)
            return;
        // Right-clicking one that isn't chosen chooses just it, as file managers do.
        if (!item->isSelected()) {
            m_annotationList->clearSelection();
            item->setSelected(true);
            m_annotationList->setCurrentItem(item, QItemSelectionModel::NoUpdate);
        }
        QMenu menu(this);
        addChosenAnnotationActions(&menu);
        menu.exec(m_annotationList->viewport()->mapToGlobal(position));
    });
    auto *copyChosen = new QAction(tr("Copy"), m_annotationList);
    copyChosen->setShortcut(QKeySequence::Copy);
    copyChosen->setShortcutContext(Qt::WidgetShortcut);
    connect(copyChosen, &QAction::triggered, this, [this] { copyAnnotations(chosenAnnotations()); });
    m_annotationList->addAction(copyChosen);
    auto *deleteChosen = new QAction(tr("Delete"), m_annotationList);
    deleteChosen->setShortcut(QKeySequence::Delete);
    deleteChosen->setShortcutContext(Qt::WidgetShortcut);
    connect(deleteChosen, &QAction::triggered, this, [this] { askToDeleteAnnotations(chosenAnnotations()); });
    m_annotationList->addAction(deleteChosen);
    notes->addWidget(m_annotationList, 1);
    auto *exportButton = new QPushButton(tr("Export to Markdown..."), m_annotationPanel);
    connect(exportButton, &QPushButton::clicked, this, [this] { askToExportAnnotations(); });
    notes->addWidget(exportButton);
    m_side->addTab(m_annotationPanel, tr("Notes"));
}

void ReaderTab::connectClient()
{
    connect(m_client, &DocumentClient::opened, this, &ReaderTab::onOpened);
    connect(m_client, &DocumentClient::failed, this, &ReaderTab::showProblem);
    connect(m_client, &DocumentClient::sizesReady, this, [this](int start, const QList<QSizeF> &sizes) {
        if (start == 0 && !sizes.isEmpty() && !m_restoring && m_view->pageCount() > 0 && !m_ready) {
            m_view->setPageCount(m_view->pageCount(), sizes.first());
        }
        m_view->setPageSizes(start, sizes);
        m_thumbnailModel->setPageSizes(start, sizes);
        if (!m_ready) {
            // With a first page's size known, the view can show the right place straight away.
            m_ready = true;
            m_stack->setCurrentWidget(m_view);
            restorePlace();
            applyPendingMatch();
            loadAnnotations();
            if (m_pendingAnnotation)
                goToAnnotation(*std::exchange(m_pendingAnnotation, std::nullopt));
        }
    });
    connect(m_client, &DocumentClient::pageRendered, m_view, &PageView::setPageImage);
    connect(m_client, &DocumentClient::pageFailed, m_view, &PageView::setPageFailed);
    connect(m_client, &DocumentClient::thumbnailReady, m_thumbnailModel, &ThumbnailModel::setThumbnail);
    connect(m_client, &DocumentClient::linksReady, m_view, &PageView::setLinks);
    connect(m_client, &DocumentClient::outlineReady, this, &ReaderTab::onOutline);
    connect(m_client, &DocumentClient::selectionReady, m_view, &PageView::setSelection);
    connect(m_client, &DocumentClient::searchHits, this, &ReaderTab::onSearchHits);
    connect(m_client, &DocumentClient::searchFinished, this, [this] {
        m_searchRunning = false;
        updateSearchStatus();
    });
    connect(m_client, &DocumentClient::bookmarkReady, this, [this](int page, const QString &bookmark) {
        m_bookmark = bookmark;
        m_bookmarkPage = page;
    });
    connect(m_client, &DocumentClient::bookmarkResolved, this, [this](int page) {
        m_view->goToPage(page);
        m_restoring = false;
    });
    connect(m_client, &DocumentClient::layoutChanged, this, [this](int pageCount, int page) {
        m_view->forgetAllPages();
        m_view->setPageCount(pageCount, QSizeF(m_layout.width, m_layout.height));
        m_view->goToPage(page);
        emit statusMessage(QString());
        QList<QSizeF> sizes;
        sizes.fill(QSizeF(m_layout.width, m_layout.height), pageCount);
        m_thumbnailModel->setPages(sizes);
        m_client->requestOutline();
        updatePageBox();
        locateAnnotations();
        if (!m_activeSearch.isEmpty()) {
            const QString text = m_activeSearch;
            m_activeSearch.clear();
            m_search->setText(text);
            applySearch();
        }
    });
    connect(m_client, &DocumentClient::recovered, this, [this] {
        emit statusMessage(tr("A problem with this document was recovered from."));
    });

    connect(m_view, &PageView::pagesWanted, m_client, &DocumentClient::setWantedPages);
    connect(m_view, &PageView::linksWanted, m_client, &DocumentClient::requestLinks);
    connect(m_view, &PageView::selectionWanted, m_client, &DocumentClient::requestSelection);
    connect(m_view, &PageView::contextMenuWanted, this, &ReaderTab::addAnnotationActions);
    connect(m_view, &PageView::selectionChanged, this, [this] {
        if (m_highlightAction)
            m_highlightAction->setEnabled(m_view->hasSelection());
    });
    connect(m_client, &DocumentClient::anchored, this, &ReaderTab::onAnchored);
    connect(m_client, &DocumentClient::located, this, &ReaderTab::onLocated);
    connect(m_view, &PageView::currentPageChanged, this, [this] { updatePageBox(); });
    connect(m_view, &PageView::zoomChanged, this, [this] {
        updateZoomBox();
        m_saveTimer->start();
    });
    connect(m_view, &PageView::placeChanged, this, [this] {
        if (m_reflowable && m_view->topPage() != m_bookmarkPage) {
            m_bookmarkPage = m_view->topPage();
            m_client->requestBookmark(m_bookmarkPage);
        }
        if (!m_restoring)
            m_saveTimer->start();
    });
    connect(m_view, &PageView::externalLinkActivated, this, &ReaderTab::followExternalLink);
    connect(m_view, &PageView::historyChanged, this, [this] { m_backButton->setEnabled(m_view->canGoBack()); });
}

void ReaderTab::onOpened(int pageCount, bool reflowable)
{
    m_reflowable = reflowable;
    m_toolbar->setEnabled(true);
    m_smallerTextAction->setVisible(reflowable);
    m_largerTextAction->setVisible(reflowable);

    const QSizeF assumed = reflowable ? QSizeF(m_layout.width, m_layout.height) : QSizeF(595, 842);
    if (reflowable)
        m_view->setScale(kActualSize);
    m_view->setPageCount(pageCount, assumed);
    QList<QSizeF> sizes;
    sizes.fill(assumed, pageCount);
    m_thumbnailModel->setPages(sizes);
    m_pageTotal->setText(tr("of %1").arg(pageCount));
    applyArticleRange();
    updatePageBox();
    updateZoomBox();
    m_client->requestOutline();
}

void ReaderTab::setArticleOnly(bool only)
{
    m_articleOnly = only;
    applyArticleRange();
}

bool ReaderTab::pageIsShown(int page) const
{
    return !m_view->hasPageRange() || (page >= m_view->firstShownPage() && page <= m_view->lastShownPage());
}

void ReaderTab::ensurePageShown(int page)
{
    if (!pageIsShown(page))
        setArticleOnly(false);
}

void ReaderTab::applyArticleRange()
{
    // A page range only means anything where pages are fixed.
    const bool canLimit = m_articlePages && !m_reflowable && m_view->pageCount() > 0;
    if (m_wholeFileAction)
        m_wholeFileAction->setVisible(canLimit);
    if (m_wholeFile) {
        const QSignalBlocker blocker(m_wholeFile);
        m_wholeFile->setChecked(!m_articleOnly);
    }
    const bool limited = canLimit && m_articleOnly;
    const bool wasLimited = m_view->hasPageRange();
    if (limited)
        m_view->setPageRange(m_articlePages->first, m_articlePages->second);
    else if (wasLimited)
        m_view->setPageRange(-1, -1);

    for (int row = 0; row < m_thumbnailModel->rowCount(); ++row)
        m_thumbnails->setRowHidden(row, !pageIsShown(row));
    for (QTreeWidgetItemIterator it(m_contents); *it; ++it) {
        const int page = (*it)->data(0, kPageRole).toInt();
        (*it)->setHidden(page >= 0 && !pageIsShown(page));
    }
    updatePageBox();

    // Matches outside the article are left out while it's on its own, so a search runs again when that changes.
    if (limited != wasLimited && !m_activeSearch.isEmpty()) {
        const QString text = m_activeSearch;
        m_activeSearch.clear();
        m_search->setText(text);
        applySearch();
    }
}

void ReaderTab::restorePlace()
{
    QJsonObject place;
    try {
        place = ReadingStateRepository(m_session.database()).placeFor(m_publicationId, m_format);
    } catch (const std::exception &) {
        return;
    }
    if (place.isEmpty()) {
        // An article starts where it starts in its volume.
        try {
            const std::optional<Publication> publication = m_session.publications().get(m_publicationId);
            if (publication && publication->parentId && publication->pageStart && *publication->pageStart > 1)
                m_view->goToPage(*publication->pageStart - 1);
        } catch (const std::exception &) {
            // It'll just start at the beginning.
        }
        return;
    }

    if (place.contains(QStringLiteral("zoom"))) {
        const double zoom = place.value(QStringLiteral("zoom")).toDouble();
        if (zoom == kStoredFitWidth)
            m_view->setZoomMode(PageView::ZoomMode::FitWidth);
        else if (zoom == kStoredFitPage)
            m_view->setZoomMode(PageView::ZoomMode::FitPage);
        else if (zoom > 0)
            m_view->setScale(zoom);
    }

    const QString bookmark = place.value(QStringLiteral("bookmark")).toString();
    if (m_reflowable && !bookmark.isEmpty()
        && place.value(QStringLiteral("fontSize")).toDouble() != m_layout.fontSize) {
        // The same text is on a different page now.
        m_restoring = true;
        m_client->requestPageForBookmark(bookmark);
    } else {
        m_view->goToPage(place.value(QStringLiteral("page")).toInt(),
                         place.value(QStringLiteral("offset")).toDouble());
    }
}

void ReaderTab::saveState()
{
    if (!m_ready || m_failedToOpen)
        return;
    m_saveTimer->stop();
    QJsonObject place;
    place.insert(QStringLiteral("page"), m_view->topPage());
    place.insert(QStringLiteral("offset"), m_view->topOffset());
    if (m_reflowable && !m_bookmark.isEmpty()) {
        place.insert(QStringLiteral("bookmark"), m_bookmark);
        place.insert(QStringLiteral("fontSize"), m_layout.fontSize);
    }

    switch (m_view->zoomMode()) {
    case PageView::ZoomMode::FitWidth: place.insert(QStringLiteral("zoom"), kStoredFitWidth); break;
    case PageView::ZoomMode::FitPage: place.insert(QStringLiteral("zoom"), kStoredFitPage); break;
    case PageView::ZoomMode::Fixed: place.insert(QStringLiteral("zoom"), m_view->scale()); break;
    }
    try {
        ReadingStateRepository(m_session.database())
            .setPlaceFor(m_publicationId, m_format, place, QDateTime::currentMSecsSinceEpoch());
    } catch (const std::exception &) {
        // Losing the place isn't worth interrupting the reader for.
    }
}

void ReaderTab::showProblem(DocumentClient::Failure failure, const QString &message)
{
    m_failedToOpen = !m_ready;
    QString text;
    switch (failure) {
    case DocumentClient::Failure::NeedsPassword:
        text = tr("This file is protected by a password, and Papertrail can't open those yet.");
        break;
    case DocumentClient::Failure::Protected:
        text = tr("This book is locked with DRM, so it can only be read in the app it was bought for.");
        break;
    case DocumentClient::Failure::Crashed:
        text = m_ready ? tr("This file keeps making the reader stop, so it's been closed. Your library isn't affected.")
                       : tr("This file made the reader stop, so it may be damaged. Your library isn't affected.");
        break;
    case DocumentClient::Failure::TimedOut:
        text = tr("This file took too long to open, so it may be damaged.");
        break;
    case DocumentClient::Failure::CantStart:
        text = message;
        break;
    case DocumentClient::Failure::Unreadable:
        text = tr("This file can't be opened here. It may be damaged, or in a form Papertrail doesn't read yet.");
        break;
    }
    m_problem->setText(text);
    m_openElsewhere->setVisible(failure != DocumentClient::Failure::CantStart);
    m_stack->setCurrentIndex(2);
    m_toolbar->setEnabled(false);
    m_side->hide();
}

bool ReaderTab::isShowingProblem() const
{
    return m_stack->currentIndex() == 2;
}

void ReaderTab::updatePageBox()
{
    // Within an article on its own, pages count from its first.
    const int first = m_view->firstShownPage();
    const int shown = m_view->pageCount() > 0 ? m_view->lastShownPage() - first + 1 : 0;
    if (!m_pageBox->hasFocus())
        m_pageBox->setText(QString::number(m_view->currentPage() - first + 1));
    m_pageTotal->setText(tr("of %1").arg(shown));
    if (!m_pageBox->validator() || static_cast<const QIntValidator *>(m_pageBox->validator())->top() != shown)
        m_pageBox->setValidator(new QIntValidator(1, qMax(1, shown), m_pageBox));
}

void ReaderTab::updateZoomBox()
{
    const QSignalBlocker blocker(m_zoomBox);
    const int custom = m_zoomBox->findData(kCustomZoomIndex);
    if (custom >= 0)
        m_zoomBox->removeItem(custom);

    switch (m_view->zoomMode()) {
    case PageView::ZoomMode::FitWidth:
        m_zoomBox->setCurrentIndex(m_zoomBox->findData(kStoredFitWidth));
        return;
    case PageView::ZoomMode::FitPage:
        m_zoomBox->setCurrentIndex(m_zoomBox->findData(kStoredFitPage));
        return;
    case PageView::ZoomMode::Fixed:
        break;
    }
    const int percent = qRound(m_view->scale() / kActualSize * 100);
    const int index = m_zoomBox->findText(QStringLiteral("%1%").arg(percent));
    if (index >= 0) {
        m_zoomBox->setCurrentIndex(index);
    } else {
        m_zoomBox->addItem(QStringLiteral("%1%").arg(percent), kCustomZoomIndex);
        m_zoomBox->setCurrentIndex(m_zoomBox->count() - 1);
    }
}

void ReaderTab::onOutline(const QList<ReaderOutlineItem> &items)
{
    // Where the file's contents only number an entry, an article starting on that page gives its title.
    QHash<int, QString> articleTitles;
    try {
        const std::optional<Publication> publication = m_session.publications().get(m_publicationId);
        const qint64 owner = publication ? publication->parentId.value_or(m_publicationId) : m_publicationId;
        for (const qint64 id : m_session.publications().childIds(owner)) {
            const std::optional<Publication> article = m_session.publications().get(id);
            if (article && article->pageStart && !articleTitles.contains(*article->pageStart - 1))
                articleTitles.insert(*article->pageStart - 1, article->title);
        }
    } catch (const std::exception &) {
        // The file's own titles are shown.
    }

    m_contents->clear();
    QList<QTreeWidgetItem *> parents;
    for (const ReaderOutlineItem &item : items) {
        const int depth = qMin(item.depth, int(parents.size()));
        parents.resize(depth);
        QTreeWidgetItem *entry = parents.isEmpty() ? new QTreeWidgetItem(m_contents) : new QTreeWidgetItem(parents.last());
        // Tree items show text as it is, so a title from the file can't turn into formatting.
        QString title = item.title;
        if (isPlaceholderTitle(title) && articleTitles.contains(item.page))
            title = articleTitles.value(item.page);
        entry->setText(0, title);
        entry->setToolTip(0, title);
        entry->setData(0, kPageRole, item.page);
        entry->setData(0, kPointRole, item.y);
        entry->setData(0, kNoPageRole, item.page < 0);
        parents << entry;
    }
    m_contents->expandToDepth(0);
    applyArticleRange();

    // Opens with the contents showing if there are any, and on the page thumbnails if not.
    m_side->setCurrentWidget(items.isEmpty() ? static_cast<QWidget *>(m_thumbnails) : static_cast<QWidget *>(m_contents));
    if (!items.isEmpty() && !isShowingProblem())
        setSidePanelVisible(true);
}

void ReaderTab::setSidePanelVisible(bool visible)
{
    m_side->setVisible(visible);
    // A panel hidden from the start has no width of its own to come back to.
    if (visible && m_splitter->sizes().value(0) < 80)
        m_splitter->setSizes({220, qMax(200, m_splitter->width() - 220)});
}

void ReaderTab::onContentsActivated(QTreeWidgetItem *item)
{
    const int page = item->data(0, kPageRole).toInt();
    if (page >= 0)
        m_view->goToPoint(page, item->data(0, kPointRole).toDouble());
}

void ReaderTab::changeFontSize(double change)
{
    const double size = qBound(kMinFontSize, m_layout.fontSize + change, kMaxFontSize);
    if (size == m_layout.fontSize || !m_reflowable)
        return;
    m_layout.fontSize = size;
    m_session.settings().setValue(QStringLiteral("reader.font_size"), size);
    emit statusMessage(tr("Laying out the text again..."));
    m_client->setLayout(m_layout, m_view->topPage());
}

void ReaderTab::followExternalLink(const QString &uri)
{
    const QUrl safe = safeWebLink(uri);
    if (safe.isEmpty()) {
        emit statusMessage(tr("This link isn't a web address, so it wasn't opened."));
        return;
    }
    m_openLink(safe);
}

void ReaderTab::focusSearch()
{
    m_search->setFocus();
    m_search->selectAll();
}

QList<int> ReaderTab::markedPages() const
{
    return m_view->pagesWithMarks();
}

void ReaderTab::loadAnnotations()
{
    m_annotations.clear();
    m_annotationsElsewhere.clear();
    try {
        if (const std::optional<FileRecord> file = m_session.files().get(m_fileId))
            m_contentHash = file->sha256;
        AnnotationRepository annotations(m_session.database());
        m_annotations = annotations.forContents(m_contentHash);
        for (const Annotation &annotation : annotations.forPublication(m_publicationId)) {
            if (annotation.contentHash != m_contentHash)
                m_annotationsElsewhere << annotation;
        }
    } catch (const std::exception &error) {
        emit statusMessage(tr("The highlights couldn't be read: %1").arg(QString::fromUtf8(error.what())));
    }
    locateAnnotations();
}

// Fixed pages have what they need. Text that moves is looked for afresh, in the background.
void ReaderTab::locateAnnotations()
{
    m_view->clearMarks();
    m_locatedParts.clear();
    ++m_locateRound;
    for (const Annotation &annotation : std::as_const(m_annotations)) {
        const bool needsFinding = std::any_of(annotation.parts.cbegin(), annotation.parts.cend(),
                                              [](const AnnotationPart &part) { return part.rects.isEmpty() && !part.place.isEmpty(); });
        if (!needsFinding) {
            showMark(annotation);
            continue;
        }
        const qint64 token = m_locateRound * kTokenRound + annotation.id;
        for (int part = 0; part < annotation.parts.size(); ++part) {
            const AnnotationPart &found = annotation.parts.at(part);
            m_client->requestLocate(token, part, found.place, found.text, found.occurrence);
        }
    }
    refreshAnnotationList();
}

void ReaderTab::onLocated(qint64 token, int part, int page, const QList<QRectF> &rects)
{
    if (token / kTokenRound != m_locateRound)
        return;
    const qint64 id = token % kTokenRound;
    auto annotation = std::find_if(m_annotations.cbegin(), m_annotations.cend(), [id](const Annotation &a) { return a.id == id; });
    if (annotation == m_annotations.cend())
        return;
    QList<QPair<int, QList<QRectF>>> &parts = m_locatedParts[id];
    if (parts.size() < annotation->parts.size())
        parts.resize(annotation->parts.size());
    if (part < parts.size())
        parts[part] = qMakePair(page, rects);
    if (page >= 0) {
        // A selection across pages has a part on each, and each one's rectangles are drawn on its own page.
        QList<AnnotationMark> marks = m_view->marksOn(page);
        AnnotationMark mark;
        mark.id = id;
        mark.rects = rects;
        mark.colour = QColor(annotation->colour);
        mark.underline = annotation->type == AnnotationType::Underline;
        mark.hasNote = !annotation->note.isEmpty();
        marks << mark;
        m_view->setMarks(page, marks);
    }
    refreshAnnotationList();
}

void ReaderTab::showMark(const Annotation &annotation)
{
    for (const AnnotationPart &part : annotation.parts) {
        if (part.page < 0 || part.rects.isEmpty())
            continue;
        QList<AnnotationMark> marks = m_view->marksOn(part.page);
        AnnotationMark mark;
        mark.id = annotation.id;
        mark.rects = part.rects;
        mark.colour = QColor(annotation.colour);
        mark.underline = annotation.type == AnnotationType::Underline;
        mark.hasNote = !annotation.note.isEmpty();
        marks << mark;
        m_view->setMarks(part.page, marks);
    }
}

void ReaderTab::refreshAnnotationList()
{
    const auto describe = [](const Annotation &annotation) {
        QString text = annotation.selectedText().simplified();
        if (text.size() > 200)
            text = text.left(200) + QStringLiteral("...");
        if (!annotation.note.isEmpty())
            text += QStringLiteral("\n") + annotation.note;
        return text;
    };
    const auto swatch = [](const QString &colour) {
        QPixmap pixmap(12, 12);
        pixmap.fill(QColor(colour));
        return QIcon(pixmap);
    };

    const QSignalBlocker blocker(m_annotationList);
    const QList<qint64> chosenBefore = chosenAnnotations();
    m_annotationList->clear();
    for (const Annotation &annotation : std::as_const(m_annotations)) {
        int page = annotation.firstPage();
        bool found = true;
        if (const auto located = m_locatedParts.constFind(annotation.id); located != m_locatedParts.constEnd()) {
            page = -1;
            for (const auto &part : *located) {
                if (part.first >= 0 && page < 0)
                    page = part.first;
            }
            found = page >= 0;
        } else if (page < 0) {
            found = true; // Still being looked for.
        }
        QString heading = page >= 0 ? tr("Page %1").arg(page + 1) : QString();
        if (!found)
            heading = tr("Can't be found in this layout");
        auto *item = new QListWidgetItem(swatch(annotation.colour), (heading.isEmpty() ? QString() : heading + QStringLiteral("\n")) + describe(annotation),
                                         m_annotationList);
        item->setData(Qt::UserRole, annotation.id);
    }
    // Kept, and said to be from elsewhere: the text it was made on may not line up with this one.
    for (const Annotation &annotation : std::as_const(m_annotationsElsewhere)) {
        auto *item = new QListWidgetItem(swatch(annotation.colour), tr("From another copy or format\n") + describe(annotation), m_annotationList);
        item->setData(Qt::UserRole, annotation.id);
    }
    for (int row = 0; row < m_annotationList->count(); ++row) {
        QListWidgetItem *item = m_annotationList->item(row);
        if (chosenBefore.contains(item->data(Qt::UserRole).toLongLong()))
            item->setSelected(true);
    }
    if (m_side)
        m_side->setTabText(m_side->indexOf(m_annotationPanel),
                           m_annotationList->count() > 0 ? tr("Notes (%1)").arg(m_annotationList->count()) : tr("Notes"));
}

void ReaderTab::goToAnnotation(qint64 id)
{
    if (const auto located = m_locatedParts.constFind(id); located != m_locatedParts.constEnd()) {
        for (const auto &part : *located) {
            if (part.first >= 0) {
                ensurePageShown(part.first);
                m_view->goToPage(part.first);
                return;
            }
        }
    }
    for (const Annotation &annotation : std::as_const(m_annotations)) {
        if (annotation.id != id)
            continue;
        if (annotation.firstPage() >= 0) {
            const AnnotationPart &part = annotation.parts.first();
            ensurePageShown(part.page);
            m_view->goToPoint(part.page, part.rects.isEmpty() ? 0 : qMax(0.0, part.rects.first().top() - 40));
        } else if (!annotation.parts.isEmpty() && !annotation.parts.first().place.isEmpty()) {
            m_client->requestPageForBookmark(annotation.parts.first().place);
        }
        return;
    }
}

void ReaderTab::annotateSelection(AnnotationType type, const QString &note, const QString &colour)
{
    const QList<SelectionPart> parts = m_view->selectionParts();
    if (parts.isEmpty())
        return;

    Annotation annotation;
    annotation.publicationId = m_publicationId;
    annotation.contentHash = m_contentHash;
    annotation.type = type;
    annotation.colour = colour.isEmpty() ? annotationColours().first() : colour;
    annotation.note = note;

    if (!m_reflowable) {
        for (const SelectionPart &selected : parts) {
            AnnotationPart part;
            part.page = selected.page;
            part.rects = selected.rects;
            part.text = selected.text;
            annotation.parts << part;
        }
        try {
            AnnotationRepository(m_session.database()).create(annotation);
        } catch (const std::exception &error) {
            emit statusMessage(tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
            return;
        }
        m_annotations << annotation;
        m_view->clearSelection();
        showMark(annotation);
        refreshAnnotationList();
        emit annotationsChanged();
        return;
    }

    // Screen positions wouldn't last, so what will find this again has to be worked out by the document's process.
    const qint64 draft = m_nextDraft++;
    DraftAnnotation entry;
    entry.annotation = annotation;
    entry.waitingFor = int(parts.size());
    for (const SelectionPart &selected : parts) {
        AnnotationPart part;
        part.text = selected.text;
        entry.annotation.parts << part;
    }
    m_drafts.insert(draft, entry);
    for (int i = 0; i < parts.size(); ++i) {
        const SelectionPart &selected = parts.at(i);
        m_client->requestAnchor(draft, i, selected.page, selected.text,
                                selected.rects.isEmpty() ? QPointF() : selected.rects.first().topLeft());
    }
    m_view->clearSelection();
}

void ReaderTab::onAnchored(qint64 token, int part, const QString &place, int occurrence)
{
    auto draft = m_drafts.find(token);
    if (draft == m_drafts.end())
        return;
    if (part < draft->annotation.parts.size()) {
        draft->annotation.parts[part].place = place;
        draft->annotation.parts[part].occurrence = occurrence;
    }
    if (--draft->waitingFor > 0)
        return;

    Annotation annotation = draft->annotation;
    m_drafts.erase(draft);
    try {
        AnnotationRepository(m_session.database()).create(annotation);
    } catch (const std::exception &error) {
        emit statusMessage(tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    m_annotations << annotation;
    const qint64 ticket = m_locateRound * kTokenRound + annotation.id;
    for (int i = 0; i < annotation.parts.size(); ++i) {
        const AnnotationPart &found = annotation.parts.at(i);
        m_client->requestLocate(ticket, i, found.place, found.text, found.occurrence);
    }
    refreshAnnotationList();
    emit annotationsChanged();
}

void ReaderTab::setAnnotationNote(qint64 id, const QString &note)
{
    try {
        AnnotationRepository(m_session.database()).setNote(id, note);
    } catch (const std::exception &error) {
        emit statusMessage(tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    loadAnnotations();
    emit annotationsChanged();
}

void ReaderTab::setAnnotationColour(qint64 id, const QString &colour)
{
    setAnnotationsColour({id}, colour);
}

void ReaderTab::deleteAnnotation(qint64 id)
{
    deleteAnnotations({id});
}

void ReaderTab::setAnnotationsColour(const QList<qint64> &ids, const QString &colour)
{
    if (ids.isEmpty())
        return;
    try {
        Transaction transaction(m_session.database());
        AnnotationRepository annotations(m_session.database());
        for (const qint64 id : ids)
            annotations.setColour(id, colour);
        transaction.commit();
    } catch (const std::exception &error) {
        emit statusMessage(tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    loadAnnotations();
    emit annotationsChanged();
}

void ReaderTab::deleteAnnotations(const QList<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    try {
        Transaction transaction(m_session.database());
        AnnotationRepository annotations(m_session.database());
        for (const qint64 id : ids)
            annotations.remove(id);
        transaction.commit();
    } catch (const std::exception &error) {
        emit statusMessage(tr("That couldn't be deleted: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    loadAnnotations();
    emit annotationsChanged();
}

QList<qint64> ReaderTab::chosenAnnotations() const
{
    QList<qint64> ids;
    for (int row = 0; row < m_annotationList->count(); ++row) {
        const QListWidgetItem *item = m_annotationList->item(row);
        if (item->isSelected())
            ids << item->data(Qt::UserRole).toLongLong();
    }
    return ids;
}

QString ReaderTab::annotationsAsText(const QList<qint64> &ids) const
{
    QHash<qint64, const Annotation *> byId;
    for (const QList<Annotation> *list : {&m_annotations, &m_annotationsElsewhere}) {
        for (const Annotation &annotation : *list)
            byId.insert(annotation.id, &annotation);
    }
    QStringList parts;
    for (const qint64 id : ids) {
        const Annotation *annotation = byId.value(id);
        if (!annotation)
            continue;
        QStringList lines;
        const QString words = annotation->selectedText().simplified();
        const int page = annotation->firstPage();
        if (!words.isEmpty())
            lines << (page >= 0 ? tr("\u201C%1\u201D (page %2)").arg(words).arg(page + 1) : tr("\u201C%1\u201D").arg(words));
        else if (page >= 0)
            lines << tr("Page %1").arg(page + 1);
        if (!annotation->note.isEmpty())
            lines << annotation->note;
        if (!lines.isEmpty())
            parts << lines.join(u'\n');
    }
    return parts.join(QStringLiteral("\n\n"));
}

void ReaderTab::copyAnnotations(const QList<qint64> &ids) const
{
    const QString text = annotationsAsText(ids);
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void ReaderTab::askToDeleteAnnotations(const QList<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    // One at a time goes straight away, as before. Several are asked about, since that's harder to undo by hand.
    if (ids.size() > 1) {
        QMessageBox box(QMessageBox::Question, tr("Delete highlights and notes"),
                        tr("Delete these %1 highlights and notes? This can't be undone.").arg(ids.size()),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::Cancel);
        box.button(QMessageBox::Yes)->setText(tr("Delete %1").arg(ids.size()));
        if (box.exec() != QMessageBox::Yes)
            return;
    }
    deleteAnnotations(ids);
}

void ReaderTab::addChosenAnnotationActions(QMenu *menu)
{
    const QList<qint64> ids = chosenAnnotations();
    if (ids.isEmpty())
        return;
    if (ids.size() == 1) {
        const qint64 id = ids.first();
        menu->addAction(tr("Edit note..."), this, [this, id] { askToEditNote(id); });
    }
    addColourMenu(menu, ids);
    menu->addAction(ids.size() == 1 ? tr("Copy") : tr("Copy %1").arg(ids.size()), this, [this, ids] { copyAnnotations(ids); });
    menu->addAction(ids.size() == 1 ? tr("Export to Markdown...") : tr("Export %1 to Markdown...").arg(ids.size()), this,
                    [this, ids] { askToExportAnnotations(ids); });
    menu->addSeparator();
    menu->addAction(ids.size() == 1 ? tr("Delete") : tr("Delete %1...").arg(ids.size()), this, [this, ids] { askToDeleteAnnotations(ids); });
}

bool ReaderTab::exportAnnotations(const QString &path, const QList<qint64> &only)
{
    try {
        const std::optional<Publication> publication = m_session.publications().get(m_publicationId);
        if (!publication)
            return false;
        QList<Annotation> all = m_annotations;
        all += m_annotationsElsewhere;
        if (!only.isEmpty())
            all.removeIf([&only](const Annotation &annotation) { return !only.contains(annotation.id); });
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(annotationsToMarkdown(*publication, all).toUtf8());
        return file.commit();
    } catch (const std::exception &) {
        return false;
    }
}

void ReaderTab::askToExportAnnotations(const QList<qint64> &only)
{
    const std::optional<Publication> publication = m_session.publications().get(m_publicationId);
    QString name = publication ? publication->title.simplified() : tr("Notes");
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral(" "));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export highlights and notes"), name.left(80) + QStringLiteral(".md"),
                                                      tr("Markdown (*.md)"));
    if (path.isEmpty())
        return;
    emit statusMessage(exportAnnotations(path, only) ? tr("Exported to %1").arg(QDir::toNativeSeparators(path)) : tr("That couldn't be saved."));
}

void ReaderTab::askForNoteAndAnnotate()
{
    bool ok = false;
    const QString note = QInputDialog::getMultiLineText(this, tr("Add a note"), tr("Note:"), QString(), &ok);
    if (ok)
        annotateSelection(AnnotationType::Note, note.trimmed());
}

void ReaderTab::askToEditNote(qint64 id)
{
    const std::optional<Annotation> annotation = AnnotationRepository(m_session.database()).get(id);
    if (!annotation)
        return;
    bool ok = false;
    const QString note = QInputDialog::getMultiLineText(this, tr("Edit note"), tr("Note:"), annotation->note, &ok);
    if (ok)
        setAnnotationNote(id, note.trimmed());
}

void ReaderTab::addAnnotationActions(QMenu *menu, const QPoint &viewportPoint)
{
    menu->addSeparator();
    if (m_view->hasSelection()) {
        QMenu *highlight = menu->addMenu(tr("Highlight"));
        const QStringList names = colourNames();
        for (int i = 0; i < annotationColours().size(); ++i) {
            const QString colour = annotationColours().at(i);
            highlight->addAction(colourSwatch(colour), names.at(i), this,
                                 [this, colour] { annotateSelection(AnnotationType::Highlight, QString(), colour); });
        }
        menu->addAction(tr("Underline"), this, [this] { annotateSelection(AnnotationType::Underline); });
        menu->addAction(tr("Add a note..."), this, &ReaderTab::askForNoteAndAnnotate);
    }
    if (const std::optional<qint64> id = m_view->annotationAt(viewportPoint)) {
        const qint64 annotation = *id;
        menu->addSeparator();
        menu->addAction(tr("Edit note..."), this, [this, annotation] { askToEditNote(annotation); });
        addColourMenu(menu, annotation);
        menu->addAction(tr("Delete highlight"), this, [this, annotation] { deleteAnnotation(annotation); });
    }
}

QStringList ReaderTab::colourNames()
{
    return {tr("Yellow"), tr("Green"), tr("Blue"), tr("Pink")};
}

QMenu *ReaderTab::addColourMenu(QMenu *menu, qint64 id)
{
    return addColourMenu(menu, QList<qint64>{id});
}

QMenu *ReaderTab::addColourMenu(QMenu *menu, const QList<qint64> &ids)
{
    // Ticked only if every one chosen has it.
    QString current;
    bool same = true;
    for (const QList<Annotation> *list : {&m_annotations, &m_annotationsElsewhere}) {
        for (const Annotation &annotation : *list) {
            if (!ids.contains(annotation.id))
                continue;
            if (current.isEmpty())
                current = annotation.colour;
            else if (annotation.colour.compare(current, Qt::CaseInsensitive) != 0)
                same = false;
        }
    }
    if (!same)
        current.clear();

    QMenu *colours = menu->addMenu(tr("Colour"));
    auto *group = new QActionGroup(colours);
    group->setExclusive(true);
    const QStringList names = colourNames();
    for (int i = 0; i < annotationColours().size(); ++i) {
        const QString colour = annotationColours().at(i);
        QAction *action = colours->addAction(colourSwatch(colour), names.at(i), this,
                                             [this, ids, colour] { setAnnotationsColour(ids, colour); });
        action->setCheckable(true);
        action->setChecked(colour.compare(current, Qt::CaseInsensitive) == 0);
        group->addAction(action);
    }
    return colours;
}

void ReaderTab::showMatch(int page, const QString &text)
{
    m_pendingMatch = std::make_pair(page, text);
    if (m_ready)
        applyPendingMatch();
}

void ReaderTab::showAnnotation(qint64 id)
{
    if (m_ready)
        goToAnnotation(id);
    else
        m_pendingAnnotation = id;
}

void ReaderTab::applyPendingMatch()
{
    if (!m_pendingMatch)
        return;
    const auto [page, text] = *m_pendingMatch;
    m_pendingMatch.reset();
    if (!m_reflowable)
        ensurePageShown(page);
    m_view->goToPage(m_reflowable ? 0 : page);
    m_search->setText(text);
    m_activeSearch.clear(); // So that it searches again from here, even for the same words.
    applySearch();
}

void ReaderTab::applySearch()
{
    m_searchTimer->stop();
    const QString text = m_search->text().trimmed();
    if (text == m_activeSearch)
        return;
    m_activeSearch = text;
    m_hits.clear();
    m_currentHit = -1;
    m_view->clearSearch();
    m_client->cancelSearch();
    if (text.isEmpty()) {
        m_searchRunning = false;
        updateSearchStatus();
        return;
    }
    m_searchRunning = true;
    m_searchStartPage = m_view->currentPage();
    m_client->startSearch(text, m_searchStartPage);
    updateSearchStatus();
}

void ReaderTab::onSearchHits(int page, const QList<QRectF> &rects)
{
    if (!pageIsShown(page))
        return;
    m_view->setSearchHits(page, rects);

    // Kept in reading order from the start of the document, though they arrive from where the
    // search began. The first match found is the nearest one ahead, so that's the one shown.
    Hit currentHit;
    if (m_currentHit >= 0)
        currentHit = m_hits.at(m_currentHit);
    const bool first = m_hits.isEmpty();
    const auto position = std::lower_bound(m_hits.begin(), m_hits.end(), page,
                                           [](const Hit &hit, int value) { return hit.page < value; });
    qsizetype index = position - m_hits.begin();
    for (const QRectF &rect : rects)
        m_hits.insert(index++, Hit{page, rect});

    if (first) {
        m_currentHit = int(m_hits.size() ? 0 : -1);
        showCurrentHit();
    } else if (m_currentHit >= 0) {
        for (qsizetype i = 0; i < m_hits.size(); ++i) {
            if (m_hits.at(i).page == currentHit.page && m_hits.at(i).rect == currentHit.rect) {
                m_currentHit = int(i);
                break;
            }
        }
    }
    updateSearchStatus();
}

void ReaderTab::showCurrentHit()
{
    if (m_currentHit >= 0 && m_currentHit < m_hits.size())
        m_view->setCurrentHit(m_hits.at(m_currentHit).page, m_hits.at(m_currentHit).rect);
    updateSearchStatus();
}

void ReaderTab::nextHit()
{
    if (m_hits.isEmpty())
        return;
    m_currentHit = (m_currentHit + 1) % int(m_hits.size());
    showCurrentHit();
}

void ReaderTab::previousHit()
{
    if (m_hits.isEmpty())
        return;
    m_currentHit = (m_currentHit - 1 + int(m_hits.size())) % int(m_hits.size());
    showCurrentHit();
}

void ReaderTab::updateSearchStatus()
{
    if (m_activeSearch.isEmpty()) {
        m_searchStatus->clear();
    } else if (m_hits.isEmpty()) {
        m_searchStatus->setText(m_searchRunning ? tr("Searching...") : tr("No matches"));
    } else {
        m_searchStatus->setText(m_searchRunning ? tr("%1 of %2 so far").arg(m_currentHit + 1).arg(m_hits.size())
                                                : tr("%1 of %2").arg(m_currentHit + 1).arg(m_hits.size()));
    }
    m_searchPrevious->setEnabled(!m_hits.isEmpty());
    m_searchNext->setEnabled(!m_hits.isEmpty());
}

} // namespace papertrail::ui
