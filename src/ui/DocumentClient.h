#pragma once

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QProcess>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTimer>

#include <optional>

namespace papertrail::ui {

struct ReaderLayout {
    double width = 520; // In points. Only reflowable documents use these.
    double height = 720;
    double fontSize = 12;
};

struct ReaderOutlineItem {
    QString title;
    int depth = 0;
    int page = -1;
    double y = 0;
};

struct ReaderLink {
    QRectF rect;
    QString uri; // From the file, so it can't be trusted. See safeWebLink.
    int page = -1;
    double y = 0;
};

// What's printed on a page that starts an article: see DocumentClient::requestHeading.
struct ReaderHeading {
    QString title;
    QString doi;       // The page's DOI, if it has only one.
    QStringList dois;  // Every DOI on it, in order.
    QString volume; // As printed in a running header or on a cover, such as "VOL. 7, NO. 11".
    QString issue;
    int month = 0;
    int year = 0;
};

// One line of a page's text, with a box for each of its characters, in points.
struct ReaderTextLine {
    QString text;
    QRectF rect;
    QList<QRectF> chars;
};

// Where a reference in a document's text points: see DocumentClient::requestReference.
struct ReaderReference {
    bool found = false;
    QString label; // Such as "Figure 3", or empty if nothing there was read as a reference.
    int kind = -1; // A core::TextReference::Kind, or -1.
    int page = -1;
    QRectF rect;   // The caption's or entry's first line, in points.
    QRectF region; // What's worth showing of it, such as the figure with its caption.
};

struct RenderRequest {
    int page = 0;
    double scale = 1; // Pixels per point.
    bool operator==(const RenderRequest &) const = default;
};

// One open document, in a separate process of its own. A page that crashes or hangs a parser costs
// that page and a moment's restart, rather than the whole app. Nothing here ever waits: everything
// comes back through signals.
class DocumentClient : public QObject {
    Q_OBJECT

public:
    enum class Failure {
        CantStart,   // The helper program is missing.
        Crashed,     // Opening this file crashes the parser.
        TimedOut,    // Opening this file took too long.
        NeedsPassword,
        Protected,   // A Kindle book locked with DRM.
        Unreadable,  // Not a document that can be read.
    };
    Q_ENUM(Failure)

    explicit DocumentClient(const QString &workerPath, QObject *parent = nullptr);
    ~DocumentClient() override;

    // `convertedPath` is where a Kindle book that has to be converted keeps its readable copy.
    void open(const QString &path, const ReaderLayout &layout, const QString &convertedPath = QString());

    // Where the words recognised on scanned pages are kept, and what reads them: `tessdata` holds the models for
    // `languages`, such as "eng+fra". Set before opening. Without it, scanned pages have no text.
    void setRecognition(const QString &ocrFolder, const QString &tessdata, const QString &languages);
    bool canRecognise() const { return !m_ocrFolder.isEmpty() && !m_tessdata.isEmpty(); }
    // Recognises a scanned page's words, if it hasn't been already, in the background. See pageRecognised.
    void requestRecognition(int page);

    // Lays a reflowable document out again, keeping `page`'s content in view. See layoutChanged.
    void setLayout(const ReaderLayout &layout, int page);

    // The pages wanted, most important first. This replaces any earlier list, so pages scrolled
    // past are never rendered.
    void setWantedPages(const QList<RenderRequest> &pages);

    void requestOutline();
    void requestLinks(int page);
    // Where a page's pictures are, which dark pages leave as they are.
    void requestImageAreas(int page);

    // The largest text on a page, and a DOI printed there, for naming an article that starts on it. The
    // answer always comes, empty if the page couldn't be read, so nothing waits for ever.
    void requestHeading(int page);

    void requestThumbnail(int page, double scale);

    // Part of a page as a picture, `rect` in points. Pictures for a list wait their turn, and one wanted now doesn't.
    // `token` comes back with the answer, which always comes: a null image if it couldn't be made.
    void requestAreaPicture(qint64 token, int page, const QRectF &rect, double scale, bool now);
    // The text inside part of a page. The answer always comes, empty if there's none or it couldn't be read.
    void requestAreaText(qint64 token, int page, const QRectF &rect);
    // A page's lines of text, with where each character is.
    void requestLines(int page);
    // What a reference at a point on a page points at, such as "Figure 3" to its caption, looking only from `first` to
    // `last`. The answer always comes, as not found if there was nothing to find or the process died.
    void requestReference(int page, const QPointF &point, int first, int last);

    // Only the newest selection on each page matters.
    void requestSelection(int page, const QPointF &a, const QPointF &b);

    // For highlights in text whose pages move. `token` comes back with the answer. If the process dies, the
    // answer still comes, as nothing found, so nothing waits for ever.
    void requestAnchor(qint64 token, int part, int page, const QString &text, const QPointF &firstCorner);
    void requestLocate(qint64 token, int part, const QString &place, const QString &text, int occurrence);

