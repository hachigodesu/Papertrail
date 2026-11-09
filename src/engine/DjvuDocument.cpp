#include "engine/Backends.h"

#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>

#include <QStringList>

#include <algorithm>
#include <utility>

namespace papertrail::engine {

namespace {

void waitForMessages(ddjvu_context_t *ctx)
{
    ddjvu_message_wait(ctx);
    while (ddjvu_message_peek(ctx))
        ddjvu_message_pop(ctx);
}

// A page's text is a tree of (kind x y x y child...) lists, with strings as leaves.
void collectText(miniexp_t node, QString &out)
{
    if (miniexp_stringp(node)) {
        out += QString::fromUtf8(miniexp_to_str(node));
        out += u' ';
        return;
    }
    while (miniexp_consp(node)) {
        collectText(miniexp_car(node), out);
        node = miniexp_cdr(node);
    }
}

// One word of a page's hidden text, with its position in points from the top left.
struct Word {
    QRectF rect;
    QString text;
    bool endsLine = false;
};

// A page's text is a tree of (kind x0 y0 x1 y1 child...) lists, in pixels up from the bottom left.
void collectWords(miniexp_t node, double pointsPerPixel, double pageHeight, QList<Word> &words)
{
    if (!miniexp_consp(node) || !miniexp_symbolp(miniexp_car(node)))
        return;
    const char *kind = miniexp_to_name(miniexp_car(node));
    miniexp_t rest = miniexp_cdr(node);
    int box[4] = {};
    for (int &value : box) {
        if (!miniexp_consp(rest) || !miniexp_numberp(miniexp_car(rest)))
            return;
        value = miniexp_to_int(miniexp_car(rest));
        rest = miniexp_cdr(rest);
    }

    if (miniexp_consp(rest) && miniexp_stringp(miniexp_car(rest))) {
        Word word;
        word.rect = QRectF(box[0] * pointsPerPixel, (pageHeight - box[3]) * pointsPerPixel,
                           (box[2] - box[0]) * pointsPerPixel, (box[3] - box[1]) * pointsPerPixel);
        word.text = QString::fromUtf8(miniexp_to_str(miniexp_car(rest)));
        words << word;
    } else {
        for (; miniexp_consp(rest); rest = miniexp_cdr(rest))
            collectWords(miniexp_car(rest), pointsPerPixel, pageHeight, words);
    }

    const QLatin1StringView level(kind);
    if (!words.isEmpty() && (level == QLatin1StringView("line") || level == QLatin1StringView("para")
                             || level == QLatin1StringView("column") || level == QLatin1StringView("region"))) {
        words.last().endsLine = true;
    }
}

double distanceTo(const QRectF &rect, const QPointF &point)
{
    const double dx = qMax(qMax(rect.left() - point.x(), point.x() - rect.right()), 0.0);
    const double dy = qMax(qMax(rect.top() - point.y(), point.y() - rect.bottom()), 0.0);
    return dx * dx + dy * dy;
}

class DjvuDocument : public Document {
public:
    DjvuDocument(ddjvu_context_t *ctx, ddjvu_document_t *doc)
        : m_ctx(ctx), m_doc(doc)
    {
    }

    ~DjvuDocument() override { ddjvu_document_release(m_doc); }

    QList<OutlineItem> outline() override
    {
        miniexp_t tree;
        while ((tree = ddjvu_document_get_outline(m_doc)) == miniexp_dummy)
            waitForMessages(m_ctx);
        QList<OutlineItem> items;
        if (miniexp_consp(tree) && miniexp_symbolp(miniexp_car(tree)))
            collectOutline(miniexp_cdr(tree), 0, items);
        ddjvu_miniexp_release(m_doc, tree);
        return items;
    }

