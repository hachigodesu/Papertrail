#include "ui/DocumentClient.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace papertrail::ui {

namespace {

constexpr int kOtherTimeoutMs = 30000;
constexpr int kSizesPerRequest = 500;
constexpr int kMaxRestarts = 10;
constexpr qsizetype kMaxWaitingThumbnails = 40;

QRectF rectFromJson(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    return QRectF(array.at(0).toDouble(), array.at(1).toDouble(), array.at(2).toDouble(), array.at(3).toDouble());
}

QList<QRectF> rectsFromJson(const QJsonValue &value)
{
    QList<QRectF> rects;
    const QJsonArray array = value.toArray();
    rects.reserve(array.size());
    for (const QJsonValue &item : array)
        rects << rectFromJson(item);
    return rects;
}

} // namespace

DocumentClient::DocumentClient(const QString &workerPath, QObject *parent)
    : QObject(parent), m_workerPath(workerPath)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &DocumentClient::onTimeout);
}

DocumentClient::~DocumentClient()
{
    m_stopping = true;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void DocumentClient::setTimeouts(int openMs, int renderMs)
{
    m_openTimeoutMs = openMs;
    m_renderTimeoutMs = renderMs;
}

void DocumentClient::open(const QString &path, const ReaderLayout &layout, const QString &convertedPath)
{
    m_path = path;
    m_convertedPath = convertedPath;
    m_layout = layout;
    if (!QFileInfo(m_workerPath).isExecutable()) {
        m_failed = true;
        emit failed(Failure::CantStart, tr("Papertrail's helper program is missing. Reinstalling should fix this."));
        return;
    }
    startProcess();
}

QJsonObject DocumentClient::layoutBody() const
{
    return QJsonObject{{QStringLiteral("width"), m_layout.width},
                       {QStringLiteral("height"), m_layout.height},
                       {QStringLiteral("em"), m_layout.fontSize}};
}

void DocumentClient::startProcess()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
    }
    m_process = new QProcess(this);
    m_process->setProgram(m_workerPath);
    m_process->setArguments({QStringLiteral("--reader")});
    m_process->setStandardErrorFile(QProcess::nullDevice());
    connect(m_process, &QProcess::readyReadStandardOutput, this, &DocumentClient::onReadyRead);
    connect(m_process, &QProcess::finished, this, &DocumentClient::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onProcessFinished();
    });
    m_buffer.clear();
    m_pendingHeader.reset();
    m_pendingBytes = 0;
    m_process->start(QIODevice::ReadWrite);

    // Whatever else is queued waits for this. After a restart, it's what makes the new process useful.
    Request open;
    open.kind = Kind::Open;
    open.body = layoutBody();
    open.body.insert(QStringLiteral("op"), QStringLiteral("open"));
    open.body.insert(QStringLiteral("path"), m_path);
    open.body.insert(QStringLiteral("convertedPath"), m_convertedPath);
    m_inFlight.reset();
    m_urgent.prepend(open);
    pump();
}

void DocumentClient::send(const Request &request)
{
    m_inFlight = request;
    m_inFlightId = m_nextId++;
    QJsonObject body = request.body;
    body.insert(QStringLiteral("id"), m_inFlightId);
    m_process->write(QJsonDocument(body).toJson(QJsonDocument::Compact) + '\n');

    int timeout = kOtherTimeoutMs;
    if (request.kind == Kind::Open || request.kind == Kind::Layout)
        timeout = m_openTimeoutMs;
    else if (request.kind == Kind::Render || request.kind == Kind::Thumbnail)
        timeout = m_renderTimeoutMs;
    m_timeout.start(timeout);
}

void DocumentClient::pump()
{
    if (m_inFlight || !m_process || m_failed || m_process->state() == QProcess::NotRunning)
        return;

    if (!m_urgent.isEmpty()) {
        send(m_urgent.takeFirst());
        return;
    }
    if (!m_opened)
        return; // Nothing else makes sense until it's open.

    if (!m_selections.isEmpty()) {
        const int page = m_selections.cbegin().key();
        send(m_selections.take(page));
        return;
    }
    while (!m_wanted.isEmpty()) {
        const RenderRequest wanted = m_wanted.takeFirst();
        if (m_badPages.contains(wanted.page))
            continue;
        Request request;
        request.kind = Kind::Render;
        request.page = wanted.page;
        request.scale = wanted.scale;
        request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("render")},
                                   {QStringLiteral("page"), wanted.page},
                                   {QStringLiteral("scale"), wanted.scale}};
        send(request);
        return;
    }
    while (!m_thumbnails.isEmpty()) {
        const Request request = m_thumbnails.takeLast(); // The newest is the one on screen.
        if (m_badPages.contains(request.page))
            continue;
        send(request);
        return;
    }
    if (!m_background.isEmpty())
        send(m_background.takeFirst());
}

