#include "ReaderHost.h"

#include "engine/Document.h"
#include "import/Extractors.h"

#include <limits>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QRegularExpression>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace papertrail::worker {

namespace {

// Far larger than any screen needs. The reader asks for less, but a file could claim pages of any size.
constexpr double kMaxPixelsAcross = 16000;
constexpr double kMaxPixels = 100e6;

// Replies carry binary data, so nothing else may reach them. Libraries sometimes print to stdout, so
// this takes a private copy of it, and points the original somewhere harmless.
FILE *claimStandardOutput()
{
#ifdef Q_OS_WIN
    const int privateCopy = _dup(_fileno(stdout));
    _dup2(_fileno(stderr), _fileno(stdout));
    _setmode(privateCopy, _O_BINARY);
    return _fdopen(privateCopy, "wb");
#else
    const int privateCopy = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    return fdopen(privateCopy, "wb");
#endif
}

QJsonArray rectToJson(const QRectF &rect)
{
    return QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()};
}

QJsonArray rectsToJson(const QList<QRectF> &rects)
{
    QJsonArray array;
    for (const QRectF &rect : rects)
        array.append(rectToJson(rect));
    return array;
}

// For testing how the reader copes with a page that crashes or hangs. Only with this variable set,
// which the app itself never does: a file named __crashpage3__.pdf crashes on page 3.
void applyPageFaults(const QString &path, int page)
{
    static const bool enabled = qEnvironmentVariableIsSet("PAPERTRAIL_WORKER_TEST_FAULTS");
    if (!enabled)
        return;
    const QString name = QFileInfo(path).fileName();
    static const QRegularExpression crash(QStringLiteral("__crashpage(\\d+)__"));
    static const QRegularExpression hang(QStringLiteral("__hangpage(\\d+)__"));
    const auto crashMatch = crash.match(name);
    if (crashMatch.hasMatch() && crashMatch.captured(1).toInt() == page)
        std::abort();
    const auto hangMatch = hang.match(name);
    if (hangMatch.hasMatch() && hangMatch.captured(1).toInt() == page) {
        while (true)
            QThread::sleep(60);
    }
}

void applyOpenFaults(const QString &path)
{
    static const bool enabled = qEnvironmentVariableIsSet("PAPERTRAIL_WORKER_TEST_FAULTS");
    if (enabled && QFileInfo(path).fileName().contains(QStringLiteral("__crash__")))
        std::abort();
}

// Enough to hold a Kindle file's headers.
constexpr qint64 kKindleHeadBytes = 256 * 1024;

} // namespace

QString readablePath(engine::Engine &engine, const QString &path, const QString &convertedPath)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1StringView("mobi") && suffix != QLatin1StringView("azw") && suffix != QLatin1StringView("azw3"))
        return path;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw engine::EngineError("could not open the file");
    const import::KindleHeader header = import::parseKindleHeader(file.read(kKindleHeadBytes));
    file.close();
    if (header.isEncrypted)
        throw engine::KindleProtected("this book is protected by DRM");
    if (!header.isKf8)
        return path;
    if (convertedPath.isEmpty())
        throw engine::EngineError("this Kindle book needs converting, and there's nowhere to put it");
    if (!QFileInfo::exists(convertedPath))
        engine.convertKf8ToEpub(path, convertedPath);
    return convertedPath;
}

namespace {

constexpr qsizetype kLongestSingleLineText = 60;

class Host {
public:
    Host(engine::Engine &engine, FILE *out) : m_engine(engine), m_out(out) {}

    void handle(const QJsonObject &request)
    {
        QJsonObject reply;
        QByteArray data;
        reply.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
        try {
            const QString op = request.value(QStringLiteral("op")).toString();
            if (op == QStringLiteral("open"))
                open(request, reply);
            else if (!m_document)
                throw engine::EngineError("no document is open");
            else if (op == QStringLiteral("layout"))
                layout(request, reply);
            else if (op == QStringLiteral("sizes"))
                sizes(request, reply);
            else if (op == QStringLiteral("render"))
                data = render(request, reply);
            else if (op == QStringLiteral("outline"))
                outline(reply);
            else if (op == QStringLiteral("links"))
                links(request, reply);
            else if (op == QStringLiteral("images"))
                images(request, reply);
            else if (op == QStringLiteral("heading"))
                heading(request, reply);
            else if (op == QStringLiteral("anchor"))
                anchor(request, reply);
            else if (op == QStringLiteral("locate"))
                locate(request, reply);
            else if (op == QStringLiteral("search"))
                search(request, reply);
            else if (op == QStringLiteral("select"))
                select(request, reply);
            else if (op == QStringLiteral("bookmark"))
                reply.insert(QStringLiteral("bookmark"), m_document->placeForPage(page(request)));
            else if (op == QStringLiteral("find"))
                reply.insert(QStringLiteral("page"), m_document->pageForPlace(request.value(QStringLiteral("bookmark")).toString()));
            else
                throw engine::EngineError("unknown request");
            reply.insert(QStringLiteral("ok"), true);
        } catch (const std::exception &error) {
            reply.insert(QStringLiteral("ok"), false);
            reply.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
            data.clear();
        }
        if (!data.isEmpty())
            reply.insert(QStringLiteral("bytes"), double(data.size()));

        const QByteArray line = QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(line.constData(), 1, size_t(line.size()), m_out);
        if (!data.isEmpty())
            std::fwrite(data.constData(), 1, size_t(data.size()), m_out);
        std::fflush(m_out);
    }

private:
    static int page(const QJsonObject &request) { return request.value(QStringLiteral("page")).toInt(); }