    QList<PageLink> links(int page) override
    {
        miniexp_t annotations;
        while ((annotations = ddjvu_document_get_pageanno(m_doc, page)) == miniexp_dummy)
            waitForMessages(m_ctx);

        QList<PageLink> links;
        ddjvu_pageinfo_t info{};
        if (annotations != miniexp_nil && pageInfo(page, info)) {
            const double pointsPerPixel = 72.0 / info.dpi;
            for (miniexp_t rest = annotations; miniexp_consp(rest); rest = miniexp_cdr(rest)) {
                // (maparea url comment (rect x y w h) ...)
                const miniexp_t area = miniexp_car(rest);
                if (!miniexp_consp(area) || miniexp_car(area) != miniexp_symbol("maparea"))
                    continue;
                const miniexp_t target = miniexp_nth(1, area);
                const char *url = miniexp_stringp(target) ? miniexp_to_str(target)
                    : miniexp_consp(target) && miniexp_stringp(miniexp_nth(1, target)) ? miniexp_to_str(miniexp_nth(1, target))
                                                                                       : nullptr;
                const miniexp_t shape = miniexp_nth(3, area);
                if (!url || !miniexp_consp(shape) || miniexp_length(shape) < 5
                    || !(miniexp_car(shape) == miniexp_symbol("rect") || miniexp_car(shape) == miniexp_symbol("oval")
                         || miniexp_car(shape) == miniexp_symbol("text"))) {
                    continue;
                }
                const int x = miniexp_to_int(miniexp_nth(1, shape));
                const int y = miniexp_to_int(miniexp_nth(2, shape));
                const int w = miniexp_to_int(miniexp_nth(3, shape));
                const int h = miniexp_to_int(miniexp_nth(4, shape));

                PageLink link;
                link.rect = QRectF(x * pointsPerPixel, (info.height - y - h) * pointsPerPixel, w * pointsPerPixel, h * pointsPerPixel);
                const QString address = QString::fromUtf8(url);
                if (address.startsWith(u'#')) {
                    link.page = pageForDestination(address, page);
                    if (link.page < 0)
                        continue;
                } else {
                    link.uri = address;
                }
                links << link;
            }
        }
        ddjvu_miniexp_release(m_doc, annotations);
        return links;
    }

    QList<QRectF> search(int page, const QString &query) override
    {
        const QStringList wanted = query.simplified().split(u' ', Qt::SkipEmptyParts);
        if (wanted.isEmpty())
            return {};
        const QList<Word> &words = wordsOn(page);
        constexpr int kMaxHits = 512;

        // Words are matched in sequence, so that a phrase can run across a line break.
        QList<QRectF> rects;
        for (qsizetype start = 0; start + wanted.size() <= words.size() && rects.size() < kMaxHits; ++start) {
            bool matched = true;
            for (qsizetype i = 0; i < wanted.size() && matched; ++i) {
                const QString &word = words.at(start + i).text;
                const bool last = i == wanted.size() - 1;
                const bool first = i == 0;
                matched = (first && last) ? word.contains(wanted.at(i), Qt::CaseInsensitive)
                    : first               ? word.endsWith(wanted.at(i), Qt::CaseInsensitive)
                    : last                ? word.startsWith(wanted.at(i), Qt::CaseInsensitive)
                                          : word.compare(wanted.at(i), Qt::CaseInsensitive) == 0;
            }
            if (matched) {
                for (qsizetype i = 0; i < wanted.size(); ++i)
                    rects << words.at(start + i).rect;
            }
        }
        return rects;
    }

    // DjVu's hidden text places words, not characters, so each word's box is shared out evenly among its characters.
    QList<TextLine> textLines(int page) override
    {
        QList<TextLine> lines;
        TextLine line;
        const auto finish = [&] {
            if (!line.text.trimmed().isEmpty())
                lines << line;
            line = TextLine();
        };
        for (const Word &word : wordsOn(page)) {
            if (!line.text.isEmpty()) {
                line.text += u' ';
                const QRectF previous = line.chars.last();
                line.chars << QRectF(previous.right(), previous.top(), qMax(0.0, word.rect.left() - previous.right()),
                                     previous.height());
            }
            const qsizetype count = qMax<qsizetype>(1, word.text.size());
            const double width = word.rect.width() / double(count);
            for (qsizetype i = 0; i < word.text.size(); ++i) {
                line.text += word.text.at(i);
                line.chars << QRectF(word.rect.left() + width * double(i), word.rect.top(), width, word.rect.height());
            }
            line.rect = line.rect.isNull() ? word.rect : line.rect.united(word.rect);
            if (word.endsLine)
                finish();
        }
        finish();
        return lines;
    }