void DocumentClient::onReadyRead()
{
    m_buffer += m_process->readAllStandardOutput();
    while (true) {
        if (!m_pendingHeader) {
            const qsizetype end = m_buffer.indexOf('\n');
            if (end < 0)
                return;
            const QJsonDocument header = QJsonDocument::fromJson(m_buffer.left(end));
            m_buffer.remove(0, end + 1);
            if (!header.isObject())
                continue;
            m_pendingHeader = header.object();
            m_pendingBytes = qint64(m_pendingHeader->value(QStringLiteral("bytes")).toDouble());
        }
        if (m_buffer.size() < m_pendingBytes)
            return;
        const QByteArray data = m_buffer.left(m_pendingBytes);
        m_buffer.remove(0, m_pendingBytes);
        const QJsonObject reply = *m_pendingHeader;
        m_pendingHeader.reset();
        m_pendingBytes = 0;
        handleReply(reply, data);
    }
}

QImage DocumentClient::imageFrom(const QJsonObject &reply, const QByteArray &data) const
{
    const int width = reply.value(QStringLiteral("width")).toInt();
    const int height = reply.value(QStringLiteral("height")).toInt();
    const int stride = reply.value(QStringLiteral("stride")).toInt();
    if (width <= 0 || height <= 0 || stride < width * 4 || data.size() < qint64(stride) * height)
        return {};
    // The bytes are shared with the image rather than copied, and freed along with it.
    auto *owned = new QByteArray(data);
    return QImage(reinterpret_cast<const uchar *>(owned->constData()), width, height, stride, QImage::Format_RGB32,
                  [](void *bytes) { delete static_cast<QByteArray *>(bytes); }, owned);
}

