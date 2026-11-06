// Answers one question for each file: can the document engines really open it,
// render its first page and return its text? Used for the M0 format test.
//
//   format-probe [--marker TEXT] FILE...
//
// Exits non-zero if any file fails to open or render.

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

#include <mupdf/fitz.h>
#include <libdjvu/ddjvuapi.h>

#include <vector>

namespace {

struct Result {
    bool ok = false;
    QString error;
    int pages = 0;
    bool reflowable = false;
    int width = 0;
    int height = 0;
    double ink = 0.0; // Fraction of first-page pixels that aren't near-white.
    QByteArray text;
};

double inkFraction(const unsigned char *samples, int w, int h, int stride, int n)
{
    if (w <= 0 || h <= 0)
        return 0.0;
    qint64 dark = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char *row = samples + qint64(y) * stride;
        for (int x = 0; x < w; ++x) {
            const unsigned char *px = row + qint64(x) * n;
            if (px[0] < 200 || px[1] < 200 || px[2] < 200)
                ++dark;
        }
    }
    return double(dark) / (double(w) * h);
}

Result probeMuPdf(fz_context *ctx, const char *path)
{
    constexpr int textPages = 5;

    Result r;
    fz_document *doc = nullptr;
    fz_pixmap *pix = nullptr;
    fz_buffer *buf = nullptr;
    fz_var(doc);
    fz_var(pix);
    fz_var(buf);

    fz_try(ctx) {
        doc = fz_open_document(ctx, path);
        r.reflowable = fz_is_document_reflowable(ctx, doc);
        if (r.reflowable)
            fz_layout_document(ctx, doc, 595, 842, 11);
        r.pages = fz_count_pages(ctx, doc);

        pix = fz_new_pixmap_from_page_number(ctx, doc, 0, fz_identity, fz_device_rgb(ctx), 0);
        r.width = fz_pixmap_width(ctx, pix);
        r.height = fz_pixmap_height(ctx, pix);
        r.ink = inkFraction(fz_pixmap_samples(ctx, pix), r.width, r.height,
                            fz_pixmap_stride(ctx, pix), fz_pixmap_components(ctx, pix));

        for (int i = 0; i < r.pages && i < textPages; ++i) {
            buf = fz_new_buffer_from_page_number(ctx, doc, i, nullptr);
            unsigned char *data = nullptr;
            const size_t len = fz_buffer_storage(ctx, buf, &data);
            r.text.append(reinterpret_cast<const char *>(data), qsizetype(len));
            fz_drop_buffer(ctx, buf);
            buf = nullptr;
        }
        r.ok = true;
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_drop_pixmap(ctx, pix);
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        r.ok = false;
        r.error = QString::fromUtf8(fz_caught_message(ctx));
    }
    return r;
}

void drainDjvuMessages(ddjvu_context_t *ctx)
{
    ddjvu_message_wait(ctx);
    while (ddjvu_message_peek(ctx))
        ddjvu_message_pop(ctx);
}

// Text isn't probed here: reading DjVu text layers is M5 engine work.
Result probeDjvu(const char *path)
{
    Result r;
    ddjvu_context_t *ctx = ddjvu_context_create("format-probe");
    ddjvu_document_t *doc = ddjvu_document_create_by_filename_utf8(ctx, path, 1);
    if (!doc) {
        r.error = QStringLiteral("could not open document");
        ddjvu_context_release(ctx);
        return r;
    }

    while (ddjvu_job_status(ddjvu_document_job(doc)) < DDJVU_JOB_OK)
        drainDjvuMessages(ctx);

    if (ddjvu_job_status(ddjvu_document_job(doc)) >= DDJVU_JOB_FAILED) {
        r.error = QStringLiteral("document decoding failed");
    } else {
        r.pages = ddjvu_document_get_pagenum(doc);
        ddjvu_page_t *page = ddjvu_page_create_by_pageno(doc, 0);
        while (page && ddjvu_job_status(ddjvu_page_job(page)) < DDJVU_JOB_OK)
            drainDjvuMessages(ctx);

        if (!page || ddjvu_job_status(ddjvu_page_job(page)) >= DDJVU_JOB_FAILED) {
            r.error = QStringLiteral("page decoding failed");
        } else {
            r.width = ddjvu_page_get_width(page);
            r.height = ddjvu_page_get_height(page);

            ddjvu_format_t *format = ddjvu_format_create(DDJVU_FORMAT_RGB24, 0, nullptr);
            ddjvu_format_set_row_order(format, 1);
            const int stride = r.width * 3;
            std::vector<char> pixels(size_t(stride) * r.height);
            ddjvu_rect_t rect{0, 0, unsigned(r.width), unsigned(r.height)};
            if (ddjvu_page_render(page, DDJVU_RENDER_COLOR, &rect, &rect, format,
                                  stride, pixels.data())) {
                r.ink = inkFraction(reinterpret_cast<const unsigned char *>(pixels.data()),
                                    r.width, r.height, stride, 3);
                r.ok = true;
            } else {
                r.error = QStringLiteral("page render failed");
            }
            ddjvu_format_release(format);
        }
        if (page)
            ddjvu_page_release(page);
    }

    ddjvu_document_release(doc);
    ddjvu_context_release(ctx);
    return r;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QStringList files = QCoreApplication::arguments().mid(1);
    QByteArray marker;
    if (files.size() >= 2 && files.first() == QStringLiteral("--marker")) {
        marker = files.at(1).toUtf8();
        files = files.mid(2);
    }

    fz_context *ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);

    QTextStream out(stdout);
    int failures = 0;
    for (const QString &file : files) {
        const QByteArray path = file.toUtf8();
        const QString suffix = QFileInfo(file).suffix().toLower();
        const bool isDjvu = suffix == QStringLiteral("djvu") || suffix == QStringLiteral("djv");
        const Result r = isDjvu ? probeDjvu(path.constData()) : probeMuPdf(ctx, path.constData());

        out << QFileInfo(file).fileName() << ": ";
        if (!r.ok) {
            ++failures;
            out << "FAILED: " << r.error << '\n';
            continue;
        }
        out << "ok pages=" << r.pages
            << " reflowable=" << (r.reflowable ? "yes" : "no")
            << " size=" << r.width << 'x' << r.height
            << " ink=" << QString::number(r.ink, 'f', 3)
            << " text=" << r.text.size();
        if (!marker.isEmpty())
            out << " marker=" << (r.text.contains(marker) ? "found" : "absent");
        out << '\n';
    }

    fz_drop_context(ctx);
    return failures == 0 ? 0 : 1;
}
