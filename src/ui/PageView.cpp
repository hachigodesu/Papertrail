#include "ui/PageView.h"

#include "ui/Labels.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace papertrail::ui {

namespace {

constexpr double kGap = 12;
constexpr double kMargin = 12;
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 8.0;
constexpr double kMaxPixelsAcross = 8000; // More than any screen needs.
constexpr qint64 kMaxImageBytes = 400 * 1024 * 1024;
constexpr int kPagesBefore = 1;
constexpr int kPagesAfter = 2;
constexpr int kMaxSelectionPages = 30;
constexpr double kSmallestArea = 6; // In points each way, so a slip of the mouse isn't taken for one.

const QColor kSearchColour(255, 200, 0, 110);
const QColor kCurrentHitColour(255, 120, 0, 150);

} // namespace

PageView::PageView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    m_imageBudget = kMaxImageBytes;
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    verticalScrollBar()->setSingleStep(40);
    horizontalScrollBar()->setSingleStep(40);
    setAccessibleName(tr("Document pages"));

    m_wantedTimer.setSingleShot(true);
    m_wantedTimer.setInterval(15);
    connect(&m_wantedTimer, &QTimer::timeout, this, &PageView::sendWanted);
}

void PageView::setPageCount(int count, const QSizeF &assumedSize)
{
    m_sizes.fill(assumedSize, count);
    m_typicalPageSize = assumedSize;
    m_shownRow = 0;
    relayout();
    scheduleWanted();
}

void PageView::setPageSizes(int start, const QList<QSizeF> &sizes)
{
    // A page that couldn't be measured keeps its assumed size.
    bool changed = false;
    for (qsizetype i = 0; i < sizes.size(); ++i) {
        const int page = start + int(i);
        if (page < m_sizes.size() && !sizes.at(i).isEmpty() && m_sizes.at(page) != sizes.at(i)) {
            m_sizes[page] = sizes.at(i);
            changed = true;
        }
    }
    if (changed) {
        updateTypicalPageSize();
        relayoutKeepingPlace();
        scheduleWanted();
    }
}

void PageView::updateTypicalPageSize()
{
    if (m_sizes.isEmpty())
        return;
    QVector<double> widths;
    QVector<double> heights;
    widths.reserve(m_sizes.size());
    heights.reserve(m_sizes.size());
    for (int page = 0; page < m_sizes.size(); ++page) {
        const QSizeF size = shownSize(page);
        widths << size.width();
        heights << size.height();
    }
    const auto middle = widths.size() / 2;
    std::nth_element(widths.begin(), widths.begin() + middle, widths.end());
    std::nth_element(heights.begin(), heights.begin() + middle, heights.end());
    m_typicalPageSize = QSizeF(widths.at(middle), heights.at(middle));
}

void PageView::forgetAllPages()
{
    m_images.clear();
    m_imageBytes = 0;
    m_failedPages.clear();
    m_links.clear();
    m_linksAsked.clear();
    m_imageAreas.clear();
    m_imageAreasAsked.clear();
    m_searchHits.clear();
    m_currentHitPage = -1;
    m_selected.clear();
    m_selectionText.clear();
    m_history.clear();
    m_forward.clear();
    emit historyChanged();
    emit selectionChanged();
}

int PageView::pagesPerRow() const
{
    return m_layoutMode == LayoutMode::TwoPages ? 2 : 1;
}

int PageView::firstShownPage() const
{
    if (m_rangeFirst < 0 || m_sizes.isEmpty())
        return 0;
    return qMin(m_rangeFirst, int(m_sizes.size()) - 1);
}

int PageView::lastShownPage() const
{
    return shownEnd() - 1;
}

int PageView::shownEnd() const
{
    if (m_rangeFirst < 0)
        return int(m_sizes.size());
    return qMax(firstShownPage() + 1, qMin(m_rangeLast + 1, int(m_sizes.size())));
}

int PageView::rowStart(int row) const
{
    return firstShownPage() + row * pagesPerRow();
}

void PageView::setPageRange(int first, int last)
{
    const int page = currentPage();
    if (first < 0) {
        m_rangeFirst = m_rangeLast = -1;
    } else {
        m_rangeFirst = first;
        m_rangeLast = qMax(first, last);
    }
    m_shownRow = 0;
    relayout();
    goToPage(qBound(firstShownPage(), page, lastShownPage()));
    scheduleWanted();
}

void PageView::updateFitScale()
{
    if (m_zoomMode == ZoomMode::Fixed || m_sizes.isEmpty())
        return;

    const int perRow = pagesPerRow();
    double width = 0;
    double height = 0;
    if (m_layoutMode == LayoutMode::Continuous) {
        // A typical page sets the size, so that one wide table or fold-out doesn't shrink the rest.
        width = m_typicalPageSize.width();
        height = m_typicalPageSize.height();
    } else {
        for (int i = rowStart(m_shownRow); i < rowStart(m_shownRow) + perRow && i < shownEnd(); ++i) {
            width += shownSize(i).width();
            height = qMax(height, shownSize(i).height());
        }
    }
    if (width <= 0 || height <= 0)
        return;

    const double barWidth = verticalScrollBar()->sizeHint().width();
    const double availableWidth = viewport()->width() + (verticalScrollBar()->isVisible() ? 0 : -barWidth) - 2 * kMargin
        - (perRow - 1) * kGap;
    const double availableHeight = viewport()->height() - 2 * kMargin;
    double fit = availableWidth / width;
    if (m_zoomMode == ZoomMode::FitPage)
        fit = qMin(fit, availableHeight / height);
    m_scale = qBound(kMinScale, fit, kMaxScale);
}

