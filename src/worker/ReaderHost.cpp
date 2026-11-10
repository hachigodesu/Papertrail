#include "ReaderHost.h"
#include "RecognisedText.h"

#include "core/text/References.h"
#include "engine/Document.h"
#include "import/Extractors.h"

#include <limits>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
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

QRectF rectFromJson(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    return QRectF(array.at(0).toDouble(), array.at(1).toDouble(), array.at(2).toDouble(), array.at(3).toDouble());
}

QJsonArray rectsToJson(const QList<QRectF> &rects)
{
    QJsonArray array;
    for (const QRectF &rect : rects)
        array.append(rectToJson(rect));
    return array;
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

// The size most of a page's text is set in, to tell headings by.
double bodySize(const QList<engine::TextLine> &lines)
{
    QList<QPair<double, qsizetype>> sizes;
    qsizetype total = 0;
    for (const engine::TextLine &line : lines) {
        sizes << qMakePair(line.size, line.text.size());
        total += line.text.size();
    }
    std::sort(sizes.begin(), sizes.end());
    qsizetype counted = 0;
    for (const auto &[size, length] : std::as_const(sizes)) {
        counted += length;
        if (counted * 2 >= total)
            return size;
    }
    return 0;
}

// Whether there's a gap above a line, more than lines of a paragraph have, or nothing above it in its column.
bool standsApart(const QList<engine::TextLine> &lines, int index)
{
    const QRectF line = lines.at(index).rect;
    for (int i = index - 1; i >= 0; --i) {
        const QRectF above = lines.at(i).rect;
        const bool sameColumn = above.right() > line.left() && above.left() < line.right();
        // Lines' boxes often overlap the next line's a little, so it's the line above that counts, however close.
        if (!sameColumn || above.top() >= line.top() - 1)
            continue;
        return line.top() - above.bottom() > line.height() * 0.8;
    }
    return true;
}

// The lines on the same row as this one, left to right, as a contents entry's title and page number often are when
// they're set apart.
struct Row {
    QString text;
    QList<int> lines;
    bool numberApart = false; // It ends with a number set well apart from the rest, or led to by dots.
};

Row rowOf(const QList<engine::TextLine> &lines, int index)
{
    const QRectF line = lines.at(index).rect;
    QList<int> members;
    for (int i = 0; i < lines.size(); ++i) {
        const QRectF other = lines.at(i).rect;
        const double overlap = qMin(line.bottom(), other.bottom()) - qMax(line.top(), other.top());
        if (overlap > qMin(line.height(), other.height()) * 0.5)
            members << i;
    }
    std::sort(members.begin(), members.end(), [&](int a, int b) { return lines.at(a).rect.left() < lines.at(b).rect.left(); });

    Row row;
    row.lines = members;
    const double height = qMax(line.height(), 4.0);
    for (int m = 0; m < members.size(); ++m) {
        const engine::TextLine &piece = lines.at(members.at(m));
        if (m > 0) {
            row.text += u' ';
            // A number on its own, well to the right of the title.
            const double gap = piece.rect.left() - lines.at(members.at(m - 1)).rect.right();
            if (m == members.size() - 1 && gap > height * 2)
                row.numberApart = true;
        }
        row.text += piece.text;
    }
    // Or within one line: the space before its last word, which is wide where the number's set at the right.
    const engine::TextLine &last = lines.at(members.last());
    const QString text = last.text;
    qsizetype end = text.size();
    while (end > 0 && text.at(end - 1).isSpace())
        --end;
    qsizetype start = end;
    while (start > 0 && !text.at(start - 1).isSpace())
        --start;
    qsizetype before = start;
    while (before > 0 && text.at(before - 1).isSpace())
        --before;
    if (before > 0 && start < last.chars.size() && before - 1 < last.chars.size()
        && last.chars.at(start).left() - last.chars.at(before - 1).right() > height * 2)
        row.numberApart = true;
    static const QRegularExpression leaders(QStringLiteral("(?:\\.\\s*){4,}\\S+\\s*$|…\\s*\\S+\\s*$"));
    if (leaders.match(row.text).hasMatch())
        row.numberApart = true;
    return row;
}

// Whether a line is one of a contents page's entries, or like one, such as an entry in a list of figures.
bool isEntryRow(const QList<engine::TextLine> &lines, int index)
{
    const Row row = rowOf(lines, index);
    return row.numberApart && core::contentsEntry(row.text).has_value();
}

// Whether a line sits in a page's top or bottom margin, where running heads and page numbers are, and isn't set larger.
bool inMargin(const engine::TextLine &line, double pageHeight, double body)
{
    if (pageHeight <= 0 || line.size > body + 0.5)
        return false;
    return line.rect.bottom() < pageHeight * 0.075 || line.rect.top() > pageHeight * 0.93;
}

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
            else if (op == QStringLiteral("renderarea"))
                data = renderArea(request, reply);
            else if (op == QStringLiteral("lines"))
                lines(request, reply);
            else if (op == QStringLiteral("areatext"))
                areaText(request, reply);
            else if (op == QStringLiteral("reference"))
                reference(request, reply);
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
            else if (op == QStringLiteral("ocr"))
                recognise(request, reply);
            else if (op == QStringLiteral("rereadwords"))
                rereadWords();
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
            m_ocrFolder = request.value(QStringLiteral("ocrFolder")).toString();
            m_document = withRecognisedText(
                m_engine.open(readablePath(m_engine, m_path, request.value(QStringLiteral("convertedPath")).toString())), m_ocrFolder);
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

    // Reads the kept words again, as after pages have been recognised by another process.
    void rereadWords()
    {
        if (auto *withWords = dynamic_cast<engine::RecognisedTextDocument *>(m_document.get())) {
            withWords->forgetWords();
            withWords->setPreferRecognised(recognisingEveryPage(m_ocrFolder));
        }
        m_lines.clear();
        m_contentsPages.clear();
    }

    // Recognises a scanned page's words, keeps them, and answers its text questions from them from now on.
    void recognise(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        reply.insert(QStringLiteral("page"), number);
        auto *withWords = dynamic_cast<engine::RecognisedTextDocument *>(m_document.get());
        if (!withWords || m_ocrFolder.isEmpty())
            throw engine::EngineError("there's nowhere to keep recognised words");
        // Asked to recognise every page, from now on its recognised words are used in place of its own.
        const bool everyPage = request.value(QStringLiteral("everyPage")).toBool() || withWords->prefersRecognised();
        if (everyPage && !withWords->prefersRecognised()) {
            markRecognisingEveryPage(m_ocrFolder);
            withWords->setPreferRecognised(true);
            m_lines.clear();
            m_contentsPages.clear();
        }
        if (const std::optional<QList<engine::TextWord>> kept = readRecognised(m_ocrFolder, number)) {
            reply.insert(QStringLiteral("words"), int(kept->size()));
            return;
        }
        if (!everyPage && !engine::looksScanned(withWords->inner(), number)) {
            reply.insert(QStringLiteral("words"), 0);
            return;
        }
        applyPageFaults(m_path, number);
        static engine::Recogniser recogniser;
        const QList<engine::TextWord> words = recognisePage(recogniser, withWords->inner(), number,
                                                            request.value(QStringLiteral("tessdata")).toString(),
                                                            request.value(QStringLiteral("languages")).toString());
        writeRecognised(m_ocrFolder, number, words, request.value(QStringLiteral("languages")).toString());
        withWords->setWords(number, words);
        m_lines.remove(number);
        m_contentsPages.remove(number);
        reply.insert(QStringLiteral("words"), int(words.size()));
    }

    // Keeps the reader's place through the change.
    void layout(const QJsonObject &request, QJsonObject &reply)
    {
        const qint64 mark = m_document->bookmarkForPage(page(request));
        m_lines.clear();
        m_contentsPages.clear();
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

    // The scale asked for, made smaller where it would give an image too large to be sensible.
    double boundedScale(int number, double scale)
    {
        const QSizeF size = m_document->pageSize(number);
        const double largest = qMax(size.width(), size.height());
        if (largest <= 0)
            throw engine::EngineError("this page has no size");
        scale = qMin(scale, kMaxPixelsAcross / largest);
        const double area = size.width() * size.height() * scale * scale;
        if (area > kMaxPixels)
            scale *= std::sqrt(kMaxPixels / area);
        return scale;
    }

    QByteArray render(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        applyPageFaults(m_path, number);
        const double scale = boundedScale(number, request.value(QStringLiteral("scale")).toDouble(1));
        const engine::DisplayImage image = m_document->renderForDisplay(number, scale);
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("width"), image.width);
        reply.insert(QStringLiteral("height"), image.height);
        reply.insert(QStringLiteral("stride"), image.stride);
        return image.pixels;
    }

    // Part of a page, in points, as a picture: for an area marked on it, or a preview of something it holds.
    QByteArray renderArea(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        applyPageFaults(m_path, number);
        const double scale = boundedScale(number, request.value(QStringLiteral("scale")).toDouble(1));
        const engine::DisplayImage image = m_document->renderForDisplay(number, scale);
        const QRect whole(0, 0, image.width, image.height);
        const QRectF wanted = rectFromJson(request.value(QStringLiteral("rect")));
        const QRect crop = QRect(int(std::floor(wanted.left() * scale)), int(std::floor(wanted.top() * scale)),
                                 int(std::ceil(wanted.width() * scale)), int(std::ceil(wanted.height() * scale)))
                               .intersected(whole);
        if (crop.isEmpty())
            throw engine::EngineError("that part of the page is empty");
        const int stride = crop.width() * 4;
        QByteArray pixels(qsizetype(stride) * crop.height(), Qt::Uninitialized);
        for (int y = 0; y < crop.height(); ++y) {
            std::memcpy(pixels.data() + qsizetype(y) * stride,
                        image.pixels.constData() + qsizetype(crop.top() + y) * image.stride + qsizetype(crop.left()) * 4,
                        size_t(stride));
        }
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("width"), crop.width());
        reply.insert(QStringLiteral("height"), crop.height());
        reply.insert(QStringLiteral("stride"), stride);
        return pixels;
    }

    // A page's lines, kept for a while since finding what a reference points at reads many pages, often again.
    const QList<engine::TextLine> &linesOn(int number)
    {
        constexpr qsizetype kPagesKept = 200;
        if (auto found = m_lines.constFind(number); found != m_lines.cend())
            return *found;
        if (m_lines.size() >= kPagesKept)
            m_lines.clear();
        applyPageFaults(m_path, number);
        QList<engine::TextLine> read;
        try {
            read = m_document->textLines(number);
        } catch (const engine::EngineError &) {
            // A page that can't be read has no text to offer.
        }
        return *m_lines.insert(number, read);
    }

    void lines(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        const bool withChars = request.value(QStringLiteral("chars")).toBool();
        QJsonArray items;
        for (const engine::TextLine &line : linesOn(number)) {
            QJsonObject item{{QStringLiteral("text"), line.text}, {QStringLiteral("rect"), rectToJson(line.rect)}};
            if (withChars)
                item.insert(QStringLiteral("chars"), rectsToJson(line.chars));
            items.append(item);
        }
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("lines"), items);
    }

    // The words inside an area marked on a page, so that searching finds it.
    void areaText(const QJsonObject &request, QJsonObject &reply)
    {
        const int number = page(request);
        applyPageFaults(m_path, number);
        reply.insert(QStringLiteral("page"), number);
        reply.insert(QStringLiteral("text"), m_document->textInRect(number, rectFromJson(request.value(QStringLiteral("rect")))));
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

    // Whether a page is a table of contents, or a list of figures or tables: headed as one, or mostly entries ending
    // with their page numbers, set at the right or led to by dots.
    bool isContentsPage(int number)
    {
        if (const auto known = m_contentsPages.constFind(number); known != m_contentsPages.cend())
            return *known;
        const QList<engine::TextLine> &lines = linesOn(number);
        bool contents = false;
        int entries = 0;
        int rows = 0;
        QSet<int> seen;
        for (int i = 0; i < lines.size() && !contents; ++i) {
            if (core::isContentsHeading(lines.at(i).text) && i < 6)
                contents = true;
            if (seen.contains(i))
                continue;
            const Row row = rowOf(lines, i);
            for (const int member : row.lines)
                seen.insert(member);
            ++rows;
            if (row.numberApart && core::contentsEntry(row.text))
                ++entries;
        }
        contents = contents || (entries >= 4 && entries * 10 >= rows * 4);
        m_contentsPages.insert(number, contents);
        return contents;
    }

    // Goes from an entry in a table of contents to the heading it names, as a link would: the line most set apart as a
    // heading that matches the entry, nearest the page it gives. Printed pages are usually behind the file's, by the
    // pages before them, so it looks from there on first. Returns false if the line isn't an entry, or its heading
    // isn't found.
    bool jumpFromContents(int number, int clicked, int first, int last, QJsonObject &reply)
    {
        const QList<engine::TextLine> &here = linesOn(number);
        const Row row = rowOf(here, clicked);
        const std::optional<core::ContentsEntry> entry = core::contentsEntry(row.text);
        if (!entry || !(row.numberApart && isContentsPage(number)))
            return false;
        const std::optional<HeadingFound> heading = headingFor(*entry, number, first, last);
        if (!heading)
            return false;
        replyWithHeading(*heading, entry->title.simplified().left(80), reply);
        return true;
    }

    struct HeadingFound {
        int page = -1;
        QRectF rect; // The heading's line, in points.
    };

    // The heading a contents entry names: where the document's own outline says, if it has the same title, or else
    // the line most set apart as a heading that matches it, nearest the page the entry gives. Printed pages are usually
    // behind the file's, by the pages before them, so it looks from there on first. `contentsPage` is skipped.
    std::optional<HeadingFound> headingFor(const core::ContentsEntry &entry, int contentsPage, int first, int last)
    {
        for (const engine::OutlineItem &item : m_document->outline()) {
            if (item.page < first || item.page > last || !core::headingMatchesEntry(item.title, entry))
                continue;
            // The line there, if it can be found, or else the place itself.
            const QList<engine::TextLine> &lines = linesOn(item.page);
            for (const engine::TextLine &line : lines) {
                if (core::headingMatchesEntry(line.text, entry) && qAbs(line.rect.top() - item.y) < 60)
                    return HeadingFound{item.page, line.rect};
            }
            const double width = m_document->pageSize(item.page).width();
            return HeadingFound{item.page, QRectF(width * 0.08, item.y, width * 0.84, 14)};
        }

        // From the printed page on, then everything else after the contents, then before them.
        const int number = contentsPage;
        constexpr int kAhead = 120;
        constexpr int kFarthest = 400;
        const int printed = qBound(first, first + entry.page - 1, last);
        QList<int> pages;
        QSet<int> listed;
        const auto add = [&](int at) {
            if (at >= first && at <= last && at != number && !listed.contains(at) && pages.size() < kFarthest) {
                listed.insert(at);
                pages << at;
            }
        };
        for (int at = qMax(printed, number + 1); at <= printed + kAhead; ++at)
            add(at);
        for (int at = number + 1; at <= last; ++at)
            add(at);
        for (int at = number - 1; at >= first; --at)
            add(at);

        int bestPage = -1;
        int bestLine = -1;
        int best = 0;
        for (qsizetype p = 0; p < pages.size(); ++p) {
            const int at = pages.at(p);
            if (isContentsPage(at))
                continue;
            const QList<engine::TextLine> &lines = linesOn(at);
            const double body = bodySize(lines);
            const double pageHeight = m_document->pageSize(at).height();
            for (int i = 0; i < lines.size(); ++i) {
                const engine::TextLine &line = lines.at(i);
                if (!core::headingMatchesEntry(line.text, entry) || inMargin(line, pageHeight, body) || isEntryRow(lines, i))
                    continue;
                // Only a heading counts, not the same words in a sentence.
                int points = 0;
                if (line.bold)
                    points += 2;
                if (line.size > body + 0.5)
                    points += 2;
                if (standsApart(lines, i))
                    points += 1;
                if (points > best) {
                    best = points;
                    bestPage = at;
                    bestLine = i;
                }
            }
            // Set apart in more than one way, near where it's expected: that's it.
            if (best >= 3 && p < kAhead)
                break;
        }
        if (bestPage < 0)
            return std::nullopt;
        return HeadingFound{bestPage, linesOn(bestPage).at(bestLine).rect};
    }

    void replyWithHeading(const HeadingFound &heading, const QString &label, QJsonObject &reply)
    {
        const QSizeF size = m_document->pageSize(heading.page);
        const QRectF region = QRectF(size.width() * 0.04, heading.rect.top() - 4, size.width() * 0.92, size.height() * 0.22)
                                  .intersected(QRectF(QPointF(0, 0), size));
        reply.insert(QStringLiteral("label"), label);
        reply.insert(QStringLiteral("kind"), int(core::TextReference::Kind::Section));
        reply.insert(QStringLiteral("found"), true);
        reply.insert(QStringLiteral("page"), heading.page);
        reply.insert(QStringLiteral("rect"), rectToJson(heading.rect));
        reply.insert(QStringLiteral("region"), rectToJson(region));
    }

    // The entry in the document's contents for a section or appendix, such as "2.1 Variance" for "Section 2.1" or
    // "Appendix E: Statistical Tables" for "Appendix E". The contents are looked for near the start.
    std::optional<std::pair<core::ContentsEntry, int>> contentsEntryFor(const core::TextReference &reference, int first, int last)
    {
        constexpr int kContentsWithin = 40;
        const QString number = reference.number.toLower();
        const QString named = (reference.word + u' ' + reference.number).toLower();
        for (int at = first; at <= qMin(last, first + kContentsWithin); ++at) {
            if (!isContentsPage(at))
                continue;
            const QList<engine::TextLine> &lines = linesOn(at);
            for (int i = 0; i < lines.size(); ++i) {
                const Row row = rowOf(lines, i);
                if (row.lines.value(0) != i || !row.numberApart)
                    continue;
                const std::optional<core::ContentsEntry> entry = core::contentsEntry(row.text);
                if (!entry)
                    continue;
                // Its label, then a space or punctuation: "2.1 Variance" for 2.1, but not "2.10 Variance".
                QString title = entry->title.toLower().simplified();
                title.replace(QRegularExpression(QStringLiteral("[:.)]")), QStringLiteral(" "));
                title = title.simplified() + u' ';
                const bool labelled = title.startsWith(named + u' ')
                                      || (reference.kind == core::TextReference::Kind::Section && title.startsWith(number + u' '));
                if (labelled)
                    return std::make_pair(*entry, at);
            }
        }
        return std::nullopt;
    }

    // Where a reference in the text points, such as "Figure 3" to its caption or "[12]" to its entry in the
    // references, for a document whose links don't say. `first` and `last` limit it to the pages shown, so an article
    // in an issue finds its own references.
    void reference(const QJsonObject &request, QJsonObject &reply)
    {
        using Kind = core::TextReference::Kind;
        const int number = page(request);
        const int count = m_document->pageCount();
        const int first = qBound(0, request.value(QStringLiteral("first")).toInt(0), count - 1);
        const int last = qBound(first, request.value(QStringLiteral("last")).toInt(count - 1), count - 1);
        const QPointF point(request.value(QStringLiteral("x")).toDouble(), request.value(QStringLiteral("y")).toDouble());
        reply.insert(QStringLiteral("found"), false);

        // The line clicked, with the lines either side, so a reference split over two lines is read whole.
        const QList<engine::TextLine> &here = linesOn(number);
        int clicked = -1;
        double nearest = 4; // In points: a click just beside a line still counts.
        for (int i = 0; i < here.size(); ++i) {
            const QRectF rect = here.at(i).rect;
            const double dx = qMax(qMax(rect.left() - point.x(), point.x() - rect.right()), 0.0);
            const double dy = qMax(qMax(rect.top() - point.y(), point.y() - rect.bottom()), 0.0);
            if (dx + dy < nearest) {
                nearest = dx + dy;
                clicked = i;
            }
        }
        if (clicked < 0)
            return;
        const engine::TextLine &line = here.at(clicked);
        int offset = 0;
        double closest = std::numeric_limits<double>::max();
        for (int i = 0; i < line.chars.size(); ++i) {
            const QRectF box = line.chars.at(i);
            const double distance = box.contains(point) ? -1 : qAbs(box.center().x() - point.x());
            if (distance < closest) {
                closest = distance;
                offset = i;
            }
        }
        // An entry in a table of contents goes to the heading it names, which the file may not link to.
        if (jumpFromContents(number, clicked, first, last, reply))
            return;

        QString text = line.text;
        if (clicked > 0) {
            const QString before = here.at(clicked - 1).text + u' ';
            text.prepend(before);
            offset += int(before.size());
        }
        if (clicked + 1 < here.size())
            text += u' ' + here.at(clicked + 1).text;
        const std::optional<core::TextReference> found = core::referenceAt(text, offset);
        if (!found)
            return;
        reply.insert(QStringLiteral("label"), found->label());
        reply.insert(QStringLiteral("kind"), int(found->kind));

        const auto isClicked = [&](int at, int index) { return at == number && index == clicked; };
        // Pages to look through, nearest the click first, which is where captions usually are. A long book only has
        // the pages near it read, so the answer doesn't take minutes: a caption that far away would be unusual. The
        // references can be further, at the end of a long book's chapter, but they're looked for from one end only.
        constexpr int kNearby = 150;
        constexpr int kFarthest = 300;
        QList<int> outward;
        for (int distance = 0; distance <= kNearby && (number - distance >= first || number + distance <= last); ++distance) {
            if (number + distance <= last)
                outward << number + distance;
            if (distance > 0 && number - distance >= first)
                outward << number - distance;
        }

        int targetPage = -1;
        int targetLine = -1;
        // The best of the lines that match, by how much they're set apart as a heading, caption or equation is, with the
        // nearest winning a tie. A sentence that happens to wrap onto a new line with "Appendix E." scores nothing.
        const auto lookForBest = [&](const QList<int> &pages, int least,
                                     const std::function<int(const QList<engine::TextLine> &, int, double)> &score) {
            int best = least - 1;
            for (const int at : pages) {
                // A table of contents, or a list of figures or tables, names everything but is none of it.
                if (isContentsPage(at))
                    continue;
                const QList<engine::TextLine> &lines = linesOn(at);
                const double body = bodySize(lines);
                for (int i = 0; i < lines.size(); ++i) {
                    if (isClicked(at, i))
                        continue;
                    const int points = score(lines, i, body);
                    if (points > best) {
                        best = points;
                        targetPage = at;
                        targetLine = i;
                    }
                }
            }
            return targetPage >= 0;
        };
        // A table of contents' entry has dots leading to its page number, which a caption or heading never has.
        static const QRegularExpression leaders(QStringLiteral("(?:\\.\\s*){4,}\\d+\\s*$|…\\s*\\d+\\s*$"));

        switch (found->kind) {
        case Kind::Equation: {
            // Set apart from the text before it, with space before its number. The nearest before the mention first,
            // where an equation's usually set out, or else after.
            QList<int> before;
            QList<int> after;
            for (const int at : std::as_const(outward))
                (at <= number ? before : after) << at;
            std::sort(before.begin(), before.end(), std::greater<int>());
            const auto numbered = [&](const QList<engine::TextLine> &lines, int i, double) {
                const engine::TextLine &line = lines.at(i);
                if (!core::endsWithEquationNumber(line.text, found->number))
                    return -1;
                const qsizetype open = line.text.lastIndexOf(u'(');
                // The number on a line of its own, set to the right of the equation.
                if (line.text.left(open).trimmed().isEmpty())
                    return 3;
                int points = 0;
                qsizetype previous = open - 1;
                while (previous >= 0 && line.text.at(previous).isSpace())
                    --previous;
                if (previous >= 0 && open < line.chars.size() && line.chars.at(open).left() - line.chars.at(previous).right() > 12)
                    points += 3;
                if (standsApart(lines, i))
                    points += 1;
                if (line.text.size() < 90)
                    points += 1;
                return points;
            };
            if (!lookForBest(before, 3, numbered))
                lookForBest(after, 3, numbered);
            break;
        }
        case Kind::Citation:
        case Kind::AuthorYear: {
            // Within the list of references: the last heading for one, or anywhere from the click on if there's none.
            int headingPage = -1;
            int headingLine = -1;
            for (int at = last; at >= first && headingPage < 0 && last - at <= kFarthest; --at) {
                const QList<engine::TextLine> &lines = linesOn(at);
                for (int i = int(lines.size()) - 1; i >= 0; --i) {
                    if (core::isReferencesHeading(lines.at(i).text)) {
                        headingPage = at;
                        headingLine = i;
                        break;
                    }
                }
            }
            const int from = headingPage >= 0 ? headingPage : number;
            const auto entry = [&](const QString &text, int at, int index) {
                if (at == headingPage && index <= headingLine)
                    return false;
                if (found->kind == Kind::Citation)
                    return core::startsNumberedEntry(text, found->number);
                if (!core::startsAuthorEntry(text, found->surname))
                    return false;
                // The year's in the entry's first few lines.
                const QList<engine::TextLine> &lines = linesOn(at);
                QString start;
                for (int i = index; i < qMin<int>(index + 4, int(lines.size())); ++i)
                    start += lines.at(i).text + u' ';
                return core::entryHasYear(start, found->year)
                       || (found->year.size() > 4 && core::entryHasYear(start, found->year.left(4)));
            };
            for (int at = from; at <= last && targetPage < 0 && at - from <= kFarthest; ++at) {
                const QList<engine::TextLine> &lines = linesOn(at);
                for (int i = 0; i < lines.size(); ++i) {
                    if (!isClicked(at, i) && entry(lines.at(i).text, at, i)) {
                        targetPage = at;
                        targetLine = i;
                        break;
                    }
                }
            }
            break;
        }
        default: {
            // A caption or heading has space above it, or is set in bold or another size, unlike a sentence that wraps
            // onto a new line. A heading is short.
            const bool heading = found->kind == Kind::Section || found->kind == Kind::Appendix;
            const double pageHeight = m_document->pageSize(number).height();
            const auto caption = [&](const core::TextReference &wanted) {
                return [&, wanted](const QList<engine::TextLine> &lines, int i, double body) {
                    const engine::TextLine &line = lines.at(i);
                    // An appendix is often headed by its letter alone, as "E Derivations", in bold or larger type.
                    static const QRegularExpression letterHeading(QStringLiteral("^\\s*([A-Z])[.:]?\\s+\\p{Lu}"));
                    const bool styled = line.bold || line.size > body + 0.5;
                    const QRegularExpressionMatch letter = letterHeading.match(line.text);
                    const bool appendixByLetter = wanted.kind == Kind::Appendix && styled && letter.hasMatch()
                                                  && letter.captured(1) == wanted.number && line.text.trimmed().size() < 80;
                    if (!core::isTargetLine(wanted, line.text) && !appendixByLetter)
                        return -1;
                    if (leaders.match(line.text).hasMatch() || isEntryRow(lines, i))
                        return -1;
                    // A running head repeats a chapter's or section's title at the top of its pages.
                    if (heading && inMargin(line, pageHeight, body))
                        return -1;
                    int points = 0;
                    if (standsApart(lines, i))
                        points += 2;
                    if (line.bold)
                        points += 2;
                    if (line.size > body + 0.5)
                        points += 2;
                    else if (line.size > 0 && line.size < body - 0.5)
                        points += 1; // Captions are often set smaller.
                    if (heading && !styled && line.text.trimmed().size() > 60)
                        return -1; // A long line that isn't set apart is a sentence.
                    return points;
                };
            };
            if (heading) {
                if (const auto entry = contentsEntryFor(*found, first, last)) {
                    if (const std::optional<HeadingFound> fromContents = headingFor(entry->first, entry->second, first, last)) {
                        replyWithHeading(*fromContents, found->label(), reply);
                        return;
                    }
                }
            }
            if (!lookForBest(outward, 2, caption(*found))) {
                // "Figure 3b" is often captioned as part of "Figure 3".
                static const QRegularExpression lettered(QStringLiteral("^(\\d+(?:\\.\\d+)*)[a-z]$"));
                const QRegularExpressionMatch base = lettered.match(found->number);
                if (base.hasMatch()) {
                    core::TextReference whole = *found;
                    whole.number = base.captured(1);
                    lookForBest(outward, 2, caption(whole));
                }
            }
            break;
        }
        }
        if (targetPage < 0)
            return;

        const QList<engine::TextLine> &lines = linesOn(targetPage);
        const QRectF target = lines.at(targetLine).rect;
        const QSizeF size = m_document->pageSize(targetPage);
        const QRectF pageArea(QPointF(0, 0), size);
        const double lineHeight = qMax(target.height(), 8.0);
        // The lines that follow on, close beneath, such as the rest of a caption or a reference.
        const auto following = [&](int most, const std::function<bool(const engine::TextLine &)> &stop) {
            QRectF area = target;
            double bottom = target.bottom();
            for (int i = targetLine + 1; i < lines.size() && i <= targetLine + most; ++i) {
                const engine::TextLine &next = lines.at(i);
                if (next.rect.top() < bottom - 1 || next.rect.top() > bottom + lineHeight * 1.2 || stop(next))
                    break;
                area = area.united(next.rect);
                bottom = next.rect.bottom();
            }
            return area;
        };
        const auto acrossPage = [&](const QRectF &area) {
            return QRectF(size.width() * 0.04, area.top(), size.width() * 0.92, area.height());
        };

        QRectF region;
        switch (found->kind) {
        case Kind::Figure: {
            const QRectF caption = following(4, [](const engine::TextLine &) { return false; });
            // With the picture it captions: the nearest above it, or else below.
            QRectF picture;
            double gap = std::numeric_limits<double>::max();
            for (const QRectF &image : m_document->imageAreas(targetPage)) {
                const double above = caption.top() - image.bottom();
                const double below = image.top() - caption.bottom();
                const double distance = above >= -4 ? above + 0 : (below >= -4 ? below + 1000 : -1);
                if (distance >= -4 && distance < gap) {
                    gap = distance;
                    picture = image;
                }
            }
            // A drawing that isn't a picture is somewhere above, as figures usually are.
            region = picture.isNull() ? acrossPage(QRectF(caption.left(), caption.top() - size.height() * 0.4, caption.width(),
                                                          caption.height() + size.height() * 0.4))
                                      : caption.united(picture).adjusted(-6, -6, 6, 6);
            break;
        }
        case Kind::Table: {
            const QRectF caption = following(3, [](const engine::TextLine &) { return false; });
            region = acrossPage(QRectF(caption.left(), caption.top(), caption.width(), caption.height() + size.height() * 0.35));
            break;
        }
        case Kind::Equation:
            region = acrossPage(target.adjusted(0, -lineHeight * 2.5, 0, lineHeight * 1.5));
            break;
        case Kind::Citation:
        case Kind::AuthorYear: {
            const double left = target.left();
            region = following(8, [&](const engine::TextLine &next) {
                // The next entry starts at the same edge, or with its own number.
                if (found->kind == Kind::Citation)
                    return core::startsNumberedEntry(next.text);
                return qAbs(next.rect.left() - left) < 1.5 && !next.text.isEmpty() && next.text.at(0).isUpper();
            });
            region = region.adjusted(-6, -4, 6, 4);
            break;
        }
        default:
            // A heading or a theorem, and what follows it.
            region = acrossPage(QRectF(target.left(), target.top() - 4, target.width(), size.height() * 0.22));
            break;
        }
        region = region.intersected(pageArea);
        reply.insert(QStringLiteral("found"), true);
        reply.insert(QStringLiteral("page"), targetPage);
        reply.insert(QStringLiteral("rect"), rectToJson(target));
        reply.insert(QStringLiteral("region"), rectToJson(region));
    }

    engine::Engine &m_engine;
    FILE *m_out;
    QString m_path;
    QString m_ocrFolder;
    std::unique_ptr<engine::Document> m_document;
    QHash<int, QList<engine::TextLine>> m_lines;
    QHash<int, bool> m_contentsPages;
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
