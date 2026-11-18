// A PDF copy of a book in another format, for reading where Papertrail isn't. A reflowable book is laid out afresh
// and each page run into a new PDF, so its text stays text with its fonts embedded. A DjVu's pages become pictures,
// with its words over them as text that can't be seen, so the copy can still be searched and copied from.
//
// MuPDF reports errors by jumping out of fz_try, so nothing inside one has a destructor or throws: everything a
// block needs is made before it, and a failure is noted and thrown once the block is over.

#include "engine/Backends.h"
#include "engine/Document.h"
#include "engine/Ocr.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <mupdf/ucdn.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace papertrail::engine {

namespace {

constexpr int kDeepestOutline = 16;    // A file's outline can nest without end, so only a reasonable depth is kept.
constexpr double kLargestSide = 14400; // In points: the largest page a PDF can have.
constexpr int kJpegQuality = 85;

[[noreturn]] void fail(const char *message)
{
    throw EngineError(message);
}

// One item of an outline to write, in order, with how deep it is.
struct OutlineEntry {
    const char *title = nullptr;
    int page = -1;
    int depth = 0;
};

// Writes the items from `*index` that are at least `minDepth` deep, each with those deeper after it beneath it.
// Call it in fz_try.
void insertOutline(fz_context *ctx, fz_outline_iterator *iterator, const OutlineEntry *items, int count, int *index, int minDepth)
{
    char uri[48];
    while (*index < count && items[*index].depth >= minDepth) {
        const OutlineEntry &entry = items[*index];
        fz_outline_item item = {};
        item.title = const_cast<char *>(entry.title ? entry.title : "");
        item.uri = nullptr;
        if (entry.page >= 0) {
            fz_snprintf(uri, sizeof uri, "#page=%d", entry.page + 1);
            item.uri = uri;
        }
        fz_outline_iterator_insert(ctx, iterator, &item);
        ++*index;
        if (*index >= count || items[*index].depth <= entry.depth)
            continue;
        bool inside = false;
        if (fz_outline_iterator_prev(ctx, iterator) >= 0) {
            if (fz_outline_iterator_down(ctx, iterator) >= 0) {
                inside = true;
                insertOutline(ctx, iterator, items, count, index, entry.depth + 1);
                fz_outline_iterator_up(ctx, iterator);
            }
            fz_outline_iterator_next(ctx, iterator);
        }
        // Those beneath it that couldn't be put there are left out.
        while (!inside && *index < count && items[*index].depth > entry.depth)
            ++*index;
    }
}

// Writes an outline into a new PDF. Call it in fz_try.
void writeOutline(fz_context *ctx, pdf_document *pdf, const OutlineEntry *items, int count)
{
    if (count <= 0)
        return;
    fz_outline_iterator *iterator = fz_new_outline_iterator(ctx, reinterpret_cast<fz_document *>(pdf));
    fz_try(ctx) {
        int index = 0;
        while (index < count) {
            const int before = index;
            insertOutline(ctx, iterator, items, count, &index, 0);
            if (index == before)
                ++index;
        }
    }
    fz_always(ctx) {
        fz_drop_outline_iterator(ctx, iterator);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// Only links a reader would follow anyway are kept, since a book's links come from its file.
bool isWebLink(const char *uri)
{
    const QString address = QString::fromUtf8(uri).trimmed();
    const QUrl url(address, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == QLatin1StringView("http") || scheme == QLatin1StringView("https")
                             || scheme == QLatin1StringView("mailto"));
}

// Adds a page to the end of a new PDF. Call it in fz_try.
void addPage(fz_context *ctx, pdf_document *pdf, fz_rect box, pdf_obj *resources, fz_buffer *contents)
{
    pdf_obj *page = pdf_add_page(ctx, pdf, box, 0, resources, contents);
    fz_try(ctx) {
        pdf_insert_page(ctx, pdf, -1, page);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, page);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// Adds an empty page, in place of one that couldn't be read. Returns false if even that failed.
bool addBlankPage(fz_context *ctx, pdf_document *pdf, fz_rect box)
{
    pdf_obj *resources = nullptr;
    fz_buffer *contents = nullptr;
    fz_device *device = nullptr;
    bool ok = true;
    fz_var(resources);
    fz_var(contents);
    fz_var(device);
    fz_try(ctx) {
        device = pdf_page_write(ctx, pdf, box, &resources, &contents);
        fz_close_device(ctx, device);
        addPage(ctx, pdf, box, resources, contents);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, device);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);
    }
    fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

// Keeps only the parts of fonts the copy uses, then saves it with its streams compressed.
void savePdf(fz_context *ctx, pdf_document *pdf, const QString &partPath)
{
    const QByteArray part = partPath.toUtf8(); // As files are opened, which MuPDF reads as UTF-8 everywhere.
    bool failed = false;
    fz_try(ctx) {
        // A whole font is far larger than what a book uses of it.
        pdf_subset_fonts(ctx, pdf, 0, nullptr);
        pdf_write_options options = pdf_default_write_options;
        options.do_garbage = 1;
        options.do_compress = 1;
        options.do_compress_images = 1;
        options.do_compress_fonts = 1;
        pdf_save_document(ctx, pdf, part.constData(), &options);
    }
    fz_catch(ctx) {
        failed = true;
    }
    if (failed)
        fail("the copy couldn't be saved");
}

// Counts an outline's items, down to the depth kept.
int countOutline(fz_outline *node, int depth)
{
    int count = 0;
    for (; node; node = node->next) {
        ++count;
        if (depth < kDeepestOutline)
            count += countOutline(node->down, depth + 1);
    }
    return count;
}

// Lists an outline's items in order, with where each leads in the laid-out book. Call it in fz_try.
void flattenOutline(fz_context *ctx, fz_document *doc, fz_outline *node, int depth, OutlineEntry *items, int count, int *index)
{
    for (; node && *index < count; node = node->next) {
        OutlineEntry &entry = items[(*index)++];
        entry.title = node->title;
        entry.depth = depth;
        entry.page = -1;
        if (node->uri) {
            fz_try(ctx) {
                entry.page = fz_page_number_from_location(ctx, doc, fz_resolve_link(ctx, doc, node->uri, nullptr, nullptr));
            }
            fz_catch(ctx) {
                entry.page = -1; // An item leading nowhere is still worth keeping, for its title.
            }
        }
        if (depth < kDeepestOutline)
            flattenOutline(ctx, doc, node->down, depth + 1, items, count, index);
    }
}

// Copies a page's links, once every page of the copy exists, since a link to a page has to find it. Call it in fz_try.
void copyLinks(fz_context *ctx, fz_document *doc, pdf_document *pdf, int pageNumber, int pages)
{
    fz_page *page = nullptr;
    fz_link *links = nullptr;
    pdf_page *target = nullptr;
    fz_var(page);
    fz_var(links);
    fz_var(target);
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, pageNumber);
        links = fz_load_links(ctx, page);
        if (links)
            target = pdf_load_page(ctx, pdf, pageNumber);
        char uri[48];
        for (fz_link *link = links; link; link = link->next) {
            if (!link->uri)
                continue;
            if (fz_is_external_link(ctx, link->uri)) {
                if (isWebLink(link->uri))
                    fz_drop_link(ctx, pdf_create_link(ctx, target, link->rect, link->uri));
                continue;
            }
            int to = -1;
            fz_try(ctx) {
                to = fz_page_number_from_location(ctx, doc, fz_resolve_link(ctx, doc, link->uri, nullptr, nullptr));
            }
            fz_catch(ctx) {
                to = -1;
            }
            if (to < 0 || to >= pages)
                continue;
            fz_snprintf(uri, sizeof uri, "#page=%d", to + 1);
            fz_drop_link(ctx, pdf_create_link(ctx, target, link->rect, uri));
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page *>(target));
        fz_drop_link(ctx, links);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// A page's size, kept within what a PDF allows.
fz_rect pageBox(double width, double height)
{
    double scale = 1.0;
    const double longest = std::max(width, height);
    if (longest > kLargestSide)
        scale = kLargestSide / longest;
    return fz_make_rect(0, 0, float(std::max(1.0, width * scale)), float(std::max(1.0, height * scale)));
}

// A word to write as text that can't be seen, with its box in points and its characters.
struct HiddenWord {
    QRectF rect;
    int first = 0; // Into the list of characters.
    int count = 0;
};

// Splits lines of text into words, each with the box around its characters.
void hiddenWordsOf(const QList<TextLine> &lines, std::vector<HiddenWord> &words, std::vector<int> &characters)
{
    constexpr qsizetype kLongestWord = 200;
    for (const TextLine &line : lines) {
        const qsizetype length = std::min(line.text.size(), line.chars.size());
        qsizetype i = 0;
        while (i < length) {
            if (line.text.at(i).isSpace()) {
                ++i;
                continue;
            }
            qsizetype end = i;
            QRectF box;
            while (end < length && !line.text.at(end).isSpace() && end - i < kLongestWord) {
                const QRectF part = line.chars.at(end);
                box = box.isNull() ? part : box.united(part);
                ++end;
            }
            const QList<uint> codes = line.text.mid(i, end - i).toUcs4();
            if (box.width() > 0 && box.height() > 0 && !codes.isEmpty()) {
                HiddenWord word;
                word.rect = box;
                word.first = int(characters.size());
                word.count = int(codes.size());
                for (const uint code : codes)
                    characters.push_back(int(code));
                words.push_back(word);
            }
            i = end;
        }
    }
}

// Draws words as text that can't be seen, each stretched to fill its box. Call it in fz_try.
void drawHiddenWords(fz_context *ctx, fz_device *device, fz_font *font, const HiddenWord *words, size_t count,
                     const int *characters, double scale)
{
    if (count == 0)
        return;
    const float ascender = fz_font_ascender(ctx, font);
    const float descender = fz_font_descender(ctx, font);
    const float height = ascender - descender > 0.1f ? ascender - descender : 1.0f;
    fz_text *text = fz_new_text(ctx);
    fz_try(ctx) {
        for (size_t w = 0; w < count; ++w) {
            const HiddenWord &word = words[w];
            const float top = float(word.rect.top() * scale);
            const float left = float(word.rect.left() * scale);
            const float width = float(word.rect.width() * scale);
            const float size = float(word.rect.height() * scale) / height;
            if (size <= 0.01f)
                continue;
            float advance = 0;
            for (int c = 0; c < word.count; ++c) {
                fz_font *used = font;
                const int glyph = fz_encode_character_with_fallback(ctx, font, characters[word.first + c], 0, FZ_LANG_UNSET, &used);
                advance += fz_advance_glyph(ctx, used, glyph, 0);
            }
            const float stretch = advance > 0.001f ? std::clamp(width / (advance * size), 0.02f, 50.0f) : 1.0f;
            const float baseline = top + ascender * size;
            float x = left;
            for (int c = 0; c < word.count; ++c) {
                const int unicode = characters[word.first + c];
                fz_font *used = font;
                const int glyph = fz_encode_character_with_fallback(ctx, font, unicode, 0, FZ_LANG_UNSET, &used);
                fz_show_glyph(ctx, text, used, fz_make_matrix(size * stretch, 0, 0, -size, x, baseline), glyph, unicode, 0, 0,
                              FZ_BIDI_LTR, FZ_LANG_UNSET);
                x += fz_advance_glyph(ctx, used, glyph, 0) * size * stretch;
            }
        }
        fz_ignore_text(ctx, device, text, fz_identity);
    }
    fz_always(ctx) {
        fz_drop_text(ctx, text);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

// Packs grey pixels into one bit each, 1 for black, as a bitonal page is.
QByteArray packBits(const DjvuPagePixels &page)
{
    const qsizetype stride = (qsizetype(page.width) + 7) / 8;
    QByteArray bits(stride * page.height, '\0');
    const auto *from = reinterpret_cast<const unsigned char *>(page.pixels.constData());
    auto *to = reinterpret_cast<unsigned char *>(bits.data());
    for (qsizetype y = 0; y < page.height; ++y) {
        for (qsizetype x = 0; x < page.width; ++x) {
            if (from[y * page.width + x] < 128)
                to[y * stride + x / 8] |= static_cast<unsigned char>(0x80 >> (x % 8));
        }
    }
    return bits;
}

// Turns colour pixels into grey ones if every pixel is a shade of grey, which a JPEG then keeps in a third the room.
void greyIfAllGrey(DjvuPagePixels &page)
{
    const qsizetype count = qsizetype(page.width) * page.height;
    const auto *rgb = reinterpret_cast<const unsigned char *>(page.pixels.constData());
    for (qsizetype i = 0; i < count; ++i) {
        if (rgb[i * 3] != rgb[i * 3 + 1] || rgb[i * 3] != rgb[i * 3 + 2])
            return;
    }
    QByteArray grey(count, '\0');
    for (qsizetype i = 0; i < count; ++i)
        grey[i] = char(rgb[i * 3]);
    page.pixels = grey;
    page.channels = 1;
}

PdfCopyResult writeMuPdfCopy(fz_context *ctx, const QString &path, const QString &partPath, const PdfCopyOptions &options,
                             const PdfCopyProgress &progress)
{
    const QByteArray file = path.toUtf8();
    // Margins on every page, as the book's own stylesheet is kept.
    const QByteArray css = QByteArrayLiteral("@page { margin: ") + QByteArray::number(std::clamp(options.margin, 0.0, 144.0), 'f', 1)
        + QByteArrayLiteral("pt !important; }");
    const float width = float(std::clamp(options.pageWidth, 144.0, kLargestSide));
    const float height = float(std::clamp(options.pageHeight, 144.0, kLargestSide));
    const float em = float(std::clamp(options.fontSize, 4.0, 72.0));

    PdfCopyResult result;
    fz_document *doc = nullptr;
    pdf_document *pdf = nullptr;
    bool locked = false;
    bool opened = false;
    int pages = 0;
    fz_var(doc);
    fz_var(pdf);
    fz_var(locked);
    fz_var(opened);
    fz_var(pages);
    fz_try(ctx) {
        doc = fz_open_document(ctx, file.constData());
        if (fz_needs_password(ctx, doc)) {
            locked = true;
            fz_throw(ctx, FZ_ERROR_GENERIC, "the file needs a password");
        }
        if (fz_is_document_reflowable(ctx, doc)) {
            fz_style_document(ctx, doc, 1, css.constData());
            fz_layout_document(ctx, doc, width, height, em);
        }
        pages = fz_count_pages(ctx, doc);
        pdf = pdf_create_document(ctx);
        opened = true;
    }
    fz_catch(ctx) {
        opened = false;
    }
    if (!opened) {
        if (pdf)
            pdf_drop_document(ctx, pdf);
        fz_drop_document(ctx, doc);
        fail(locked ? "the file needs a password" : "the book couldn't be opened");
    }
    // Everything past here ends by dropping these.
    const auto close = [&] {
        pdf_drop_document(ctx, pdf);
        fz_drop_document(ctx, doc);
    };
    if (pages <= 0) {
        close();
        fail("the book has no pages");
    }

    fz_rect last = fz_make_rect(0, 0, width, height);
    for (int i = 0; i < pages; ++i) {
        fz_page *page = nullptr;
        fz_device *device = nullptr;
        pdf_obj *resources = nullptr;
        fz_buffer *contents = nullptr;
        bool written = true;
        bool added = false;
        fz_var(page);
        fz_var(device);
        fz_var(resources);
        fz_var(contents);
        fz_var(added);
        fz_var(last);
        fz_try(ctx) {
            page = fz_load_page(ctx, doc, i);
            fz_rect box = fz_bound_page(ctx, page);
            if (fz_is_empty_rect(box))
                box = last;
            last = box;
            device = pdf_page_write(ctx, pdf, box, &resources, &contents);
            fz_run_page(ctx, page, device, fz_identity, nullptr);
            fz_close_device(ctx, device);
            addPage(ctx, pdf, box, resources, contents);
            added = true;
        }
        fz_always(ctx) {
            fz_drop_device(ctx, device);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            written = false;
        }
        // A page that can't be read is left blank, so the rest keep their numbers.
        if (!written) {
            if (!added && !addBlankPage(ctx, pdf, last)) {
                close();
                fail("the copy couldn't be written");
            }
            ++result.unreadablePages;
        }
        if (progress)
            progress(i + 1, pages);
    }

    // Links and the outline are a bonus: a book is still worth having without them.
    for (int i = 0; i < pages; ++i) {
        fz_try(ctx) {
            copyLinks(ctx, doc, pdf, i, pages);
        }
        fz_catch(ctx) {
        }
    }
    fz_outline *outline = nullptr;
    OutlineEntry *items = nullptr;
    fz_var(outline);
    fz_var(items);
    fz_try(ctx) {
        outline = fz_load_outline(ctx, doc);
        const int count = countOutline(outline, 0);
        if (count > 0) {
            items = static_cast<OutlineEntry *>(fz_calloc(ctx, size_t(count), sizeof(OutlineEntry)));
            int index = 0;
            flattenOutline(ctx, doc, outline, 0, items, count, &index);
            writeOutline(ctx, pdf, items, index);
        }
    }
    fz_always(ctx) {
        fz_free(ctx, items);
        fz_drop_outline(ctx, outline);
    }
    fz_catch(ctx) {
    }

    try {
        savePdf(ctx, pdf, partPath);
    } catch (const EngineError &) {
        close();
        throw;
    }
    close();
    result.pages = pages;
    return result;
}

PdfCopyResult writeDjvuCopy(fz_context *ctx, Document &pages, Document &text, const QString &partPath,
                            const PdfCopyOptions &options, const PdfCopyProgress &progress)
{
    const int count = pages.pageCount();
    if (count <= 0)
        fail("the book has no pages");

    PdfCopyResult result;
    pdf_document *pdf = nullptr;
    fz_font *font = nullptr;
    bool started = true;
    fz_var(pdf);
    fz_var(font);
    fz_try(ctx) {
        pdf = pdf_create_document(ctx);
        if (options.searchable) {
            // A font for the words that can't be seen. Characters it lacks come from MuPDF's others for their script.
            int length = 0;
            int index = 0;
            const unsigned char *data = fz_lookup_noto_font(ctx, UCDN_SCRIPT_LATIN, FZ_LANG_UNSET, &length, &index);
            font = data ? fz_new_font_from_memory(ctx, "NotoSans", data, length, index, 0) : fz_new_base14_font(ctx, "Helvetica");
        }
    }
    fz_catch(ctx) {
        started = false;
    }
    const auto close = [&] {
        fz_drop_font(ctx, font);
        pdf_drop_document(ctx, pdf);
    };
    if (!started) {
        close();
        fail("the copy couldn't be started");
    }

    fz_rect last = fz_make_rect(0, 0, 612, 792);
    for (int i = 0; i < count; ++i) {
        // Everything the page needs, made before the block that writes it.
        DjvuPagePixels pixels;
        bool readable = true;
        try {
            pixels = renderDjvuPage(pages, i, options.dpi);
            if (pixels.bitonal)
                pixels.pixels = packBits(pixels);
            else
                greyIfAllGrey(pixels);
        } catch (const EngineError &) {
            readable = false;
        }
        std::vector<HiddenWord> words;
        std::vector<int> characters;
        if (readable && options.searchable) {
            try {
                hiddenWordsOf(text.textLines(i), words, characters);
            } catch (const EngineError &) {
                // A page whose words can't be read is still worth its picture.
            }
        }

        bool written = readable;
        bool added = false;
        if (readable) {
            const fz_rect box = pageBox(pixels.size.width(), pixels.size.height());
            const double scale = pixels.size.width() > 0 ? double(box.x1) / pixels.size.width() : 1.0;
            last = box;
            fz_buffer *data = nullptr;
            fz_buffer *jpeg = nullptr;
            fz_compressed_buffer *compressed = nullptr;
            fz_pixmap *pixmap = nullptr;
            fz_image *image = nullptr;
            fz_device *device = nullptr;
            pdf_obj *resources = nullptr;
            fz_buffer *contents = nullptr;
            fz_var(added);
            fz_var(data);
            fz_var(jpeg);
            fz_var(compressed);
            fz_var(pixmap);
            fz_var(image);
            fz_var(device);
            fz_var(resources);
            fz_var(contents);
            fz_try(ctx) {
                if (pixels.bitonal) {
                    // One bit a pixel, which the writer compresses with Flate: smaller than CCITT G4 here.
                    static const float decode[2] = {1, 0};
                    data = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char *>(pixels.pixels.constData()),
                                                          size_t(pixels.pixels.size()));
                    compressed = fz_new_compressed_buffer(ctx);
                    compressed->buffer = data;
                    data = nullptr;
                    compressed->params.type = FZ_IMAGE_RAW;
                    // The image takes the buffer, even if it fails.
                    fz_compressed_buffer *handed = compressed;
                    compressed = nullptr;
                    image = fz_new_image_from_compressed_buffer(ctx, pixels.width, pixels.height, 1, fz_device_gray(ctx), pixels.dpi,
                                                                pixels.dpi, 0, 0, decode, nullptr, handed, nullptr);
                } else {
                    // The pixmap points at the pixels, which outlive it and the JPEG made from it.
                    pixmap = fz_new_pixmap_with_data(ctx, pixels.channels == 1 ? fz_device_gray(ctx) : fz_device_rgb(ctx), pixels.width,
                                                     pixels.height, nullptr, 0, pixels.width * pixels.channels,
                                                     reinterpret_cast<unsigned char *>(pixels.pixels.data()));
                    pixmap->xres = pixmap->yres = pixels.dpi;
                    jpeg = fz_new_buffer_from_pixmap_as_jpeg(ctx, pixmap, fz_default_color_params, kJpegQuality, 0);
                    image = fz_new_image_from_buffer(ctx, jpeg);
                }
                device = pdf_page_write(ctx, pdf, box, &resources, &contents);
                fz_fill_image(ctx, device, image, fz_make_matrix(box.x1, 0, 0, box.y1, 0, 0), 1, fz_default_color_params);
                if (font)
                    drawHiddenWords(ctx, device, font, words.data(), words.size(), characters.data(), scale);
                fz_close_device(ctx, device);
                addPage(ctx, pdf, box, resources, contents);
                added = true;
            }
            fz_always(ctx) {
                fz_drop_device(ctx, device);
                pdf_drop_obj(ctx, resources);
                fz_drop_buffer(ctx, contents);
                fz_drop_image(ctx, image);
                fz_drop_pixmap(ctx, pixmap);
                fz_drop_buffer(ctx, jpeg);
                fz_drop_compressed_buffer(ctx, compressed);
                fz_drop_buffer(ctx, data);
            }
            fz_catch(ctx) {
                written = false;
            }
        }
        if (!written) {
            if (!added && !addBlankPage(ctx, pdf, last)) {
                close();
                fail("the copy couldn't be written");
            }
            ++result.unreadablePages;
        }
        if (progress)
            progress(i + 1, count);
    }

    // The outline and links are a bonus: a book is still worth having without them.
    std::vector<QByteArray> titles;
    std::vector<OutlineEntry> items;
    std::vector<QByteArray> addresses;
    std::vector<int> linkPages;
    std::vector<fz_rect> linkRects;
    try {
        const QList<OutlineItem> outline = pages.outline();
        const qsizetype kept = std::min<qsizetype>(outline.size(), 100000);
        for (qsizetype i = 0; i < kept; ++i)
            titles.push_back(outline.at(i).title.toUtf8());
        for (qsizetype i = 0; i < kept; ++i) {
            const OutlineItem &item = outline.at(i);
            OutlineEntry entry;
            entry.title = titles[size_t(i)].constData();
            entry.page = item.page >= 0 && item.page < count ? item.page : -1;
            entry.depth = std::clamp(item.depth, 0, kDeepestOutline);
            items.push_back(entry);
        }
        for (int i = 0; i < count; ++i) {
            for (const PageLink &link : pages.links(i)) {
                const QByteArray address = link.page >= 0 ? QByteArray("#page=") + QByteArray::number(link.page + 1) : link.uri.toUtf8();
                if (link.page < 0 && !isWebLink(address.constData()))
                    continue;
                if (link.page >= count)
                    continue;
                addresses.push_back(address);
                linkPages.push_back(i);
                linkRects.push_back(fz_make_rect(float(link.rect.left()), float(link.rect.top()), float(link.rect.right()),
                                                 float(link.rect.bottom())));
            }
        }
    } catch (const EngineError &) {
    }
    fz_try(ctx) {
        writeOutline(ctx, pdf, items.data(), int(items.size()));
    }
    fz_catch(ctx) {
    }
    for (size_t i = 0; i < addresses.size(); ++i) {
        pdf_page *page = nullptr;
        fz_var(page);
        fz_try(ctx) {
            page = pdf_load_page(ctx, pdf, linkPages[i]);
            fz_drop_link(ctx, pdf_create_link(ctx, page, linkRects[i], addresses[i].constData()));
        }
        fz_always(ctx) {
            fz_drop_page(ctx, reinterpret_cast<fz_page *>(page));
        }
        fz_catch(ctx) {
        }
    }

    try {
        savePdf(ctx, pdf, partPath);
    } catch (const EngineError &) {
        close();
        throw;
    }
    close();
    result.pages = count;
    return result;
}

} // namespace

PdfCopyResult writePdfCopy(fz_context *ctx, DjvuBackend &djvu, const QString &path, const QString &outPath,
                           const PdfCopyOptions &options, const PdfCopyProgress &progress)
{
    if (outPath.isEmpty())
        fail("there's nowhere to write the copy");
    const QFileInfo source(path);
    const QFileInfo target(outPath);
    // The copy never replaces what it's made from, by any name for it.
    if (QDir::cleanPath(target.absoluteFilePath()).compare(QDir::cleanPath(source.absoluteFilePath()), Qt::CaseInsensitive) == 0
        || (target.exists() && target.canonicalFilePath() == source.canonicalFilePath())) {
        fail("the copy can't replace the original");
    }
    if (!source.isFile())
        fail("the book can't be found");

    const QString part = outPath + QStringLiteral(".part");
    QFile::remove(part);
    PdfCopyResult result;
    try {
        const QString suffix = source.suffix().toLower();
        if (suffix == QLatin1StringView("djvu") || suffix == QLatin1StringView("djv")) {
            std::unique_ptr<Document> own = djvu.open(path);
            if (options.recognisedWords) {
                RecognisedTextDocument withWords(std::move(own), options.recognisedWords);
                withWords.setPreferRecognised(options.preferRecognised);
                result = writeDjvuCopy(ctx, withWords.inner(), withWords, part, options, progress);
            } else {
                result = writeDjvuCopy(ctx, *own, *own, part, options, progress);
            }
        } else {
            result = writeMuPdfCopy(ctx, path, part, options, progress);
        }
    } catch (const EngineError &) {
        QFile::remove(part);
        throw;
    }
    QFile::remove(outPath);
    if (!QFile::rename(part, outPath)) {
        QFile::remove(part);
        fail("the copy couldn't be saved there");
    }
    return result;
}

} // namespace papertrail::engine