void PageView::relayout()
{
    updateFitScale();

    const int perRow = pagesPerRow();
    const int pages = shownEnd();
    const int rows = m_sizes.isEmpty() ? 0 : (pages - firstShownPage() + perRow - 1) / perRow;
    const bool oneRowAtATime = m_layoutMode != LayoutMode::Continuous;
    m_shownRow = qBound(0, m_shownRow, qMax(0, rows - 1));

    m_pageRects.fill(QRectF(), int(m_sizes.size()));
    m_rowTops.resize(rows);
    m_rowHeights.resize(rows);

    for (int row = 0; row < rows; ++row) {
        double height = 0;
        for (int i = rowStart(row); i < rowStart(row) + perRow && i < pages; ++i)
            height = qMax(height, shownSize(i).height() * m_scale);
        m_rowHeights[row] = height;
    }

    double y = kMargin;
    for (int row = 0; row < rows; ++row) {
        if (oneRowAtATime && row != m_shownRow) {
            m_rowTops[row] = kMargin;
            continue;
        }
        m_rowTops[row] = y;
        double width = -kGap;
        for (int i = rowStart(row); i < rowStart(row) + perRow && i < pages; ++i)
            width += shownSize(i).width() * m_scale + kGap;
        // Centred if it fits, and otherwise from the left, with the scroll bar to reach the rest.
        double x = width + 2 * kMargin <= viewport()->width() ? (viewport()->width() - width) / 2 : kMargin;
        for (int i = rowStart(row); i < rowStart(row) + perRow && i < pages; ++i) {
            const QSizeF size = shownSize(i) * m_scale;
            m_pageRects[i] = QRectF(QPointF(std::round(x), std::round(y)), QSizeF(std::round(size.width()), std::round(size.height())));
            x += size.width() + kGap;
        }
        y += m_rowHeights.at(row) + kGap;
    }
    m_contentHeight = rows > 0 ? y - kGap + kMargin : 0;

    updateScrollBars();
    viewport()->update();
}

QSizeF PageView::shownSize(int page) const
{
    const QSizeF size = m_sizes.value(page);
    return m_rotation % 180 == 0 ? size : size.transposed();
}

// The page's own points, scaled to pixels, are turned about its top left corner and moved back into its rectangle.
QTransform PageView::pageToContent(int page) const
{
    const QRectF shown = m_pageRects.value(page);
    const double width = m_rotation % 180 == 0 ? shown.width() : shown.height(); // Unturned, in pixels.
    const double height = m_rotation % 180 == 0 ? shown.height() : shown.width();
    QTransform turn;
    switch (m_rotation) {
    case 90: turn.translate(shown.x() + height, shown.y()); break;
    case 180: turn.translate(shown.x() + width, shown.y() + height); break;
    case 270: turn.translate(shown.x(), shown.y() + width); break;
    default: turn.translate(shown.x(), shown.y()); break;
    }
    turn.rotate(m_rotation);
    return QTransform::fromScale(m_scale, m_scale) * turn;
}

QPointF PageView::contentToPage(int page, const QPointF &content) const
{
    return pageToContent(page).inverted().map(content);
}

void PageView::setRotation(int degrees)
{
    const int rotation = ((degrees % 360) + 360) % 360 / 90 * 90;
    if (rotation == m_rotation)
        return;
    const int page = topPage();
    m_rotation = rotation;
    updateTypicalPageSize();
    relayout();
    goToPage(page);
    scheduleWanted();
}

void PageView::relayoutKeepingPlace()
{
    const int page = topPage();
    const double offset = topOffset();
    relayout();
    goToPage(page, offset);
}

void PageView::updateScrollBars()
{
    m_settingScroll = true;
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(m_contentHeight)) - viewport()->height()));
    updateHorizontalRange();
    m_settingScroll = false;
}

// Only the rows on show count, so the scroll bar is only there while there's more of them to reach.
void PageView::updateHorizontalRange()
{
    double widest = 0;
    if (!m_pageRects.isEmpty()) {
        const int perRow = pagesPerRow();
        const double bottom = contentY() + viewport()->height();
        const int rows = int(m_rowTops.size());
        for (int row = rowForY(contentY()); row < rows; ++row) {
            if (m_layoutMode == LayoutMode::Continuous && m_rowTops.at(row) > bottom)
                break;
            if (m_layoutMode != LayoutMode::Continuous && row != m_shownRow)
                break;
            for (int i = rowStart(row); i < rowStart(row) + perRow && i < shownEnd(); ++i)
                widest = qMax(widest, m_pageRects.at(i).right());
        }
    }
    m_contentWidth = qMax(double(viewport()->width()), widest + kMargin);
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setRange(0, qMax(0, int(std::ceil(m_contentWidth)) - viewport()->width()));
}

double PageView::contentX() const
{
    return horizontalScrollBar()->value();
}

double PageView::contentY() const
{
    return verticalScrollBar()->value();
}

int PageView::rowForY(double y) const
{
    if (m_layoutMode != LayoutMode::Continuous)
        return m_shownRow;
    const auto it = std::upper_bound(m_rowTops.cbegin(), m_rowTops.cend(), y);
    return qMax(0, int(it - m_rowTops.cbegin()) - 1);
}

int PageView::rowOfPage(int page) const
{
    return (page - firstShownPage()) / pagesPerRow();
}

int PageView::currentPage() const
{
    if (m_sizes.isEmpty())
        return 0;
    const int row = rowForY(contentY() + viewport()->height() / 2.0);
    return qMin(rowStart(row), lastShownPage());
}