void DocumentClient::handleReply(const QJsonObject &reply, const QByteArray &data)
{
    if (!m_inFlight || qint64(reply.value(QStringLiteral("id")).toDouble()) != m_inFlightId)
        return;
    m_timeout.stop();
    const Request request = *m_inFlight;
    m_inFlight.reset();
    const bool ok = reply.value(QStringLiteral("ok")).toBool();
    const QString error = reply.value(QStringLiteral("error")).toString();

    switch (request.kind) {
    case Kind::Open:
        if (!ok) {
            m_failed = true;
            emit failed(reply.value(QStringLiteral("needsPassword")).toBool() ? Failure::NeedsPassword
                            : reply.value(QStringLiteral("protected")).toBool()   ? Failure::Protected
                                                                                  : Failure::Unreadable,
                        error);
            return;
        }
        m_pageCount = reply.value(QStringLiteral("pages")).toInt();
        m_reflowable = reply.value(QStringLiteral("reflowable")).toBool();
        if (!m_opened) {
            m_opened = true;
            queueSizes(0);
            emit opened(m_pageCount, m_reflowable);
        } else {
            emit recovered();
        }
        break;
    case Kind::Layout:
        if (ok) {
            m_pageCount = reply.value(QStringLiteral("pages")).toInt();
            m_badPages.clear();
            queueSizes(0);
            emit layoutChanged(m_pageCount, reply.value(QStringLiteral("page")).toInt());
        }
        break;
    case Kind::Sizes:
        if (ok) {
            const int start = reply.value(QStringLiteral("start")).toInt();
            const QJsonArray values = reply.value(QStringLiteral("sizes")).toArray();
            QList<QSizeF> sizes;
            sizes.reserve(values.size() / 2);
            for (qsizetype i = 0; i + 1 < values.size(); i += 2)
                sizes << QSizeF(values.at(i).toDouble(), values.at(i + 1).toDouble());
            emit sizesReady(start, sizes);
            if (start + sizes.size() < m_pageCount)
                queueSizes(start + int(sizes.size()));
        }
        break;
    case Kind::Render:
        if (ok) {
            const QImage image = imageFrom(reply, data);
            if (image.isNull())
                emit pageFailed(request.page);
            else
                emit pageRendered(request.page, request.scale, image);
        } else {
            m_badPages.insert(request.page);
            emit pageFailed(request.page);
        }
        break;
    case Kind::Thumbnail:
        if (ok) {
            const QImage image = imageFrom(reply, data);
            if (!image.isNull())
                emit thumbnailReady(request.page, image);
        }
        break;
    case Kind::Outline: {
        QList<ReaderOutlineItem> items;
        const QJsonArray array = reply.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            items << ReaderOutlineItem{item.value(QStringLiteral("title")).toString(), item.value(QStringLiteral("depth")).toInt(),
                                       item.value(QStringLiteral("page")).toInt(-1), item.value(QStringLiteral("y")).toDouble()};
        }
        emit outlineReady(items);
        break;
    }
    case Kind::Links: {
        QList<ReaderLink> links;
        const QJsonArray array = reply.value(QStringLiteral("links")).toArray();
        for (const QJsonValue &value : array) {
            const QJsonObject item = value.toObject();
            links << ReaderLink{rectFromJson(item.value(QStringLiteral("rect"))), item.value(QStringLiteral("uri")).toString(),
                                item.value(QStringLiteral("page")).toInt(-1), item.value(QStringLiteral("y")).toDouble()};
        }
        emit linksReady(request.page, links);
        break;
    }
    case Kind::Select:
        // A newer selection for this page is already waiting, so this one's out of date.
        if (ok && !m_selections.contains(request.page)) {
            emit selectionReady(request.page, rectsFromJson(reply.value(QStringLiteral("rects"))),
                                reply.value(QStringLiteral("text")).toString());
        }
        break;
    case Kind::Bookmark:
        if (ok)
            emit bookmarkReady(request.page, reply.value(QStringLiteral("bookmark")).toString());
        break;
    case Kind::Find:
        if (ok)
            emit bookmarkResolved(reply.value(QStringLiteral("page")).toInt());
        break;
    case Kind::Search:
        if (m_searching && request.body.value(QStringLiteral("text")).toString() == m_searchText) {
            if (ok) {
                const QList<QRectF> rects = rectsFromJson(reply.value(QStringLiteral("rects")));
                if (!rects.isEmpty())
                    emit searchHits(request.page, rects);
            }
            ++m_searchDone;
            emit searchProgress(m_searchDone, m_pageCount);
            continueSearch();
        }
        break;
    }
    pump();
}

void DocumentClient::handleLostRequest()
{
    if (!m_inFlight)
        return;
    const Request request = *m_inFlight;
    m_inFlight.reset();
    switch (request.kind) {
    case Kind::Render:
    case Kind::Thumbnail:
        // The page is what did it, so it isn't tried again this session.
        m_badPages.insert(request.page);
        emit pageFailed(request.page);
        break;
    case Kind::Search:
        ++m_searchDone;
        emit searchProgress(m_searchDone, m_pageCount);
        continueSearch();
        break;
    default:
        break;
    }
}

void DocumentClient::onProcessFinished()
{
    if (m_stopping || m_failed)
        return;
    m_timeout.stop();

    if (m_inFlight && m_inFlight->kind == Kind::Open && !m_opened) {
        m_failed = true;
        m_inFlight.reset();
        emit failed(Failure::Crashed, tr("Opening this file made the reader stop."));
        return;
    }
    if (!m_opened && !m_inFlight && m_process->error() == QProcess::FailedToStart) {
        m_failed = true;
        emit failed(Failure::CantStart, m_process->errorString());
        return;
    }

    handleLostRequest();
    if (++m_restarts > kMaxRestarts) {
        m_failed = true;
        emit failed(Failure::Crashed, tr("This file keeps making the reader stop."));
        return;
    }
    startProcess();
}

void DocumentClient::onTimeout()
{
    if (!m_process)
        return;
    if (m_inFlight && m_inFlight->kind == Kind::Open && !m_opened) {
        m_failed = true;
        m_inFlight.reset();
        m_process->disconnect(this);
        m_process->kill();
        emit failed(Failure::TimedOut, tr("This file took too long to open."));
        return;
    }
    // The finished signal takes it from here, and treats it the same as a crash.
    m_process->kill();
}