    void requestBookmark(int page);
    void requestPageForBookmark(const QString &bookmark);
    // Which page a kept place is on now, answered by placeFound with the same token, without going there.
    void requestPlacePage(qint64 token, const QString &place);

    // Looks through every page in the background, starting from `fromPage`, and wrapping round.
    void startSearch(const QString &text, int fromPage);
    void cancelSearch();

    void setTimeouts(int openMs, int renderMs);

    int restartCount() const { return m_restarts; }
    bool isOpen() const { return m_opened; }

signals:
    void opened(int pageCount, bool reflowable);
    void failed(papertrail::ui::DocumentClient::Failure failure, const QString &message);

    // Sizes arrive in batches, since a long document takes a while to measure.
    void sizesReady(int start, const QList<QSizeF> &sizes);

    void layoutChanged(int pageCount, int page);
    void pageRendered(int page, double scale, const QImage &image);
    void pageFailed(int page);
    void thumbnailReady(int page, const QImage &image);
    void areaPictureReady(qint64 token, int page, const QImage &image);
    void areaTextReady(qint64 token, int page, const QString &text);
    void linesReady(int page, const QList<papertrail::ui::ReaderTextLine> &lines);
    void referenceReady(const papertrail::ui::ReaderReference &reference);
    void outlineReady(const QList<papertrail::ui::ReaderOutlineItem> &items);
    void linksReady(int page, const QList<papertrail::ui::ReaderLink> &links);
    void imageAreasReady(int page, const QList<QRectF> &areas);
    void headingReady(int page, const papertrail::ui::ReaderHeading &heading);
    void selectionReady(int page, const QList<QRectF> &rects, const QString &text);
    void anchored(qint64 token, int part, const QString &place, int occurrence);
    void located(qint64 token, int part, int page, const QList<QRectF> &rects);
    void bookmarkReady(int page, const QString &bookmark);
    void bookmarkResolved(int page);
    void placeFound(qint64 token, int page); // -1 if it couldn't be found.
    // A page's words were recognised: how many, 0 if it isn't a scan, or -1 if they couldn't be.
    void pageRecognised(int page, int words);
    void searchHits(int page, const QList<QRectF> &rects);
    void searchProgress(int pagesDone, int pageCount);
    void searchFinished();
    void recovered(); // The helper crashed, and has been restarted.

private:
    enum class Kind {
        Open, Layout, Sizes, Render, Thumbnail, Outline, Links, Images, Heading, Select, Bookmark, Anchor, Locate, Find,
        Search, AreaPicture, AreaText, Lines, Reference, Recognise
    };

    struct Request {
        Kind kind = Kind::Open;
        QJsonObject body;
        int page = -1;
        double scale = 1;
        qint64 token = 0;
        int part = 0;
    };

    void startProcess();
    void pump();
    void send(const Request &request);
    void onReadyRead();
    void onProcessFinished();
    void onTimeout();
    void handleReply(const QJsonObject &reply, const QByteArray &data);
    void handleLostRequest();
    void queueSizes(int start);
    void continueSearch();
    QJsonObject layoutBody() const;
    QImage imageFrom(const QJsonObject &reply, const QByteArray &data) const;

    QString m_workerPath;
    QString m_path;
    QString m_convertedPath;
    ReaderLayout m_layout;
    QProcess *m_process = nullptr;
    QByteArray m_buffer;
    std::optional<QJsonObject> m_pendingHeader;
    qint64 m_pendingBytes = 0;
    QTimer m_timeout;
    int m_openTimeoutMs = 120000;
    int m_renderTimeoutMs = 60000;

    qint64 m_nextId = 1;
    std::optional<Request> m_inFlight;
    qint64 m_inFlightId = 0;
    QList<Request> m_urgent;
    QList<RenderRequest> m_wanted;
    QList<Request> m_background;
    QString m_ocrFolder;
    QString m_tessdata;
    QString m_languages;
    QSet<int> m_unrecognisable; // Pages whose recognition failed, not asked about again.
    QList<Request> m_thumbnails;
    QHash<int, Request> m_selections;
    QSet<int> m_badPages;

    bool m_opened = false;
    bool m_failed = false;
    bool m_stopping = false;
    int m_restarts = 0;
    int m_pageCount = 0;
    bool m_reflowable = false;

    QString m_searchText;
    int m_searchFrom = 0;
    int m_searchDone = 0;
    bool m_searching = false;
};

} // namespace papertrail::ui

Q_DECLARE_METATYPE(papertrail::ui::ReaderOutlineItem)
Q_DECLARE_METATYPE(papertrail::ui::ReaderLink)
Q_DECLARE_METATYPE(papertrail::ui::ReaderTextLine)
Q_DECLARE_METATYPE(papertrail::ui::ReaderReference)