int PageView::topPage() const
{
    if (m_sizes.isEmpty())
        return 0;
    const int row = rowForY(contentY() + kMargin + 1);
    return qMin(rowStart(row), lastShownPage());
}

double PageView::topOffset() const
{
    if (m_rowHeights.isEmpty())
        return 0;
    const int row = rowForY(contentY() + kMargin + 1);
    const double height = m_rowHeights.value(row);
    return height > 0 ? qBound(0.0, (contentY() - m_rowTops.value(row)) / height, 1.0) : 0;
}

void PageView::goToPage(int page, double offset)
{
    if (m_sizes.isEmpty())
        return;
    const int target = qBound(firstShownPage(), page, lastShownPage());
    const int row = rowOfPage(target);
    if (m_layoutMode != LayoutMode::Continuous && row != m_shownRow) {
        m_shownRow = row;
        relayout();
    }
    verticalScrollBar()->setValue(int(m_rowTops.value(row) + offset * m_rowHeights.value(row) - (offset == 0 ? kMargin : 0)));
    noteCurrentPage();
    scheduleWanted();
    viewport()->update();
}

void PageView::goToPoint(int page, double y)
{
    if (m_sizes.isEmpty())
        return;
    m_history << Place{topPage(), topOffset()};
    m_forward.clear();
    emit historyChanged();
    const int target = qBound(0, page, int(m_sizes.size()) - 1);
    const QRectF shown = m_pageRects.value(target);
    if (shown.height() <= 0) {
        goToPage(page, 0);
        return;
    }
    // On a page turned on its side, a height on the page runs across the screen, so it goes to the page's top.
    const double turnedY = m_rotation % 180 == 0 ? pageToContent(target).map(QPointF(0, y)).y() - shown.y() : 0;
    goToPage(page, qBound(0.0, turnedY / shown.height(), 1.0));
}

void PageView::goBack()
{
    if (m_history.isEmpty())
        return;
    const Place place = m_history.takeLast();
    m_forward << Place{topPage(), topOffset()};
    emit historyChanged();
    goToPage(place.page, place.offset);
}

void PageView::goForward()
{
    if (m_forward.isEmpty())
        return;
    const Place place = m_forward.takeLast();
    m_history << Place{topPage(), topOffset()};
    emit historyChanged();
    goToPage(place.page, place.offset);
}

void PageView::showRow(int row, bool atEnd)
{
    const int rows = int(m_rowTops.size());
    if (row < 0 || row >= rows)
        return;
    m_shownRow = row;
    relayout();
    verticalScrollBar()->setValue(atEnd ? verticalScrollBar()->maximum() : 0);
    noteCurrentPage();
    scheduleWanted();
}

void PageView::nextPage()
{
    if (m_layoutMode == LayoutMode::Continuous)
        goToPage(rowStart(rowOfPage(currentPage()) + 1));
    else
        showRow(m_shownRow + 1);
}

void PageView::previousPage()
{
    if (m_layoutMode == LayoutMode::Continuous)
        goToPage(rowStart(rowOfPage(currentPage()) - 1));
    else
        showRow(m_shownRow - 1);
}

void PageView::setLayoutMode(LayoutMode mode)
{
    if (mode == m_layoutMode)
        return;
    const int page = topPage();
    const double offset = topOffset();
    m_layoutMode = mode;
    m_shownRow = rowOfPage(page);
    relayout();
    goToPage(page, offset);
    scheduleWanted();
}

void PageView::setColours(Colours colours)
{
    m_colours = colours;
    if (m_colours == Colours::Dark)
        scheduleWanted(); // For where the pictures are.
    viewport()->update();
}

void PageView::setZoomMode(ZoomMode mode)
{
    if (mode == m_zoomMode)
        return;
    m_zoomMode = mode;
    relayoutKeepingPlace();
    scheduleWanted();
    emit zoomChanged();
}

void PageView::setScale(double scale)
{
    const double bounded = qBound(kMinScale, scale, kMaxScale);
    if (m_zoomMode == ZoomMode::Fixed && qFuzzyCompare(bounded, m_scale))
        return;
    m_zoomMode = ZoomMode::Fixed;
    m_scale = bounded;
    relayoutKeepingPlace();
    scheduleWanted();
    emit zoomChanged();
}

void PageView::zoomBy(double factor)
{
    setScale(m_scale * factor);
}

double PageView::wantedScale() const
{
    double scale = m_scale * devicePixelRatioF();
    // A sensible limit, even for a page claiming to be enormous.
    double largest = 0;
    const int page = currentPage();
    if (page < m_sizes.size())
        largest = qMax(m_sizes.at(page).width(), m_sizes.at(page).height());
    if (largest > 0)
        scale = qMin(scale, kMaxPixelsAcross / largest);
    // Rounded, so that small changes in a fitted window don't each mean rendering again.
    return std::round(scale * 1000) / 1000;
}

void PageView::scheduleWanted()
{
    if (!m_wantedTimer.isActive())
        m_wantedTimer.start();
}

