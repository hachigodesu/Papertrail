#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <memory>
#include <stdexcept>

namespace papertrail::engine {

class EngineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A Kindle book locked with DRM. Papertrail can't read these, and doesn't try to.
class KindleProtected : public EngineError {
public:
    using EngineError::EngineError;
};

// A rendered page as tightly packed 8-bit RGB.
struct RenderedPage {
    int width = 0;
    int height = 0;
    QByteArray rgb;

    // The share of pixels that aren't close to white, from 0 to 1.
    double inkFraction() const;
};

// A rendered page for the screen: 32 bits a pixel, blue first, on a white background.
struct DisplayImage {
    int width = 0;
    int height = 0;
    int stride = 0;
    QByteArray pixels;
};

struct OutlineItem {
    QString title;
    int depth = 0;
    int page = -1; // -1 if it doesn't lead anywhere in this document.
    double y = 0;  // In points from the top of the page.
};

struct PageLink {
    QRectF rect;   // In points.
    QString uri;   // Set for a link out of the document. Comes from the file, so it can't be trusted.
    int page = -1; // Set for a link within the document.
    double y = 0;
};

struct TextSelection {
    QList<QRectF> rects;
    QString text;
};

// One open document. Nothing here ever writes to the document's file.
class Document {
public:
    virtual ~Document() = default;

    virtual bool needsPassword() = 0;
    virtual int pageCount() = 0;
    virtual bool isReflowable() = 0;

    // Throws EngineError if the page can't be read.
    virtual QString pageText(int page) = 0;
    virtual RenderedPage renderPage(int page, int maxDimension) = 0;

    // The page's size in points. Throws EngineError if it can't be read.
    virtual QSizeF pageSize(int page) = 0;

    // `scale` is pixels per point. Throws EngineError if the page can't be rendered.
    virtual DisplayImage renderForDisplay(int page, double scale);

    // Lays a reflowable document out again, in points. Does nothing for fixed layouts.
    virtual void setLayout(double width, double height, double fontSize);

    // A position that stays meaningful after laying out again, but only while this is open.
    virtual qint64 bookmarkForPage(int page);
    virtual int pageForBookmark(qint64 bookmark);

    // A position to keep for next time, which survives a change of layout. It's approximate in a
    // reflowable document: the same part of the same chapter. Just the page number otherwise.
    virtual QString placeForPage(int page);
    virtual int pageForPlace(const QString &place);

    // These are empty where a format doesn't support them.
    virtual QList<OutlineItem> outline() { return {}; }
    virtual QList<PageLink> links(int) { return {}; }
    virtual QList<QRectF> search(int, const QString &) { return {}; }

    // The text between two points on a page, as a reader would select it by dragging.
    virtual TextSelection selectText(int, const QPointF &, const QPointF &) { return {}; }

    // The text on the first page set in the largest type, which is often the title.
    virtual QString largestTextOnFirstPage() = 0;

    // Keys such as "info:Title", where the format has them.
    virtual QMap<QString, QString> metadata() = 0;

    // Raw embedded metadata. Empty if the format or file has none.
    virtual QByteArray xmpPacket() = 0;
    virtual QByteArray epubPackageDocument() = 0;
};

// Opens documents with the right engine for each format. Use one per thread.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Throws EngineError if the file can't be opened as a document.
    std::unique_ptr<Document> open(const QString &path);

    // Writes an EPUB made from a KF8 Kindle file, which MuPDF can't read itself. Throws
    // KindleProtected if it's locked with DRM, or EngineError if it can't be converted.
    void convertKf8ToEpub(const QString &path, const QString &epubPath);

    // Writes the file only if encoding succeeds, replacing any earlier version.
    void saveJpeg(const RenderedPage &page, const QString &path, int quality = 80);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace papertrail::engine