void DocumentClient::setLayout(const ReaderLayout &layout, int page)
{
    m_layout = layout;
    m_wanted.clear();
    m_thumbnails.clear();
    m_selections.clear();
    // Sizes from the old layout are no use now.
    m_background.removeIf([](const Request &request) { return request.kind == Kind::Sizes; });
    Request request;
    request.kind = Kind::Layout;
    request.body = layoutBody();
    request.body.insert(QStringLiteral("op"), QStringLiteral("layout"));
    request.body.insert(QStringLiteral("page"), page);
    m_urgent << request;
    pump();
}

void DocumentClient::setWantedPages(const QList<RenderRequest> &pages)
{
    m_wanted = pages;
    if (m_inFlight && m_inFlight->kind == Kind::Render)
        m_wanted.removeAll(RenderRequest{m_inFlight->page, m_inFlight->scale});
    pump();
}

void DocumentClient::queueSizes(int start)
{
    Request request;
    request.kind = Kind::Sizes;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("sizes")},
                               {QStringLiteral("start"), start},
                               {QStringLiteral("count"), qMin(kSizesPerRequest, m_pageCount - start)}};
    m_background.prepend(request);
}

void DocumentClient::requestOutline()
{
    Request request;
    request.kind = Kind::Outline;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("outline")}};
    m_background << request;
    pump();
}

void DocumentClient::requestLinks(int page)
{
    Request request;
    request.kind = Kind::Links;
    request.page = page;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("links")}, {QStringLiteral("page"), page}};
    m_urgent << request;
    pump();
}

void DocumentClient::requestThumbnail(int page, double scale)
{
    Request request;
    request.kind = Kind::Thumbnail;
    request.page = page;
    request.scale = scale;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("render")},
                               {QStringLiteral("page"), page},
                               {QStringLiteral("scale"), scale}};
    m_thumbnails << request;
    if (m_thumbnails.size() > kMaxWaitingThumbnails)
        m_thumbnails.removeFirst();
    pump();
}

void DocumentClient::requestSelection(int page, const QPointF &a, const QPointF &b)
{
    Request request;
    request.kind = Kind::Select;
    request.page = page;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("select")}, {QStringLiteral("page"), page},
                               {QStringLiteral("ax"), a.x()}, {QStringLiteral("ay"), a.y()},
                               {QStringLiteral("bx"), b.x()}, {QStringLiteral("by"), b.y()}};
    m_selections.insert(page, request);
    pump();
}

void DocumentClient::requestBookmark(int page)
{
    Request request;
    request.kind = Kind::Bookmark;
    request.page = page;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("bookmark")}, {QStringLiteral("page"), page}};
    m_urgent << request;
    pump();
}

void DocumentClient::requestPageForBookmark(const QString &bookmark)
{
    Request request;
    request.kind = Kind::Find;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("find")}, {QStringLiteral("bookmark"), bookmark}};
    m_urgent << request;
    pump();
}

void DocumentClient::startSearch(const QString &text, int fromPage)
{
    cancelSearch();
    if (text.isEmpty() || m_pageCount <= 0)
        return;
    m_searching = true;
    m_searchText = text;
    m_searchFrom = qBound(0, fromPage, m_pageCount - 1);
    m_searchDone = 0;
    continueSearch();
}

void DocumentClient::cancelSearch()
{
    m_searching = false;
    m_searchText.clear();
    m_background.removeIf([](const Request &request) { return request.kind == Kind::Search; });
}

void DocumentClient::continueSearch()
{
    if (!m_searching)
        return;
    if (m_searchDone >= m_pageCount) {
        m_searching = false;
        emit searchFinished();
        return;
    }
    const int page = (m_searchFrom + m_searchDone) % m_pageCount;
    Request request;
    request.kind = Kind::Search;
    request.page = page;
    request.body = QJsonObject{{QStringLiteral("op"), QStringLiteral("search")},
                               {QStringLiteral("page"), page},
                               {QStringLiteral("text"), m_searchText}};
    m_background << request;
    pump();
}

} // namespace papertrail::ui