    TextSelection selectText(int page, const QPointF &a, const QPointF &b) override
    {
        const QList<Word> &words = wordsOn(page);
        if (words.isEmpty())
            return {};

        const auto nearest = [&](const QPointF &point) {
            qsizetype best = 0;
            double bestDistance = distanceTo(words.first().rect, point);
            for (qsizetype i = 1; i < words.size() && bestDistance > 0; ++i) {
                const double distance = distanceTo(words.at(i).rect, point);
                if (distance < bestDistance) {
                    best = i;
                    bestDistance = distance;
                }
            }
            return best;
        };
        qsizetype first = nearest(a);
        qsizetype last = nearest(b);
        if (first > last)
            std::swap(first, last);

        TextSelection selection;
        for (qsizetype i = first; i <= last; ++i) {
            const Word &word = words.at(i);
            selection.rects << word.rect;
            selection.text += word.text;
            if (i < last)
                selection.text += word.endsLine ? u'\n' : u' ';
        }
        return selection;
    }

    bool needsPassword() override { return false; }
    bool isReflowable() override { return false; }
    int pageCount() override { return ddjvu_document_get_pagenum(m_doc); }

    QString pageText(int page) override
    {
        miniexp_t text;
        while ((text = ddjvu_document_get_pagetext(m_doc, page, "page")) == miniexp_dummy)
            waitForMessages(m_ctx);

        QString result;
        if (text != miniexp_nil)
            collectText(text, result);
        ddjvu_miniexp_release(m_doc, text);
        return result;
    }

    RenderedPage renderPage(int pageNumber, int maxDimension) override
    {
        ddjvu_page_t *page = ddjvu_page_create_by_pageno(m_doc, pageNumber);
        if (!page)
            throw EngineError("could not load page");
        while (ddjvu_job_status(ddjvu_page_job(page)) < DDJVU_JOB_OK)
            waitForMessages(m_ctx);
        if (ddjvu_job_status(ddjvu_page_job(page)) >= DDJVU_JOB_FAILED) {
            ddjvu_page_release(page);
            throw EngineError("could not decode page");
        }

        const int width = ddjvu_page_get_width(page);
        const int height = ddjvu_page_get_height(page);
        const double scale = double(maxDimension) / std::max(std::max(width, height), 1);

        RenderedPage result;
        result.width = std::max(1, int(width * scale));
        result.height = std::max(1, int(height * scale));
        result.rgb.resize(qsizetype(result.width) * result.height * 3);

        ddjvu_format_t *format = ddjvu_format_create(DDJVU_FORMAT_RGB24, 0, nullptr);
        ddjvu_format_set_row_order(format, 1);
        ddjvu_rect_t rect{0, 0, unsigned(result.width), unsigned(result.height)};
        const int rendered = ddjvu_page_render(page, DDJVU_RENDER_COLOR, &rect, &rect, format,
                                               unsigned(result.width) * 3, result.rgb.data());
        ddjvu_format_release(format);
        ddjvu_page_release(page);
        if (!rendered)
            throw EngineError("could not render page");
        return result;
    }

    QSizeF pageSize(int page) override
    {
        ddjvu_pageinfo_t info{};
        if (!pageInfo(page, info))
            throw EngineError("could not read page size");
        return {info.width * 72.0 / info.dpi, info.height * 72.0 / info.dpi};
    }

    QString largestTextOnPage(int) override { return {}; } // DjVu text has no font sizes.
    QMap<QString, QString> metadata() override { return {}; }
    QByteArray xmpPacket() override { return {}; }
    QByteArray epubPackageDocument() override { return {}; }

private:
    bool pageInfo(int page, ddjvu_pageinfo_t &info)
    {
        ddjvu_status_t status;
        while ((status = ddjvu_document_get_pageinfo(m_doc, page, &info)) < DDJVU_JOB_OK)
            waitForMessages(m_ctx);
        return status < DDJVU_JOB_FAILED && info.dpi > 0;
    }