void PageView::sendWanted()
{
    if (m_sizes.isEmpty())
        return;

    const int perRow = pagesPerRow();
    const int pages = shownEnd();
    const double top = contentY();
    const double bottom = top + viewport()->height();
    const int firstRow = rowForY(top);
    int lastRow = firstRow;
    if (m_layoutMode == LayoutMode::Continuous) {
        while (lastRow + 1 < m_rowTops.size() && m_rowTops.at(lastRow + 1) < bottom)
            ++lastRow;
    }

    QList<int> order;
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int i = rowStart(row); i < rowStart(row) + perRow && i < pages; ++i)
            order << i;
    }
    for (const int page : QList<int>(order)) {
        if (!m_linksAsked.contains(page)) {
            m_linksAsked.insert(page);
            emit linksWanted(page);
        }
        // Only dark pages need to know, and finding them means reading the page's contents.
        if (m_colours == Colours::Dark && !m_imageAreasAsked.contains(page)) {
            m_imageAreasAsked.insert(page);
            emit imageAreasWanted(page);
        }
    }
    // Pages either side, so that turning a page is immediate.
    if (!order.isEmpty()) {
        const int first = order.first();
        const int last = order.last();
        for (int i = 1; i <= kPagesAfter * perRow && last + i < pages; ++i)
            order << last + i;
        for (int i = 1; i <= kPagesBefore * perRow && first - i >= firstShownPage(); ++i)
            order << first - i;
    }

    const double scale = wantedScale();
    QList<RenderRequest> wanted;
    for (const int page : order) {
        if (m_failedPages.contains(page))
            continue;
        const auto cached = m_images.constFind(page);
        if (cached == m_images.constEnd() || !qFuzzyCompare(cached->scale, scale))
            wanted << RenderRequest{page, scale};
    }
    emit pagesWanted(wanted);
}

void PageView::setPageImage(int page, double scale, const QImage &image)
{
    auto existing = m_images.find(page);
    if (existing != m_images.end()) {
        m_imageBytes -= existing->image.sizeInBytes();
        m_images.erase(existing);
    }
    m_images.insert(page, CachedImage{image, scale, ++m_useClock});
    m_imageBytes += image.sizeInBytes();
    trimCache();

    const QRectF rect = m_pageRects.value(page);
    if (!rect.isNull())
        viewport()->update(rect.translated(-contentX(), -contentY()).toAlignedRect());
}

void PageView::trimCache()
{
    if (m_imageBytes <= m_imageBudget)
        return;
    // Not whatever's on show, however little room there is.
    const QRectF visible(contentX(), contentY(), viewport()->width(), viewport()->height());
    QList<int> pages = m_images.keys();
    std::sort(pages.begin(), pages.end(),
              [this](int a, int b) { return m_images.value(a).lastUsed < m_images.value(b).lastUsed; });
    for (const int page : pages) {
        if (m_imageBytes <= m_imageBudget)
            break;
        if (m_pageRects.value(page).intersects(visible))
            continue;
        m_imageBytes -= m_images.value(page).image.sizeInBytes();
        m_images.remove(page);
    }
}

void PageView::setPageFailed(int page)
{
    m_failedPages.insert(page);
    viewport()->update();
    scheduleWanted();
}

void PageView::setLinks(int page, const QList<ReaderLink> &links)
{
    m_links.insert(page, links);
}

void PageView::setImageAreas(int page, const QList<QRectF> &areas)
{
    // A picture covering most of the page is most likely a scan of it, whose text should go dark like any other.
    const QSizeF size = m_sizes.value(page, m_typicalPageSize);
    const double pageArea = size.width() * size.height();
    QList<QRectF> pictures;
    for (const QRectF &area : areas) {
        const QRectF onPage = area.intersected(QRectF(QPointF(0, 0), size));
        if (!onPage.isEmpty() && (pageArea <= 0 || onPage.width() * onPage.height() < 0.6 * pageArea))
            pictures << onPage;
    }
    m_imageAreas.insert(page, pictures);
    if (m_colours == Colours::Dark && !pictures.isEmpty())
        viewport()->update();
}

void PageView::setSearchHits(int page, const QList<QRectF> &rects)
{
    m_searchHits.insert(page, rects);
    viewport()->update();
}

void PageView::clearSearch()
{
    m_searchHits.clear();
    m_currentHitPage = -1;
    viewport()->update();
}

void PageView::setCurrentHit(int page, const QRectF &rect)
{
    m_currentHitPage = page;
    m_currentHitRect = rect;

    // Only moves if the match isn't already on screen.
    const QRectF pageArea = m_pageRects.value(page);
    const QRectF visible(contentX(), contentY(), viewport()->width(), viewport()->height());
    const QRectF hit = pageToContent(page).mapRect(rect);
    if (pageArea.isNull() || !visible.contains(hit)) {
        const double height = pageArea.height();
        goToPage(page, height > 0 ? qMax(0.0, (hit.y() - pageArea.y() - viewport()->height() / 3.0) / height) : 0);
    }
    viewport()->update();
}

void PageView::setRuler(int page, const QRectF &rect)
{
    m_rulerPage = page;
    m_rulerRect = rect;
    viewport()->update();
}

void PageView::clearRuler()
{
    m_rulerPage = -1;
    m_rulerRect = QRectF();
    viewport()->update();
}

void PageView::scrollToShow(int page, const QRectF &rect)
{
    const QRectF pageArea = m_pageRects.value(page);
    if (pageArea.isNull()) {
        // Not laid out near here yet: goes to it first, then it's on screen.
        goToPoint(page, qMax(0.0, rect.top() - 40));
        return;
    }
    const QRectF shown = pageToContent(page).mapRect(rect);
    const double margin = qMin(80.0, viewport()->height() / 6.0);
    const double top = contentY();
    const double bottom = top + viewport()->height();
    QScrollBar *bar = verticalScrollBar();
    if (shown.top() < top + margin)
        bar->setValue(bar->value() - int(top + margin - shown.top()));
    else if (shown.bottom() > bottom - margin)
        bar->setValue(bar->value() + int(shown.bottom() - (bottom - margin)));
}

