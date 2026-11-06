#include "ui/PageView.h"

#include "ui/Labels.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
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

const QColor kSearchColour(255, 200, 0, 110);
const QColor kCurrentHitColour(255, 120, 0, 150);

} // namespace

PageView::PageView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
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
    for (const QSizeF &size : m_sizes) {
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
    m_searchHits.clear();
    m_currentHitPage = -1;
    m_selected.clear();
    m_selectionText.clear();
    m_history.clear();
    emit historyChanged();
    emit selectionChanged();
}

int PageView::pagesPerRow() const
{
    return m_layoutMode == LayoutMode::TwoPages ? 2 : 1;
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
        for (int i = m_shownRow * perRow; i < (m_shownRow + 1) * perRow && i < m_sizes.size(); ++i) {
            width += m_sizes.at(i).width();
            height = qMax(height, m_sizes.at(i).height());
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
    const int pages = int(m_sizes.size());
    const int rows = (pages + perRow - 1) / perRow;
    const bool oneRowAtATime = m_layoutMode != LayoutMode::Continuous;
    m_shownRow = qBound(0, m_shownRow, qMax(0, rows - 1));

    m_pageRects.fill(QRectF(), pages);
    m_rowTops.resize(rows);
    m_rowHeights.resize(rows);

    for (int row = 0; row < rows; ++row) {
        double height = 0;
        for (int i = row * perRow; i < (row + 1) * perRow && i < pages; ++i)
            height = qMax(height, m_sizes.at(i).height() * m_scale);
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
        for (int i = row * perRow; i < (row + 1) * perRow && i < pages; ++i)
            width += m_sizes.at(i).width() * m_scale + kGap;
        // Centred if it fits, and otherwise from the left, with the scroll bar to reach the rest.
        double x = width + 2 * kMargin <= viewport()->width() ? (viewport()->width() - width) / 2 : kMargin;
        for (int i = row * perRow; i < (row + 1) * perRow && i < pages; ++i) {
            const QSizeF size = m_sizes.at(i) * m_scale;
            m_pageRects[i] = QRectF(QPointF(std::round(x), std::round(y)), QSizeF(std::round(size.width()), std::round(size.height())));
            x += size.width() + kGap;
        }
        y += m_rowHeights.at(row) + kGap;
    }
    m_contentHeight = rows > 0 ? y - kGap + kMargin : 0;

    updateScrollBars();
    viewport()->update();
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
            for (int i = row * perRow; i < (row + 1) * perRow && i < m_pageRects.size(); ++i)
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
    return page / pagesPerRow();
}

int PageView::currentPage() const
{
    if (m_sizes.isEmpty())
        return 0;
    const int row = rowForY(contentY() + viewport()->height() / 2.0);
    return qMin(row * pagesPerRow(), int(m_sizes.size()) - 1);
}

int PageView::topPage() const
{
    if (m_sizes.isEmpty())
        return 0;
    const int row = rowForY(contentY() + kMargin + 1);
    return qMin(row * pagesPerRow(), int(m_sizes.size()) - 1);
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
    const int target = qBound(0, page, int(m_sizes.size()) - 1);
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
    emit historyChanged();
    const double height = m_sizes.value(qBound(0, page, int(m_sizes.size()) - 1)).height();
    goToPage(page, height > 0 ? qBound(0.0, y / height, 1.0) : 0);
}

void PageView::goBack()
{
    if (m_history.isEmpty())
        return;
    const Place place = m_history.takeLast();
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
        goToPage(rowOfPage(currentPage()) * pagesPerRow() + pagesPerRow());
    else
        showRow(m_shownRow + 1);
}

void PageView::previousPage()
{
    if (m_layoutMode == LayoutMode::Continuous)
        goToPage((rowOfPage(currentPage()) - 1) * pagesPerRow());
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
    const int pages = int(m_sizes.size());
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
        for (int i = row * perRow; i < (row + 1) * perRow && i < pages; ++i)
            order << i;
    }
    for (const int page : QList<int>(order)) {
        if (!m_linksAsked.contains(page)) {
            m_linksAsked.insert(page);
            emit linksWanted(page);
        }
    }
    // Pages either side, so that turning a page is immediate.
    if (!order.isEmpty()) {
        const int first = order.first();
        const int last = order.last();
        for (int i = 1; i <= kPagesAfter * perRow && last + i < pages; ++i)
            order << last + i;
        for (int i = 1; i <= kPagesBefore * perRow && first - i >= 0; ++i)
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
    if (m_imageBytes <= kMaxImageBytes)
        return;
    // Not whatever's on show, however little room there is.
    const QRectF visible(contentX(), contentY(), viewport()->width(), viewport()->height());
    QList<int> pages = m_images.keys();
    std::sort(pages.begin(), pages.end(),
              [this](int a, int b) { return m_images.value(a).lastUsed < m_images.value(b).lastUsed; });
    for (const int page : pages) {
        if (m_imageBytes <= kMaxImageBytes)
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
    const QRectF hit(pageArea.x() + rect.x() * m_scale, pageArea.y() + rect.y() * m_scale, rect.width() * m_scale,
                     rect.height() * m_scale);
    if (pageArea.isNull() || !visible.contains(hit)) {
        const double height = m_sizes.value(page).height();
        goToPage(page, height > 0 ? qMax(0.0, (rect.y() * m_scale - viewport()->height() / 3.0) / (height * m_scale)) : 0);
    }
    viewport()->update();
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
    const QRectF area = m_pageRects.value(page);
    return QPoint(int(area.x() + point.x() * m_scale - contentX()), int(area.y() + point.y() * m_scale - contentY()));
}

std::optional<std::pair<int, QPointF>> PageView::pageAt(const QPoint &viewportPoint, bool nearest) const
{
    if (m_sizes.isEmpty())
        return std::nullopt;
    const QPointF content(viewportPoint.x() + contentX(), viewportPoint.y() + contentY());
    const int row = rowForY(content.y());
    const int perRow = pagesPerRow();
    for (int i = row * perRow; i < (row + 1) * perRow && i < m_sizes.size(); ++i) {
        const QRectF area = m_pageRects.at(i);
        if (area.contains(content)) {
            return std::make_pair(i, QPointF((content.x() - area.x()) / m_scale, (content.y() - area.y()) / m_scale));
        }
    }
    if (!nearest)
        return std::nullopt;

    int best = qMin(row * perRow, int(m_sizes.size()) - 1);
    if (perRow == 2 && best + 1 < m_sizes.size() && content.x() > m_pageRects.at(best).right())
        best += 1;
    const QRectF area = m_pageRects.at(best);
    if (area.isNull())
        return std::nullopt;
    const QPointF clamped(qBound(area.left(), content.x(), area.right()), qBound(area.top(), content.y(), area.bottom()));
    return std::make_pair(best, QPointF((clamped.x() - area.x()) / m_scale, (clamped.y() - area.y()) / m_scale));
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

void PageView::drawPage(QPainter &painter, int page, const QRectF &target)
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
        painter.setCompositionMode(QPainter::CompositionMode_Difference);
        painter.fillRect(target, QColor(232, 232, 232));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else if (m_colours == Colours::Sepia) {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.fillRect(target, QColor(244, 233, 208));
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    const auto toTarget = [&](const QRectF &rect) {
        return QRectF(target.x() + rect.x() * m_scale, target.y() + rect.y() * m_scale, rect.width() * m_scale,
                      rect.height() * m_scale);
    };
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
        for (int i = row * perRow; i < (row + 1) * perRow && i < m_sizes.size(); ++i) {
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
        goToPage(0);
        return;
    case Qt::Key_End:
        goToPage(int(m_sizes.size()) - 1);
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
        if (horizontalScrollBar()->maximum() == 0) {
            nextPage();
            return;
        }
        break;
    case Qt::Key_Escape:
        clearSelection();
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_pressPosition = event->position().toPoint();
    m_pressedOnLink = linkAt(m_pressPosition) != nullptr;
    clearSelection();
    const auto place = pageAt(m_pressPosition);
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
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        if ((position - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
            if (const auto place = pageAt(position, true))
                updateSelectionTo(place->first, place->second);
        }
        return;
    }

    const ReaderLink *link = linkAt(position);
    viewport()->setCursor(link ? Qt::PointingHandCursor : (pageAt(position) ? Qt::IBeamCursor : Qt::ArrowCursor));
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
    const bool wasClick = (position - m_pressPosition).manhattanLength() < QApplication::startDragDistance();
    m_dragging = false;
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
    menu.exec(event->globalPos());
}

} // namespace papertrail::ui
