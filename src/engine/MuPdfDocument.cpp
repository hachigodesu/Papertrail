// MuPDF reports errors with setjmp and longjmp, which skip C++ destructors. So inside each
// fz_try block there's nothing with a destructor, and nothing that can throw a C++ exception.
// Results stay in plain pointers until the block has finished.

#include "engine/Backends.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace papertrail::engine {

namespace {

[[noreturn]] void throwCaught(const QString &message)
{
    throw EngineError(message.toStdString());
}

QByteArray takeBuffer(fz_context *ctx, fz_buffer *buffer)
{
    if (!buffer)
        return {};
    unsigned char *data = nullptr;
    const size_t size = fz_buffer_storage(ctx, buffer, &data);
    QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    fz_drop_buffer(ctx, buffer);
    return bytes;
}

class MuPdfDocument : public Document {
public:
    MuPdfDocument(fz_context *ctx, const QString &path)
        : m_ctx(ctx), m_path(path.toUtf8())
    {
        const char *pathText = m_path.constData();
        bool failed = false;
        QByteArray message;
        fz_try(m_ctx) {
            m_doc = fz_open_document(m_ctx, pathText);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed)
            throw EngineError("could not open document");
    }

    ~MuPdfDocument() override
    {
        forgetSelectionText();
        fz_drop_document(m_ctx, m_doc);
    }

    bool needsPassword() override
    {
        int needs = 0;
        fz_try(m_ctx) {
            needs = fz_needs_password(m_ctx, m_doc);
        }
        fz_catch(m_ctx) {
            needs = 0;
        }
        return needs != 0;
    }

    int pageCount() override
    {
        layoutIfNeeded();
        int count = 0;
        bool failed = false;
        fz_try(m_ctx) {
            count = fz_count_pages(m_ctx, m_doc);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed)
            throwCaught(QStringLiteral("could not count pages"));
        return count;
    }

    bool isReflowable() override
    {
        int reflowable = 0;
        fz_try(m_ctx) {
            reflowable = fz_is_document_reflowable(m_ctx, m_doc);
        }
        fz_catch(m_ctx) {
            reflowable = 0;
        }
        return reflowable != 0;
    }

    QString pageText(int page) override
    {
        layoutIfNeeded();
        fz_buffer *buffer = nullptr;
        bool failed = false;
        fz_var(buffer);
        fz_try(m_ctx) {
            buffer = fz_new_buffer_from_page_number(m_ctx, m_doc, page, nullptr);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed)
            throwCaught(QStringLiteral("could not read page text"));
        return QString::fromUtf8(takeBuffer(m_ctx, buffer));
    }