bool PageView::isShowing(int page, const QRectF &rect) const
{
    const QRectF pageArea = m_pageRects.value(page);
    if (pageArea.isNull())
        return false;
    const QRectF visible(contentX(), contentY(), viewport()->width(), viewport()->height());
    return visible.intersects(pageToContent(page).mapRect(rect));
}

void PageView::setSpoken(int page, const QList<QRectF> &rects)
{
    m_spokenPage = page;
    m_spokenRects = rects;
    viewport()->update();
}

void PageView::clearSpoken()
{
    m_spokenPage = -1;
    m_spokenRects.clear();
    viewport()->update();
}

std::optional<std::pair<int, QRectF>> PageView::selectionStart() const
{
    std::optional<std::pair<int, QRectF>> first;
    for (auto it = m_selected.cbegin(); it != m_selected.cend(); ++it) {
        if (it->rects.isEmpty() || (first && first->first <= it.key()))
            continue;
        QRectF top = it->rects.first();
        for (const QRectF &rect : it->rects) {
            if (rect.top() < top.top() - 1 || (qAbs(rect.top() - top.top()) <= 1 && rect.left() < top.left()))
                top = rect;
        }
        first = std::make_pair(it.key(), top);
    }
    return first;
}

void PageView::setSelection(int page, const QList<QRectF> &rects, const QString &text)
{
    m_selected.insert(page, Selected{rects, text});
    QList<int> pages = m_selected.keys();
    std::sort(pages.begin(), pages.end());
    QStringList parts;
    for (const int key : pages) {
        if (!m_selected.value(key).text.isEmpty())
            parts << m_selected.value(key).text;
    }
    m_selectionText = parts.join(u'\n');
    viewport()->update();
    emit selectionChanged();
}

QList<SelectionPart> PageView::selectionParts() const
{
    QList<int> pages = m_selected.keys();
    std::sort(pages.begin(), pages.end());
    QList<SelectionPart> parts;
    for (const int page : pages) {
        const Selected &selected = m_selected[page];
        if (!selected.text.isEmpty())
            parts << SelectionPart{page, selected.rects, selected.text};
    }
    return parts;
}

void PageView::setMarks(int page, const QList<AnnotationMark> &marks)
{
    if (marks.isEmpty())
        m_marks.remove(page);
    else
        m_marks.insert(page, marks);
    viewport()->update();
}

void PageView::clearMarks()
{
    m_marks.clear();
    viewport()->update();
}

QList<int> PageView::pagesWithMarks() const
{
    QList<int> pages = m_marks.keys();
    std::sort(pages.begin(), pages.end());
    return pages;
}

std::optional<qint64> PageView::annotationAt(const QPoint &viewportPoint) const
{
    for (auto it = m_marks.cbegin(); it != m_marks.cend(); ++it) {
        for (const AnnotationMark &mark : it.value()) {
            for (const QRectF &rect : mark.rects) {
                const QPoint first = viewportPointFor(it.key(), rect.topLeft());
                const QPoint last = viewportPointFor(it.key(), rect.bottomRight());
                if (QRect(first, last).normalized().contains(viewportPoint))
                    return mark.id;
            }
        }
    }
    return std::nullopt;
}

void PageView::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    m_selectionText.clear();
    viewport()->update();
    emit selectionChanged();
}

QString PageView::selectedText() const
{
    return m_selectionText;
}

QRectF PageView::pageRect(int page) const
{
    return m_pageRects.value(page);
}

QPoint PageView::viewportPointFor(int page, const QPointF &point) const
{
    const QPointF content = pageToContent(page).map(point);
    return QPoint(int(content.x() - contentX()), int(content.y() - contentY()));
}

std::optional<std::pair<int, QPointF>> PageView::pageAt(const QPoint &viewportPoint, bool nearest) const
{
    if (m_sizes.isEmpty())
        return std::nullopt;
    const QPointF content(viewportPoint.x() + contentX(), viewportPoint.y() + contentY());
    const int row = rowForY(content.y());
    const int perRow = pagesPerRow();
    for (int i = rowStart(row); i < rowStart(row) + perRow && i < shownEnd(); ++i) {
        const QRectF area = m_pageRects.at(i);
        if (area.contains(content))
            return std::make_pair(i, contentToPage(i, content));
    }
    if (!nearest)
        return std::nullopt;

    int best = qMin(rowStart(row), lastShownPage());
    if (perRow == 2 && best + 1 < shownEnd() && content.x() > m_pageRects.at(best).right())
        best += 1;
    const QRectF area = m_pageRects.at(best);
    if (area.isNull())
        return std::nullopt;
    const QPointF clamped(qBound(area.left(), content.x(), area.right()), qBound(area.top(), content.y(), area.bottom()));
    return std::make_pair(best, contentToPage(best, clamped));
}

std::optional<ReaderLink> PageView::linkAtPoint(const QPoint &viewportPoint) const
{
    if (const ReaderLink *link = linkAt(viewportPoint))
        return *link;
    return std::nullopt;
}

const ReaderLink *PageView::linkAt(const QPoint &viewportPoint) const
{
    const auto place = pageAt(viewportPoint);
    if (!place)
        return nullptr;
    const auto links = m_links.constFind(place->first);
    if (links == m_links.constEnd())
        return nullptr;
    for (const ReaderLink &link : *links) {
        if (link.rect.contains(place->second))
            return &link;
    }
    return nullptr;
}

void PageView::noteCurrentPage()
{
    const int page = currentPage();
    if (page != m_lastCurrentPage) {
        m_lastCurrentPage = page;
        emit currentPageChanged(page);
    }
    emit placeChanged();
}

void PageView::scrollContentsBy(int, int)
{
    if (m_settingScroll)
        return;
    m_settingScroll = true;
    updateHorizontalRange();
    m_settingScroll = false;
    viewport()->update();
    noteCurrentPage();
    scheduleWanted();
}