    void applyLayout(const QJsonObject &request)
    {
        m_document->setLayout(request.value(QStringLiteral("width")).toDouble(595),
                              request.value(QStringLiteral("height")).toDouble(842),
                              request.value(QStringLiteral("em")).toDouble(12));
    }

    void describe(QJsonObject &reply)
    {
        reply.insert(QStringLiteral("pages"), m_document->pageCount());
        reply.insert(QStringLiteral("reflowable"), m_document->isReflowable());
    }

    void open(const QJsonObject &request, QJsonObject &reply)
    {
        m_path = request.value(QStringLiteral("path")).toString();
        applyOpenFaults(m_path);
        try {
            m_document = m_engine.open(readablePath(m_engine, m_path, request.value(QStringLiteral("convertedPath")).toString()));
        } catch (const engine::KindleProtected &) {
            reply.insert(QStringLiteral("protected"), true);
            throw;
        }
        if (m_document->needsPassword()) {
            m_document.reset();
            reply.insert(QStringLiteral("needsPassword"), true);
            throw engine::EngineError("this file needs a password");
        }
        applyLayout(request);
        describe(reply);
        if (m_document->pageCount() < 1)
            throw engine::EngineError("this file has no pages");
    }

    // Keeps the reader's place through the change.
    void layout(const QJsonObject &request, QJsonObject &reply)
    {
        const qint64 mark = m_document->bookmarkForPage(page(request));
        applyLayout(request);
        describe(reply);
        reply.insert(QStringLiteral("page"), m_document->pageForBookmark(mark));
    }

    void sizes(const QJsonObject &request, QJsonObject &reply)
    {
        const int start = request.value(QStringLiteral("start")).toInt();
        const int count = request.value(QStringLiteral("count")).toInt();
        QJsonArray values;
        for (int i = start; i < start + count; ++i) {
            QSizeF size(0, 0);
            try {
                size = m_document->pageSize(i);
            } catch (const engine::EngineError &) {
                // A page that can't be read keeps a placeholder of the usual size.
            }
            values.append(size.width());
            values.append(size.height());
        }
        reply.insert(QStringLiteral("start"), start);
        reply.insert(QStringLiteral("sizes"), values);
    }

    QByteArray render(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        applyPageFaults(m_path, number);
        double scale = request.value(QStringLiteral("scale")).toDouble(1);
        const QSizeF size = m_document->pageSize(number);
        const double largest = qMax(size.width(), size.height());
        if (largest <= 0)
            throw engine::EngineError("this page has no size");
        scale = qMin(scale, kMaxPixelsAcross / largest);
        const double area = size.width() * size.height() * scale * scale;
        if (area > kMaxPixels)
            scale *= std::sqrt(kMaxPixels / area);

        const engine::DisplayImage image = m_document->renderForDisplay(number, scale);
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("width"), image.width);
        reply.insert(QStringLiteral("height"), image.height);
        reply.insert(QStringLiteral("stride"), image.stride);
        return image.pixels;
    }

    void outline(QJsonObject &reply)
    {
        QJsonArray items;
        for (const engine::OutlineItem &item : m_document->outline()) {
            items.append(QJsonObject{{QStringLiteral("title"), item.title},
                                     {QStringLiteral("depth"), item.depth},
                                     {QStringLiteral("page"), item.page},
                                     {QStringLiteral("y"), item.y}});
        }
        reply.insert(QStringLiteral("items"), items);
    }

    void links(const QJsonObject &request, QJsonObject &reply)
    {
        QJsonArray items;
        for (const engine::PageLink &link : m_document->links(page(request))) {
            items.append(QJsonObject{{QStringLiteral("rect"), rectToJson(link.rect)},
                                     {QStringLiteral("uri"), link.uri},
                                     {QStringLiteral("page"), link.page},
                                     {QStringLiteral("y"), link.y}});
        }
        reply.insert(QStringLiteral("page"), page(request));
        reply.insert(QStringLiteral("links"), items);
    }

    void images(const QJsonObject &request, QJsonObject &reply)
    {
        QJsonArray areas;
        for (const QRectF &area : m_document->imageAreas(page(request)))
            areas.append(rectToJson(area));
        reply.insert(QStringLiteral("page"), page(request));
        reply.insert(QStringLiteral("areas"), areas);
    }

