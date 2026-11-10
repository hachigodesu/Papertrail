#pragma once

#include "ui/DocumentClient.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QTimer>
#include <QTransform>
#include <QVector>

#include <optional>

class QMenu;

namespace papertrail::ui {

// Shows a document's pages, and only ever holds the pages near the view in memory. This does no
// parsing of its own: images, links and text all come from the document's own process.
struct AnnotationMark {
    qint64 id = 0;
    QList<QRectF> rects; // In points.
    QColor colour;
    bool underline = false;
    bool hasNote = false;
    bool area = false; // An area drawn over the page, outlined so what's under it can still be seen.
};

struct SelectionPart {
    int page = 0;
    QList<QRectF> rects;
    QString text;
};

class PageView : public QAbstractScrollArea {
    Q_OBJECT

public:
    enum class LayoutMode { Continuous, SinglePage, TwoPages };
    enum class ZoomMode { Fixed, FitWidth, FitPage };
    enum class Colours { Light, Dark, Sepia };

    explicit PageView(QWidget *parent = nullptr);

    void setPageCount(int count, const QSizeF &assumedSize);
    void setPageSizes(int start, const QList<QSizeF> &sizes);
    void forgetAllPages(); // After laying the document out again.

    void setPageImage(int page, double scale, const QImage &image);
    void setPageFailed(int page);
    void setLinks(int page, const QList<ReaderLink> &links);
    // Where a page's pictures are, in points. Dark pages leave them as they are.
    void setImageAreas(int page, const QList<QRectF> &areas);
    QList<QRectF> imageAreasOn(int page) const { return m_imageAreas.value(page); }

    // How much memory rendered pages may take, in bytes. Less when two documents share a tab.
    void setImageBudget(qint64 bytes);

    void setLayoutMode(LayoutMode mode);
    LayoutMode layoutMode() const { return m_layoutMode; }

    void setColours(Colours colours);
    Colours colours() const { return m_colours; }

    void setZoomMode(ZoomMode mode);
    ZoomMode zoomMode() const { return m_zoomMode; }

    // In points to pixels. 96/72 is actual size.
    void setScale(double scale);
    double scale() const { return m_scale; }
    void zoomBy(double factor);

    int pageCount() const { return int(m_sizes.size()); }
    // A page's own size in points, however it's turned.
    QSizeF pageSize(int page) const { return m_sizes.value(page); }

    // Turns every page clockwise by 0, 90, 180 or 270 degrees. Everything given to the view and everything it reports,
    // such as marks, selections and points on a page, stays in the page's own unturned points.
    void setRotation(int degrees);
    int rotation() const { return m_rotation; }

    // Shows only pages `first` to `last`, counting from 0, such as one article in a volume. A negative
    // `first` shows them all again. Pages outside aren't laid out, so they can't be scrolled to.
    void setPageRange(int first, int last);
    bool hasPageRange() const { return m_rangeFirst >= 0; }
    int firstShownPage() const;
    int lastShownPage() const;

    // The page taking up the middle of the view.
    int currentPage() const;

    // The page at the top of the view, and how far down it the view starts, from 0 to 1.
    int topPage() const;
    double topOffset() const;

    void goToPage(int page, double offset = 0);

    // Goes to a point on a page, given in points from the top.
    void goToPoint(int page, double y);

    void nextPage();
    void previousPage();

    // Goes back to where the last followed link was clicked, and forward again to where it led.
    void goBack();
    bool canGoBack() const { return !m_history.isEmpty(); }
    void goForward();
    bool canGoForward() const { return !m_forward.isEmpty(); }

    void setSearchHits(int page, const QList<QRectF> &rects);
    void clearSearch();
    void setCurrentHit(int page, const QRectF &rect);

    // A reading ruler: everything but this part of this page is dimmed. Up and Down ask for it to move, with
    // rulerStepRequested, rather than scrolling, and a plain click says where with pageClicked.
    void setRuler(int page, const QRectF &rect);
    void clearRuler();
    bool hasRuler() const { return m_rulerPage >= 0; }
    int rulerPage() const { return m_rulerPage; }
    QRectF rulerRect() const { return m_rulerRect; }
    // Scrolls only as far as needed for this part of a page to be in view, keeping it a little from the edge.
    void scrollToShow(int page, const QRectF &rect);

    void setSelection(int page, const QList<QRectF> &rects, const QString &text);
    void clearSelection();
    QString selectedText() const;
    bool hasSelection() const { return !m_selectionText.isEmpty(); }
    QList<SelectionPart> selectionParts() const; // In page order.

    // What's drawn on each page for highlights and notes. These are looked after by whoever owns the view.
    void setMarks(int page, const QList<AnnotationMark> &marks);
    void clearMarks();
    QList<AnnotationMark> marksOn(int page) const { return m_marks.value(page); }
    QList<int> pagesWithMarks() const;
    std::optional<qint64> annotationAt(const QPoint &viewportPoint) const;

    // Areas are drawn with Shift and a drag, or with any drag after asking for one. Only where pages are fixed: elsewhere
    // trying says so with areaRefused.
    void setAreasAllowed(bool allowed) { m_areasAllowed = allowed; }
    void startArea(); // The next drag draws an area.
    bool isWaitingForArea() const { return m_areaMode; }

    // The link at a place in the view, if there's one.
    std::optional<ReaderLink> linkAtPoint(const QPoint &viewportPoint) const;
    bool isDrawingArea() const { return m_drawingArea; }

