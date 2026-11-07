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
    QString doi;
    QString volume; // As printed in a running header or on a cover, such as "VOL. 7, NO. 11".
    QString issue;
    int month = 0;
    int year = 0;
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

    // Lays a reflowable document out again, keeping `page`'s content in view. See layoutChanged.
    void setLayout(const ReaderLayout &layout, int page);

    // The pages wanted, most important first. This replaces any earlier list, so pages scrolled
    // past are never rendered.
    void setWantedPages(const QList<RenderRequest> &pages);

    void requestOutline();
    void requestLinks(int page);

    // The largest text on a page, and a DOI printed there, for naming an article that starts on it. The
    // answer always comes, empty if the page couldn't be read, so nothing waits for ever.
    void requestHeading(int page);

    void requestThumbnail(int page, double scale);

    // Only the newest selection on each page matters.
    void requestSelection(int page, const QPointF &a, const QPointF &b);

    // For highlights in text whose pages move. `token` comes back with the answer. If the process dies, the
    // answer still comes, as nothing found, so nothing waits for ever.
    void requestAnchor(qint64 token, int part, int page, const QString &text, const QPointF &firstCorner);
    void requestLocate(qint64 token, int part, const QString &place, const QString &text, int occurrence);

    void requestBookmark(int page);
    void requestPageForBookmark(const QString &bookmark);

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
    void outlineReady(const QList<papertrail::ui::ReaderOutlineItem> &items);
    void linksReady(int page, const QList<papertrail::ui::ReaderLink> &links);
    void headingReady(int page, const papertrail::ui::ReaderHeading &heading);
    void selectionReady(int page, const QList<QRectF> &rects, const QString &text);
    void anchored(qint64 token, int part, const QString &place, int occurrence);
    void located(qint64 token, int part, int page, const QList<QRectF> &rects);
    void bookmarkReady(int page, const QString &bookmark);
    void bookmarkResolved(int page);
    void searchHits(int page, const QList<QRectF> &rects);
    void searchProgress(int pagesDone, int pageCount);
    void searchFinished();
    void recovered(); // The helper crashed, and has been restarted.

private:
    enum class Kind { Open, Layout, Sizes, Render, Thumbnail, Outline, Links, Heading, Select, Bookmark, Anchor, Locate, Find, Search };

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