void PageView::resizeEvent(QResizeEvent *)
{
    relayoutKeepingPlace();
    scheduleWanted();
}

void PageView::drawPage(QPainter &painter, int page, const QRectF &shownTarget)
{
    // Drawn as if unturned, at the origin, with the painter doing the turning.
    painter.save();
    QTransform turn = QTransform::fromTranslate(-m_pageRects.value(page).x(), -m_pageRects.value(page).y())
        * QTransform::fromScale(1 / m_scale, 1 / m_scale) * pageToContent(page);
    turn *= QTransform::fromTranslate(shownTarget.x() - m_pageRects.value(page).x(), shownTarget.y() - m_pageRects.value(page).y());
    painter.setTransform(turn, true);
    const QSizeF unturned = m_rotation % 180 == 0 ? shownTarget.size() : shownTarget.size().transposed();
    const QRectF target(QPointF(m_pageRects.value(page).x(), m_pageRects.value(page).y()), unturned);
    drawUnturnedPage(painter, page, target);
    painter.restore();
}

void PageView::drawUnturnedPage(QPainter &painter, int page, const QRectF &target)
{
    const auto cached = m_images.find(page);
    if (cached != m_images.end()) {
        cached->lastUsed = ++m_useClock;
        // Slightly blurred until the new size arrives, which beats a blank page while zooming.
        const bool exact = cached->image.width() == qRound(target.width() * devicePixelRatioF());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, !exact);
        painter.drawImage(target, cached->image);
    } else {
        painter.fillRect(target, Qt::white);
        if (m_failedPages.contains(page)) {
            painter.setPen(Qt::black);
            painter.drawText(target, Qt::AlignCenter | Qt::TextWordWrap,
                             tr("This page couldn't be shown.\nThe rest of the document isn't affected."));
        } else {
            painter.setPen(QColor(140, 140, 140));
            painter.drawText(target, Qt::AlignCenter, tr("Page %1").arg(page + 1));
        }
    }

    // Colours are applied as it's drawn, so changing them doesn't mean rendering anything again.
    if (m_colours == Colours::Dark) {
        // Everything but the pictures, which would look like negatives.
        const QList<QRectF> pictures = cached != m_images.end() ? m_imageAreas.value(page) : QList<QRectF>();
        if (!pictures.isEmpty()) {
            painter.save();
            QPainterPath outside;
            outside.addRect(target);
            for (const QRectF &picture : pictures) {
                QPainterPath inside;
                inside.addRect(QRectF(target.x() + picture.x() * m_scale, target.y() + picture.y() * m_scale,
                                      picture.width() * m_scale, picture.height() * m_scale));
                outside = outside.subtracted(inside);
            }
            painter.setClipPath(outside, Qt::IntersectClip);
        }
        painter.setCompositionMode(QPainter::CompositionMode_Difference);
        painter.fillRect(target, QColor(232, 232, 232));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (!pictures.isEmpty())
            painter.restore();
    } else if (m_colours == Colours::Sepia) {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.fillRect(target, QColor(244, 233, 208));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    const auto toTarget = [&](const QRectF &rect) {
        return QRectF(target.x() + rect.x() * m_scale, target.y() + rect.y() * m_scale, rect.width() * m_scale,
                      rect.height() * m_scale);
    };
    const auto marks = m_marks.constFind(page);
    if (marks != m_marks.constEnd()) {
        for (const AnnotationMark &mark : *marks) {
            QColor colour = mark.colour;
            if (mark.area) {
                // Outlined with a light wash, since what it's drawn over is the point of it.
                for (const QRectF &rect : mark.rects) {
                    const QRectF area = toTarget(rect);
                    QColor wash = colour;
                    wash.setAlpha(35);
                    painter.fillRect(area, wash);
                    QColor edge = colour;
                    edge.setAlpha(255);
                    painter.setPen(QPen(edge, 2));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(area.adjusted(1, 1, -1, -1));
                }
                if (mark.hasNote && !mark.rects.isEmpty()) {
                    QColor corner = colour;
                    corner.setAlpha(255);
                    const QRectF area = toTarget(mark.rects.first());
                    painter.fillRect(QRectF(area.left(), area.top(), 8, 8), corner);
                }
                continue;
            }
            for (const QRectF &rect : mark.rects) {
                const QRectF area = toTarget(rect);
                if (mark.underline) {
                    colour.setAlpha(255);
                    painter.fillRect(QRectF(area.left(), area.bottom() - 2, area.width(), 2), colour);
                } else {
                    colour.setAlpha(110);
                    painter.fillRect(area, colour);
                }
            }
            // A corner to show there's a note to read.
            if (mark.hasNote && !mark.rects.isEmpty()) {
                QColor corner = mark.colour;
                corner.setAlpha(255);
                const QRectF area = toTarget(mark.rects.first());
                painter.fillRect(QRectF(area.left() - 3, area.top(), 3, area.height()), corner);
            }
        }
    }
    const auto hits = m_searchHits.constFind(page);
    if (hits != m_searchHits.constEnd()) {
        for (const QRectF &rect : *hits)
            painter.fillRect(toTarget(rect), kSearchColour);
    }
    if (page == m_currentHitPage)
        painter.fillRect(toTarget(m_currentHitRect), kCurrentHitColour);

    const auto selected = m_selected.constFind(page);
    if (selected != m_selected.constEnd()) {
        QColor colour = palette().color(QPalette::Highlight);
        colour.setAlpha(110);
        for (const QRectF &rect : selected->rects)
            painter.fillRect(toTarget(rect), colour);
    }

    if (m_drawingArea && page == m_areaPage) {
        const QRectF area = toTarget(drawnArea());
        QColor wash = palette().color(QPalette::Highlight);
        wash.setAlpha(40);
        painter.fillRect(area, wash);
        QPen pen(palette().color(QPalette::Highlight), 1.5, Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(area);
    }

    // The sentence being read aloud, in a colour of its own.
    if (page == m_spokenPage) {
        for (const QRectF &rect : std::as_const(m_spokenRects))
            painter.fillRect(toTarget(rect.adjusted(-1, -1, 1, 1)), QColor(90, 170, 255, 85));
    }

    // The reading ruler dims all but its part of its page, after everything else, so it turns with the page.
    if (m_rulerPage >= 0) {
        QPainterPath dimmed;
        dimmed.addRect(target);
        if (page == m_rulerPage) {
            QPainterPath kept;
            kept.addRoundedRect(toTarget(m_rulerRect.adjusted(-3, -2, 3, 2)), 3, 3);
            dimmed = dimmed.subtracted(kept);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, m_colours == Colours::Dark ? 150 : 110));
        painter.drawPath(dimmed);
    }
}