    RenderedPage renderPage(int page, int maxDimension) override
    {
        layoutIfNeeded();
        fz_page *loaded = nullptr;
        fz_pixmap *pixmap = nullptr;
        bool failed = false;
        fz_var(loaded);
        fz_var(pixmap);
        fz_try(m_ctx) {
            loaded = fz_load_page(m_ctx, m_doc, page);
            const fz_rect bounds = fz_bound_page(m_ctx, loaded);
            const float largest = std::max(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
            const float scale = largest > 0 ? float(maxDimension) / largest : 1.0f;
            pixmap = fz_new_pixmap_from_page(m_ctx, loaded, fz_scale(scale, scale), fz_device_rgb(m_ctx), 0);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed) {
            fz_drop_pixmap(m_ctx, pixmap);
            fz_drop_page(m_ctx, loaded);
            throwCaught(QStringLiteral("could not render page"));
        }

        RenderedPage result;
        result.width = fz_pixmap_width(m_ctx, pixmap);
        result.height = fz_pixmap_height(m_ctx, pixmap);
        const int stride = fz_pixmap_stride(m_ctx, pixmap);
        const unsigned char *samples = fz_pixmap_samples(m_ctx, pixmap);
        result.rgb.resize(qsizetype(result.width) * result.height * 3);
        for (int y = 0; y < result.height; ++y) {
            std::memcpy(result.rgb.data() + qsizetype(y) * result.width * 3,
                        samples + qsizetype(y) * stride, size_t(result.width) * 3);
        }
        fz_drop_pixmap(m_ctx, pixmap);
        fz_drop_page(m_ctx, loaded);
        return result;
    }

    QSizeF pageSize(int page) override
    {
        layoutIfNeeded();
        fz_page *loaded = nullptr;
        fz_rect bounds = fz_empty_rect;
        bool failed = false;
        fz_var(loaded);
        fz_try(m_ctx) {
            loaded = fz_load_page(m_ctx, m_doc, page);
            bounds = fz_bound_page(m_ctx, loaded);
        }
        fz_always(m_ctx) {
            fz_drop_page(m_ctx, loaded);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed)
            throwCaught(QStringLiteral("could not read page size"));
        return {double(bounds.x1 - bounds.x0), double(bounds.y1 - bounds.y0)};
    }

    DisplayImage renderForDisplay(int page, double scale) override
    {
        layoutIfNeeded();
        const fz_matrix matrix = fz_scale(float(scale), float(scale));
        fz_page *loaded = nullptr;
        fz_pixmap *pixmap = nullptr;
        fz_device *device = nullptr;
        bool failed = false;
        fz_var(loaded);
        fz_var(pixmap);
        fz_var(device);
        fz_try(m_ctx) {
            loaded = fz_load_page(m_ctx, m_doc, page);
            const fz_irect box = fz_round_rect(fz_transform_rect(fz_bound_page(m_ctx, loaded), matrix));
            pixmap = fz_new_pixmap_with_bbox(m_ctx, fz_device_bgr(m_ctx), box, nullptr, 1);
            fz_clear_pixmap_with_value(m_ctx, pixmap, 0xFF);
            device = fz_new_draw_device(m_ctx, matrix, pixmap);
            fz_run_page(m_ctx, loaded, device, fz_identity, nullptr);
            fz_close_device(m_ctx, device);
        }
        fz_always(m_ctx) {
            fz_drop_device(m_ctx, device);
            fz_drop_page(m_ctx, loaded);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed) {
            fz_drop_pixmap(m_ctx, pixmap);
            throwCaught(QStringLiteral("could not render page"));
        }

        DisplayImage image;
        image.width = fz_pixmap_width(m_ctx, pixmap);
        image.height = fz_pixmap_height(m_ctx, pixmap);
        image.stride = fz_pixmap_stride(m_ctx, pixmap);
        image.pixels = QByteArray(reinterpret_cast<const char *>(fz_pixmap_samples(m_ctx, pixmap)),
                                  qsizetype(image.stride) * image.height);
        fz_drop_pixmap(m_ctx, pixmap);
        return image;
    }

    void setLayout(double width, double height, double fontSize) override
    {
        forgetSelectionText();
        m_layoutWidth = float(width);
        m_layoutHeight = float(height);
        m_layoutFontSize = float(fontSize);
        m_laidOut = false;
        layoutIfNeeded();
    }

    qint64 bookmarkForPage(int page) override
    {
        layoutIfNeeded();
        fz_bookmark mark = page;
        fz_try(m_ctx) {
            mark = fz_make_bookmark(m_ctx, m_doc, fz_location_from_page_number(m_ctx, m_doc, page));
        }
        fz_catch(m_ctx) {
            mark = page;
        }
        return qint64(mark);
    }

    int pageForBookmark(qint64 bookmark) override
    {
        layoutIfNeeded();
        int page = 0;
        const fz_bookmark mark = fz_bookmark(bookmark);
        fz_try(m_ctx) {
            page = fz_page_number_from_location(m_ctx, m_doc, fz_lookup_bookmark(m_ctx, m_doc, mark));
        }
        fz_catch(m_ctx) {
            page = 0;
        }
        return page;
    }

    QString placeForPage(int page) override
    {
        if (!isReflowable())
            return Document::placeForPage(page);
        layoutIfNeeded();
        fz_location location = fz_make_location(0, 0);
        int chapterPages = 1;
        fz_try(m_ctx) {
            location = fz_location_from_page_number(m_ctx, m_doc, page);
            chapterPages = fz_count_chapter_pages(m_ctx, m_doc, location.chapter);
        }
        fz_catch(m_ctx) {
            chapterPages = 1;
        }
        return QStringLiteral("%1:%2").arg(location.chapter).arg(double(location.page) / qMax(1, chapterPages), 0, 'f', 5);
    }

    int pageForPlace(const QString &place) override
    {
        if (!isReflowable())
            return Document::pageForPlace(place);
        layoutIfNeeded();
        const int chapter = place.section(u':', 0, 0).toInt();
        const double progress = place.section(u':', 1, 1).toDouble();
        int page = 0;
        fz_try(m_ctx) {
            const int chapters = fz_count_chapters(m_ctx, m_doc);
            const int target = std::clamp(chapter, 0, qMax(0, chapters - 1));
            const int chapterPages = fz_count_chapter_pages(m_ctx, m_doc, target);
            const int inChapter = std::clamp(int(progress * chapterPages), 0, qMax(0, chapterPages - 1));
            page = fz_page_number_from_location(m_ctx, m_doc, fz_make_location(target, inChapter));
        }
        fz_catch(m_ctx) {
            page = 0;
        }
        return qMax(0, page);
    }

    QList<OutlineItem> outline() override
    {
        layoutIfNeeded();
        fz_outline *root = nullptr;
        fz_var(root);
        fz_try(m_ctx) {
            root = fz_load_outline(m_ctx, m_doc);
        }
        fz_catch(m_ctx) {
            root = nullptr;
        }
        QList<OutlineItem> items;
        collectOutline(root, 0, items);
        fz_drop_outline(m_ctx, root);
        return items;
    }

    QList<PageLink> links(int page) override
    {
        layoutIfNeeded();
        fz_page *loaded = nullptr;
        fz_link *first = nullptr;
        fz_var(loaded);
        fz_var(first);
        fz_try(m_ctx) {
            loaded = fz_load_page(m_ctx, m_doc, page);
            first = fz_load_links(m_ctx, loaded);
        }
        fz_catch(m_ctx) {
            fz_drop_page(m_ctx, loaded);
            return {};
        }

        QList<PageLink> links;
        for (fz_link *link = first; link; link = link->next) {
            if (!link->uri)
                continue;
            PageLink item;
            item.rect = QRectF(link->rect.x0, link->rect.y0, link->rect.x1 - link->rect.x0, link->rect.y1 - link->rect.y0);
            bool external = false;
            float y = 0;
            int target = -1;
            const char *uri = link->uri;
            fz_try(m_ctx) {
                external = fz_is_external_link(m_ctx, uri) != 0;
                if (!external) {
                    float x = 0;
                    target = fz_page_number_from_location(m_ctx, m_doc, fz_resolve_link(m_ctx, m_doc, uri, &x, &y));
                }
            }
            fz_catch(m_ctx) {
                target = -1;
            }
            if (external)
                item.uri = QString::fromUtf8(link->uri);
            item.page = target;
            item.y = y;
            if (external || target >= 0)
                links << item;
        }
        fz_drop_link(m_ctx, first);
        fz_drop_page(m_ctx, loaded);
        return links;
    }

    QList<QRectF> search(int page, const QString &query) override
    {
        layoutIfNeeded();
        const QByteArray needle = query.toUtf8();
        const char *needleText = needle.constData();
        constexpr int kMaxHits = 512;
        static fz_quad quads[kMaxHits];
        int count = 0;
        fz_try(m_ctx) {
            count = fz_search_page_number(m_ctx, m_doc, page, needleText, nullptr, quads, kMaxHits);
        }
        fz_catch(m_ctx) {
            count = 0;
        }
        QList<QRectF> rects;
        for (int i = 0; i < count; ++i)
            rects << rectFrom(quads[i]);
        return rects;
    }

    TextSelection selectText(int page, const QPointF &a, const QPointF &b) override
    {
        layoutIfNeeded();
        if (page != m_selectionPage) {
            forgetSelectionText();
            fz_try(m_ctx) {
                m_selectionText = fz_new_stext_page_from_page_number(m_ctx, m_doc, page, nullptr);
                m_selectionPage = page;
            }
            fz_catch(m_ctx) {
                m_selectionText = nullptr;
            }
        }
        if (!m_selectionText)
            return {};

        constexpr int kMaxQuads = 4096;
        static fz_quad quads[kMaxQuads];
        const fz_point from{float(a.x()), float(a.y())};
        const fz_point to{float(b.x()), float(b.y())};
        int count = 0;
        char *copied = nullptr;
        fz_var(copied);
        fz_try(m_ctx) {
            count = fz_highlight_selection(m_ctx, m_selectionText, from, to, quads, kMaxQuads);
            copied = fz_copy_selection(m_ctx, m_selectionText, from, to, 0);
        }
        fz_catch(m_ctx) {
            count = 0;
        }

        TextSelection selection;
        for (int i = 0; i < count; ++i)
            selection.rects << rectFrom(quads[i]);
        if (copied) {
            selection.text = QString::fromUtf8(copied);
            fz_free(m_ctx, copied);
        }
        return selection;
    }

    QString largestTextOnFirstPage() override
    {
        layoutIfNeeded();
        fz_stext_page *text = nullptr;
        fz_var(text);
        fz_try(m_ctx) {
            text = fz_new_stext_page_from_page_number(m_ctx, m_doc, 0, nullptr);
        }
        fz_catch(m_ctx) {
            return {};
        }

        struct Line {
            QString text;
            float size;
            int block;
        };
        std::vector<Line> lines;
        int blockIndex = 0;
        for (fz_stext_block *block = text->first_block; block; block = block->next, ++blockIndex) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                QString content;
                float total = 0;
                int count = 0;
                for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
                    content += QString::fromUcs4(reinterpret_cast<const char32_t *>(&ch->c), 1);
                    total += ch->size;
                    ++count;
                }
                content = content.simplified();
                const auto letters = std::count_if(content.begin(), content.end(), [](QChar c) { return c.isLetter(); });
                if (count > 0 && letters >= 3)
                    lines.push_back({content, total / float(count), blockIndex});
            }
        }
        fz_drop_stext_page(m_ctx, text);