    // What an article starting on this page is probably called, for a table of contents that only numbers them.
    void heading(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        if (number < 0 || number >= m_document->pageCount())
            throw engine::EngineError("there's no such page");
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("title"), m_document->largestTextOnPage(number).left(500));
        const QString text = m_document->pageText(number);
        reply.insert(QStringLiteral("doi"), import::articleDoiOnPage(text));
        // All of them too, for telling where articles start in a file with no contents. A few are enough.
        reply.insert(QStringLiteral("dois"), QJsonArray::fromStringList(import::doisOnPage(text).mid(0, 8)));
        if (const std::optional<import::IssueNumbering> numbering = import::findIssueNumbering(text.left(4000))) {
            reply.insert(QStringLiteral("volume"), numbering->volume);
            reply.insert(QStringLiteral("issue"), numbering->issue);
            if (numbering->month)
                reply.insert(QStringLiteral("month"), *numbering->month);
            if (numbering->year)
                reply.insert(QStringLiteral("year"), *numbering->year);
        }
    }

    void search(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("rects"),
                     rectsToJson(m_document->search(number, request.value(QStringLiteral("text")).toString())));
    }

    // Notes what would find this text again after the pages have moved: where its page starts, and which of
    // any repeats on that page it is. Long stretches are unlikely to repeat, and run over several lines.
    void anchor(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        reply.insert(QStringLiteral("place"), m_document->placeForPage(number));
        const QString text = request.value(QStringLiteral("text")).toString().simplified();
        int occurrence = 0;
        if (text.size() <= kLongestSingleLineText) {
            const QPointF wanted(request.value(QStringLiteral("x")).toDouble(), request.value(QStringLiteral("y")).toDouble());
            const QList<QRectF> rects = m_document->search(number, text);
            double nearest = std::numeric_limits<double>::max();
            for (int i = 0; i < rects.size(); ++i) {
                const QPointF gap = rects.at(i).topLeft() - wanted;
                const double distance = gap.manhattanLength();
                if (distance < nearest) {
                    nearest = distance;
                    occurrence = i;
                }
            }
        }
        reply.insert(QStringLiteral("occurrence"), occurrence);
    }

    // Finds text from an anchor, looking on the page its place is now on, then either side of it.
    void locate(const QJsonObject &request, QJsonObject &reply)
    {
        const QString text = request.value(QStringLiteral("text")).toString().simplified();
        const int occurrence = request.value(QStringLiteral("occurrence")).toInt();
        const int start = m_document->pageForPlace(request.value(QStringLiteral("place")).toString());
        const int count = m_document->pageCount();

        const auto find = [&](const QString &needle) {
            for (const int offset : {0, 1, 2, 3, -1, -2}) {
                const int number = start + offset;
                if (number < 0 || number >= count)
                    continue;
                const QList<QRectF> rects = m_document->search(number, needle);
                if (rects.isEmpty())
                    continue;
                reply.insert(QStringLiteral("page"), number);
                // A long stretch is one match over several lines, and a short one is one rectangle each.
                if (needle.size() > kLongestSingleLineText) {
                    reply.insert(QStringLiteral("rects"), rectsToJson(rects));
                } else {
                    // On the page it started on, the same one as before. Elsewhere, the first is the best guess.
                    const int index = offset == 0 ? qBound(0, occurrence, int(rects.size()) - 1) : 0;
                    reply.insert(QStringLiteral("rects"), rectsToJson({rects.at(index)}));
                }
                return true;
            }
            return false;
        };
        if (find(text))
            return;
        // A new layout can carry a long stretch over a page break, so its start shows where it is.
        if (text.size() > kLongestSingleLineText && find(text.left(kLongestSingleLineText)))
            return;
        reply.insert(QStringLiteral("page"), -1);
    }

    void select(const QJsonObject &request, QJsonObject &reply)
    {
        const engine::TextSelection selection = m_document->selectText(
            page(request),
            QPointF(request.value(QStringLiteral("ax")).toDouble(), request.value(QStringLiteral("ay")).toDouble()),
            QPointF(request.value(QStringLiteral("bx")).toDouble(), request.value(QStringLiteral("by")).toDouble()));
        reply.insert(QStringLiteral("page"), page(request));
        reply.insert(QStringLiteral("rects"), rectsToJson(selection.rects));
        reply.insert(QStringLiteral("text"), selection.text);
    }

    engine::Engine &m_engine;
    FILE *m_out;
    QString m_path;
    std::unique_ptr<engine::Document> m_document;
};

} // namespace

int runReaderHost(engine::Engine &engine)
{
    FILE *out = claimStandardOutput();
    if (!out)
        return 1;
    Host host(engine, out);
    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonDocument request = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        if (request.isObject())
            host.handle(request.object());
    }
    return 0;
}

} // namespace papertrail::worker