void PageView::setImageBudget(qint64 bytes)
{
    m_imageBudget = qMax<qint64>(bytes, 32 * 1024 * 1024);
    trimCache();
}

void PageView::startArea()
{
    if (!m_areasAllowed) {
        emit areaRefused();
        return;
    }
    m_areaMode = true;
    clearSelection();
    viewport()->setCursor(Qt::CrossCursor);
}

QPointF PageView::pointOnPage(int page, const QPoint &viewportPoint) const
{
    const QSizeF size = m_sizes.value(page);
    const QPointF content(viewportPoint.x() + contentX(), viewportPoint.y() + contentY());
    const QPointF point = contentToPage(page, content);
    return QPointF(qBound(0.0, point.x(), size.width()), qBound(0.0, point.y(), size.height()));
}

QRectF PageView::drawnArea() const
{
    return QRectF(m_areaStart, m_areaEnd).normalized();
}

void PageView::stopDrawingArea()
{
    m_drawingArea = false;
    m_areaMode = false;
    viewport()->unsetCursor();
    viewport()->update();
}

void PageView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    const QColor background = m_colours == Colours::Light ? palette().color(QPalette::Mid)
        : m_colours == Colours::Sepia                     ? QColor(120, 108, 88)
                                                          : QColor(32, 32, 32);
    painter.fillRect(event->rect(), background);
    if (m_sizes.isEmpty())
        return;

    const double x = contentX();
    const double y = contentY();
    const int perRow = pagesPerRow();
    const int firstRow = rowForY(y);
    const double bottom = y + viewport()->height();
    const int rows = int(m_rowTops.size());
    for (int row = firstRow; row < rows; ++row) {
        if (m_layoutMode == LayoutMode::Continuous && m_rowTops.at(row) > bottom)
            break;
        if (m_layoutMode != LayoutMode::Continuous && row != m_shownRow)
            break;
        for (int i = rowStart(row); i < rowStart(row) + perRow && i < shownEnd(); ++i) {
            const QRectF area = m_pageRects.at(i);
            if (!area.isNull())
                drawPage(painter, i, area.translated(-x, -y));
        }
    }
}