    // For tests.
    QRectF pageRect(int page) const;
    QPoint viewportPointFor(int page, const QPointF &point) const;
    bool isPageLoaded(int page) const { return m_images.contains(page) && m_images.value(page).scale == wantedScale(); }
    double wantedScale() const;

signals:
    // Replaces any earlier list.
    void pagesWanted(const QList<papertrail::ui::RenderRequest> &pages);
    void linksWanted(int page);
    void imageAreasWanted(int page);
    void selectionWanted(int page, const QPointF &a, const QPointF &b);
    void currentPageChanged(int page);
    void zoomChanged();
    void placeChanged();
    void selectionChanged();
    void historyChanged();

    // A right-click menu is about to appear, for the owner to add to.
    void contextMenuWanted(QMenu *menu, const QPoint &viewportPoint);

    // A link out of the document has been clicked. It comes from the file, so it can't be trusted.
    void externalLinkActivated(const QString &uri);

    // Ctrl and a click on a page, to see what the words there refer to, at `point` in points.
    void referenceWanted(int page, const QPointF &point, const QPoint &viewportPoint);

    // An area has been drawn on a page, in points, ending at `viewportPoint`.
    void areaChosen(int page, const QRectF &rect, const QPoint &viewportPoint);
    void areaRefused();

    // Escape was pressed with nothing here for it to stop, such as a selection, so something around the view may use it.
    void escapeUnused();
    // Up or Down with a ruler shown: -1 or 1.
    void rulerStepRequested(int step);
    // A plain click on a page, not on a link, with a ruler shown.
    void pageClicked(int page, const QPointF &point);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    struct CachedImage {
        QImage image;
        double scale = 0;
        quint64 lastUsed = 0;
    };

    struct Selected {
        QList<QRectF> rects;
        QString text;
    };

    struct Place {
        int page = 0;
        double offset = 0;
    };

    void relayout();
    void relayoutKeepingPlace();
    void updateScrollBars();
    void updateHorizontalRange();
    void updateTypicalPageSize();
    void scheduleWanted();
    void sendWanted();
    void noteCurrentPage();
    void trimCache();
    int rowForY(double y) const;
    int rowOfPage(int page) const;
    int rowStart(int row) const; // The first page of a row.
    int shownEnd() const; // One past the last page that can be shown.
    int pagesPerRow() const;
    double contentX() const;
    double contentY() const;
    void updateFitScale();
    void drawPage(QPainter &painter, int page, const QRectF &target);
    void drawUnturnedPage(QPainter &painter, int page, const QRectF &target);

    // A page's size as shown, turned by the rotation, in points.
    QSizeF shownSize(int page) const;
    // From a page's own points to content pixels, through its scale and rotation, and back.
    QTransform pageToContent(int page) const;
    QPointF contentToPage(int page, const QPointF &content) const;

    // The page and the point on it, in points, at a place in the view. Nearest page if between pages.
    std::optional<std::pair<int, QPointF>> pageAt(const QPoint &viewportPoint, bool nearest = false) const;
    const ReaderLink *linkAt(const QPoint &viewportPoint) const;
    void updateSelectionTo(int page, const QPointF &point);
    QPointF pointOnPage(int page, const QPoint &viewportPoint) const; // Kept within the page.
    QRectF drawnArea() const;
    void stopDrawingArea();
    void showRow(int row, bool atEnd = false);

    QVector<QSizeF> m_sizes;
    QSizeF m_typicalPageSize;
    QVector<QRectF> m_pageRects; // In content pixels. Empty where a page isn't on show.
    QVector<double> m_rowTops;
    QVector<double> m_rowHeights;
    double m_contentWidth = 0;
    double m_contentHeight = 0;
    int m_shownRow = 0; // For the modes that show one row at a time.
    int m_rangeFirst = -1; // See setPageRange.
    int m_rangeLast = -1;

    LayoutMode m_layoutMode = LayoutMode::Continuous;
    ZoomMode m_zoomMode = ZoomMode::FitWidth;
    Colours m_colours = Colours::Light;
    double m_scale = 96.0 / 72.0;
    int m_rotation = 0;

    QHash<int, CachedImage> m_images;
    QSet<int> m_failedPages;
    quint64 m_useClock = 0;
    qint64 m_imageBytes = 0;
    qint64 m_imageBudget = 0;

    QHash<int, QList<ReaderLink>> m_links;
    QSet<int> m_linksAsked;
    QHash<int, QList<QRectF>> m_imageAreas;
    QSet<int> m_imageAreasAsked;

    QHash<int, QList<AnnotationMark>> m_marks;
    QHash<int, QList<QRectF>> m_searchHits;
    int m_currentHitPage = -1;
    int m_rulerPage = -1;
    QRectF m_rulerRect;
    QRectF m_currentHitRect;

    QHash<int, Selected> m_selected;
    QString m_selectionText;
    bool m_dragging = false;
    bool m_pressedOnLink = false;
    int m_dragStartPage = 0;
    QPointF m_dragStartPoint;
    QPoint m_pressPosition;

    bool m_areasAllowed = true;
    bool m_areaMode = false;
    bool m_drawingArea = false;
    int m_areaPage = 0;
    QPointF m_areaStart;
    QPointF m_areaEnd;

    QList<Place> m_history;
    QList<Place> m_forward; // Places gone back from, until something new is followed.
    QTimer m_wantedTimer;
    int m_lastCurrentPage = -1;
    bool m_settingScroll = false;
};

} // namespace papertrail::ui
