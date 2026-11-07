#include "ui/ReaderTab.h"

#include "core/repo/ReadingStateRepository.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"

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
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace papertrail::ui {

using namespace core;

namespace {

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
        m_view->goToPage(m_pageBox->text().toInt() - 1);
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
    m_contents->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_contents->header()->setStretchLastSection(false);
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
    updatePageBox();
    updateZoomBox();
    m_client->requestOutline();
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
    if (!m_pageBox->hasFocus())
        m_pageBox->setText(QString::number(m_view->currentPage() + 1));
    m_pageTotal->setText(tr("of %1").arg(m_view->pageCount()));
    if (!m_pageBox->validator() || static_cast<const QIntValidator *>(m_pageBox->validator())->top() != m_view->pageCount())
        m_pageBox->setValidator(new QIntValidator(1, qMax(1, m_view->pageCount()), m_pageBox));
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
    m_contents->clear();
    QList<QTreeWidgetItem *> parents;
    for (const ReaderOutlineItem &item : items) {
        const int depth = qMin(item.depth, int(parents.size()));
        parents.resize(depth);
        QTreeWidgetItem *entry = parents.isEmpty() ? new QTreeWidgetItem(m_contents) : new QTreeWidgetItem(parents.last());
        // Tree items show text as it is, so a title from the file can't turn into formatting.
        entry->setText(0, item.title);
        entry->setToolTip(0, item.title);
        entry->setData(0, kPageRole, item.page);
        entry->setData(0, kPointRole, item.y);
        entry->setData(0, kNoPageRole, item.page < 0);
        parents << entry;
    }
    m_contents->expandToDepth(0);

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