        // Group neighbouring lines of the same size, since a title can wrap, even across blocks.
        struct Run {
            QString text;
            float size;
        };
        std::vector<Run> runs;
        for (const Line &line : lines) {
            if (!runs.empty() && std::abs(runs.back().size - line.size) <= 0.5f)
                runs.back().text += QStringLiteral(" ") + line.text;
            else
                runs.push_back({line.text, line.size});
        }

        // The largest run is often a journal's masthead or logo, such as "Masthead" or "RESEARCH".
        // So prefer the largest run that's long enough to be a title.
        const Run *best = nullptr;
        const Run *largest = nullptr;
        for (const Run &run : runs) {
            if (!largest || run.size > largest->size)
                largest = &run;
            const bool titleLength = run.text.count(u' ') >= 2 && run.text.size() >= 15 && run.text.size() <= 300;
            if (titleLength && (!best || run.size > best->size))
                best = &run;
        }
        if (best)
            return best->text;
        return largest ? largest->text : QString();
    }

    QMap<QString, QString> metadata() override
    {
        QMap<QString, QString> values;
        for (const char *key : {"format", "encryption", "info:Title", "info:Author", "info:Subject",
                                "info:Keywords", "info:Creator", "info:Producer"}) {
            char buffer[1024] = {};
            int length = -1;
            fz_try(m_ctx) {
                length = fz_lookup_metadata(m_ctx, m_doc, key, buffer, sizeof buffer);
            }
            fz_catch(m_ctx) {
                length = -1;
            }
            if (length > 0)
                values.insert(QString::fromLatin1(key), QString::fromUtf8(buffer).trimmed());
        }
        return values;
    }

    QByteArray xmpPacket() override
    {
        pdf_document *pdf = pdf_document_from_fz_document(m_ctx, m_doc);
        if (!pdf)
            return {};
        fz_buffer *buffer = nullptr;
        fz_var(buffer);
        fz_try(m_ctx) {
            pdf_obj *stream = pdf_dict_getp(m_ctx, pdf_trailer(m_ctx, pdf), "Root/Metadata");
            if (stream)
                buffer = pdf_load_stream(m_ctx, stream);
        }
        fz_catch(m_ctx) {
            return {};
        }
        return takeBuffer(m_ctx, buffer);
    }

    QByteArray epubPackageDocument() override
    {
        fz_archive *archive = nullptr;
        fz_buffer *container = nullptr;
        fz_var(archive);
        fz_var(container);
        fz_try(m_ctx) {
            archive = fz_open_archive(m_ctx, m_path.constData());
            container = fz_try_read_archive_entry(m_ctx, archive, "META-INF/container.xml");
        }
        fz_catch(m_ctx) {
            fz_drop_archive(m_ctx, archive);
            return {};
        }

        const QByteArray containerXml = takeBuffer(m_ctx, container);
        const qsizetype attribute = containerXml.indexOf("full-path=\"");
        if (attribute < 0) {
            fz_drop_archive(m_ctx, archive);
            return {};
        }
        const qsizetype start = attribute + 11;
        const QByteArray packagePath = containerXml.mid(start, containerXml.indexOf('"', start) - start);
        const char *packageName = packagePath.constData();

        fz_buffer *package = nullptr;
        fz_var(package);
        fz_try(m_ctx) {
            package = fz_try_read_archive_entry(m_ctx, archive, packageName);
        }
        fz_always(m_ctx) {
            fz_drop_archive(m_ctx, archive);
        }
        fz_catch(m_ctx) {
            return {};
        }
        return takeBuffer(m_ctx, package);
    }