    // The words of the page last asked about, which a drag to select asks about over and over.
    const QList<Word> &wordsOn(int page)
    {
        if (page == m_wordsPage)
            return m_words;
        m_words.clear();
        m_wordsPage = page;
        ddjvu_pageinfo_t info{};
        if (!pageInfo(page, info))
            return m_words;
        miniexp_t text;
        while ((text = ddjvu_document_get_pagetext(m_doc, page, "word")) == miniexp_dummy)
            waitForMessages(m_ctx);
        if (text != miniexp_nil)
            collectWords(text, 72.0 / info.dpi, info.height, m_words);
        ddjvu_miniexp_release(m_doc, text);
        return m_words;
    }

    // A destination such as "#12", "#+1" or "#chapter1.djvu". Returns -1 if it's nowhere in this file.
    int pageForDestination(const QString &destination, int fromPage)
    {
        const QString name = destination.mid(1);
        bool isNumber = false;
        const int number = name.toInt(&isNumber);
        const int pages = pageCount();
        int page = -1;
        if (isNumber)
            page = name.startsWith(u'+') || name.startsWith(u'-') ? fromPage + number : number - 1;
        if (page >= 0 && page < pages)
            return page;

        const int files = ddjvu_document_get_filenum(m_doc);
        for (int i = 0; i < files; ++i) {
            ddjvu_fileinfo_t info{};
            if (ddjvu_document_get_fileinfo(m_doc, i, &info) < DDJVU_JOB_FAILED && info.pageno >= 0) {
                const QByteArray utf8 = name.toUtf8();
                if ((info.id && utf8 == info.id) || (info.name && utf8 == info.name) || (info.title && utf8 == info.title))
                    return info.pageno;
            }
        }
        return -1;
    }

    // (bookmarks ("Title" "#1" ("Child" "#2") ...) ...)
    void collectOutline(miniexp_t items, int depth, QList<OutlineItem> &out)
    {
        for (; miniexp_consp(items); items = miniexp_cdr(items)) {
            const miniexp_t item = miniexp_car(items);
            if (!miniexp_consp(item) || !miniexp_stringp(miniexp_car(item)))
                continue;
            OutlineItem entry;
            entry.title = QString::fromUtf8(miniexp_to_str(miniexp_car(item))).simplified();
            entry.depth = depth;
            const miniexp_t destination = miniexp_nth(1, item);
            if (miniexp_stringp(destination))
                entry.page = pageForDestination(QString::fromUtf8(miniexp_to_str(destination)), 0);
            out << entry;
            collectOutline(miniexp_cddr(item), depth + 1, out);
        }
    }

    ddjvu_context_t *m_ctx;
    ddjvu_document_t *m_doc;
    QList<Word> m_words;
    int m_wordsPage = -1;
};

} // namespace

DjvuBackend::DjvuBackend()
    : m_ctx(ddjvu_context_create("papertrail"))
{
    if (!m_ctx)
        throw EngineError("could not start the DjVu engine");
}

DjvuBackend::~DjvuBackend()
{
    ddjvu_context_release(m_ctx);
}

std::unique_ptr<Document> DjvuBackend::open(const QString &path)
{
    const QByteArray utf8 = path.toUtf8();
    ddjvu_document_t *doc = ddjvu_document_create_by_filename_utf8(m_ctx, utf8.constData(), 1);
    if (!doc)
        throw EngineError("could not open document");
    while (ddjvu_job_status(ddjvu_document_job(doc)) < DDJVU_JOB_OK)
        waitForMessages(m_ctx);
    if (ddjvu_job_status(ddjvu_document_job(doc)) >= DDJVU_JOB_FAILED) {
        ddjvu_document_release(doc);
        throw EngineError("could not decode document");
    }
    return std::make_unique<DjvuDocument>(m_ctx, doc);
}

} // namespace papertrail::engine