void PageView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double steps = event->angleDelta().y() / 120.0;
        if (steps != 0)
            zoomBy(std::pow(1.15, steps));
        event->accept();
        return;
    }

    // With one row at a time, scrolling past either end turns the page.
    if (m_layoutMode != LayoutMode::Continuous) {
        const int delta = event->angleDelta().y();
        QScrollBar *bar = verticalScrollBar();
        if (delta < 0 && bar->value() >= bar->maximum()) {
            showRow(m_shownRow + 1);
            event->accept();
            return;
        }
        if (delta > 0 && bar->value() <= bar->minimum()) {
            showRow(m_shownRow - 1, true);
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    QScrollBar *bar = verticalScrollBar();
    const bool oneRow = m_layoutMode != LayoutMode::Continuous;
    if (event->matches(QKeySequence::Copy)) {
        if (hasSelection())
            QApplication::clipboard()->setText(selectedText());
        return;
    }
    switch (event->key()) {
    case Qt::Key_PageDown:
    case Qt::Key_Space:
        if (event->key() == Qt::Key_Space && (event->modifiers() & Qt::ShiftModifier)) {
            if (oneRow && bar->value() <= bar->minimum())
                showRow(m_shownRow - 1, true);
            else
                bar->setValue(bar->value() - bar->pageStep() * 9 / 10);
        } else if (oneRow && bar->value() >= bar->maximum()) {
            showRow(m_shownRow + 1);
        } else {
            bar->setValue(bar->value() + bar->pageStep() * 9 / 10);
        }
        return;
    case Qt::Key_PageUp:
        if (oneRow && bar->value() <= bar->minimum())
            showRow(m_shownRow - 1, true);
        else
            bar->setValue(bar->value() - bar->pageStep() * 9 / 10);
        return;
    case Qt::Key_Home:
        goToPage(firstShownPage());
        return;
    case Qt::Key_End:
        goToPage(lastShownPage());
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.15);
        return;
    case Qt::Key_Minus:
        zoomBy(1 / 1.15);
        return;
    case Qt::Key_Left:
        if (event->modifiers() & Qt::AltModifier) {
            goBack();
            return;
        }
        if (bar->maximum() >= 0 && horizontalScrollBar()->maximum() == 0) {
            previousPage();
            return;
        }
        break;
    case Qt::Key_Right:
        if (event->modifiers() & Qt::AltModifier) {
            goForward();
            return;
        }
        if (horizontalScrollBar()->maximum() == 0) {
            nextPage();
            return;
        }
        break;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (m_rulerPage >= 0 && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            emit rulerStepRequested(event->key() == Qt::Key_Down ? 1 : -1);
            return;
        }
        break;
    case Qt::Key_Escape:
        if (m_drawingArea || m_areaMode) {
            stopDrawingArea();
        } else if (hasSelection()) {
            clearSelection();
        } else {
            emit escapeUnused();
            return;
        }
        clearSelection();
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    // A mouse's own back and forward buttons, as in a browser.
    if (event->button() == Qt::BackButton) {
        goBack();
        return;
    }
    if (event->button() == Qt::ForwardButton) {
        goForward();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_pressPosition = event->position().toPoint();
    m_pressedOnLink = linkAt(m_pressPosition) != nullptr;
    clearSelection();
    const auto place = pageAt(m_pressPosition);

    // Ctrl and a click asks what the words there refer to, rather than starting a selection.
    if (place && (event->modifiers() & Qt::ControlModifier) && !m_areaMode) {
        m_dragging = false;
        m_pressedOnLink = false;
        emit referenceWanted(place->first, place->second, m_pressPosition);
        return;
    }

    // Shift and a drag draws an area, as does any drag once one's been asked for.
    if (place && (m_areaMode || (event->modifiers() & Qt::ShiftModifier))) {
        if (!m_areasAllowed) {
            m_areaMode = false;
            m_dragging = false;
            emit areaRefused();
            return;
        }
        m_drawingArea = true;
        m_dragging = false;
        m_pressedOnLink = false;
        m_areaPage = place->first;
        m_areaStart = m_areaEnd = place->second;
        viewport()->setCursor(Qt::CrossCursor);
        return;
    }
    m_dragging = place.has_value();
    if (place) {
        m_dragStartPage = place->first;
        m_dragStartPoint = place->second;
    }
}

void PageView::updateSelectionTo(int page, const QPointF &point)
{
    const int first = qMin(page, m_dragStartPage);
    const int last = qMin(qMax(page, m_dragStartPage), first + kMaxSelectionPages - 1);

    // Anything no longer in the range has dropped out of the selection.
    bool removed = false;
    for (const int key : m_selected.keys()) {
        if (key < first || key > last) {
            m_selected.remove(key);
            removed = true;
        }
    }
    if (removed)
        viewport()->update();

    for (int i = first; i <= last; ++i) {
        const QSizeF size = m_sizes.value(i);
        QPointF a(0, 0);
        QPointF b(size.width(), size.height());
        if (i == m_dragStartPage && i == page) {
            a = m_dragStartPoint;
            b = point;
        } else if (i == first) {
            a = i == m_dragStartPage ? m_dragStartPoint : point;
        } else if (i == last) {
            b = i == page ? point : m_dragStartPoint;
        }
        emit selectionWanted(i, a, b);
    }
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();
    if (m_drawingArea && (event->buttons() & Qt::LeftButton)) {
        m_areaEnd = pointOnPage(m_areaPage, position);
        viewport()->update();
        return;
    }
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        if ((position - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
            if (const auto place = pageAt(position, true))
                updateSelectionTo(place->first, place->second);
        }
        return;
    }

    const ReaderLink *link = linkAt(position);
    const bool overPage = pageAt(position).has_value();
    if (overPage && m_areasAllowed && (m_areaMode || (event->modifiers() & Qt::ShiftModifier)))
        viewport()->setCursor(Qt::CrossCursor);
    else if (overPage && (event->modifiers() & Qt::ControlModifier))
        viewport()->setCursor(Qt::PointingHandCursor);
    else
        viewport()->setCursor(link ? Qt::PointingHandCursor : (overPage ? Qt::IBeamCursor : Qt::ArrowCursor));
    if (link && !link->uri.isEmpty()) {
        // Says where a link goes before it's followed, with anything unsafe marked as such.
        const QString tip = safeWebLink(link->uri).isEmpty() ? tr("This link can't be opened from here") : link->uri;
        QToolTip::showText(event->globalPosition().toPoint(), tip, viewport());
    } else {
        QToolTip::hideText();
    }
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint position = event->position().toPoint();
    if (m_drawingArea) {
        m_areaEnd = pointOnPage(m_areaPage, position);
        const QRectF area = drawnArea();
        const int page = m_areaPage;
        stopDrawingArea();
        if (area.width() >= kSmallestArea && area.height() >= kSmallestArea)
            emit areaChosen(page, area, position);
        return;
    }
    const bool wasClick = (position - m_pressPosition).manhattanLength() < QApplication::startDragDistance();
    m_dragging = false;
    if (wasClick && !m_pressedOnLink && m_rulerPage >= 0 && event->modifiers() == Qt::NoModifier) {
        if (const auto found = pageAt(position, false))
            emit pageClicked(found->first, found->second);
        return;
    }
    if (!wasClick || !m_pressedOnLink)
        return;
    if (const ReaderLink *link = linkAt(position)) {
        if (!link->uri.isEmpty())
            emit externalLinkActivated(link->uri);
        else if (link->page >= 0)
            goToPoint(link->page, link->y);
    }
}

void PageView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy"), QKeySequence::Copy, this, [this] {
        QApplication::clipboard()->setText(selectedText());
    });
    copy->setEnabled(hasSelection());
    emit contextMenuWanted(&menu, event->pos());
    menu.exec(event->globalPos());
}

} // namespace papertrail::ui