private:
    static QRectF rectFrom(const fz_quad &quad)
    {
        const fz_rect rect = fz_rect_from_quad(quad);
        return QRectF(rect.x0, rect.y0, rect.x1 - rect.x0, rect.y1 - rect.y0);
    }

    void forgetSelectionText()
    {
        fz_drop_stext_page(m_ctx, m_selectionText);
        m_selectionText = nullptr;
        m_selectionPage = -1;
    }

    void collectOutline(fz_outline *item, int depth, QList<OutlineItem> &items)
    {
        for (; item; item = item->next) {
            OutlineItem entry;
            entry.title = QString::fromUtf8(item->title).simplified();
            entry.depth = depth;
            int page = -1;
            float y = item->y;
            const fz_location location = item->page;
            const char *uri = item->uri;
            // EPUBs leave the location out, and only give a link to resolve.
            fz_try(m_ctx) {
                if (location.chapter >= 0) {
                    page = fz_page_number_from_location(m_ctx, m_doc, location);
                } else if (uri && !fz_is_external_link(m_ctx, uri)) {
                    float x = 0;
                    page = fz_page_number_from_location(m_ctx, m_doc, fz_resolve_link(m_ctx, m_doc, uri, &x, &y));
                }
            }
            fz_catch(m_ctx) {
                page = -1;
            }
            entry.page = page;
            entry.y = std::isnan(y) ? 0 : y;
            items << entry;
            collectOutline(item->down, depth + 1, items);
        }
    }

    // Reflowable formats have no pages until they're laid out at some size.
    void layoutIfNeeded()
    {
        if (m_laidOut)
            return;
        m_laidOut = true;
        if (!isReflowable())
            return;
        fz_try(m_ctx) {
            fz_layout_document(m_ctx, m_doc, m_layoutWidth, m_layoutHeight, m_layoutFontSize);
        }
        fz_catch(m_ctx) {
            // Page counts and rendering will report the problem if it matters.
        }
    }

    fz_context *m_ctx;
    QByteArray m_path;
    fz_document *m_doc = nullptr;
    bool m_laidOut = false;
    float m_layoutWidth = 595;
    float m_layoutHeight = 842;
    float m_layoutFontSize = 11;
    fz_stext_page *m_selectionText = nullptr;
    int m_selectionPage = -1;
};

} // namespace

std::unique_ptr<Document> openMuPdfDocument(fz_context *ctx, const QString &path)
{
    return std::make_unique<MuPdfDocument>(ctx, path);
}

} // namespace papertrail::engine
