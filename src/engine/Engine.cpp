#include "engine/Backends.h"
#include "engine/Document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <mupdf/fitz.h>

namespace papertrail::engine {

void Document::saveMarkedCopy(const QString &, const QList<MarkToWrite> &)
{
    throw EngineError("only a PDF can have its marks written into a copy");
}

double RenderedPage::inkFraction() const
{
    const qsizetype pixels = qsizetype(width) * height;
    if (pixels <= 0 || rgb.size() < pixels * 3)
        return 0.0;
    const auto *data = reinterpret_cast<const unsigned char *>(rgb.constData());
    qsizetype dark = 0;
    for (qsizetype i = 0; i < pixels; ++i) {
        if (data[i * 3] < 200 || data[i * 3 + 1] < 200 || data[i * 3 + 2] < 200)
            ++dark;
    }
    return double(dark) / double(pixels);
}

DisplayImage Document::renderForDisplay(int page, double scale)
{
    const QSizeF size = pageSize(page);
    const RenderedPage rendered = renderPage(page, qMax(1, int(qMax(size.width(), size.height()) * scale)));

    DisplayImage image;
    image.width = rendered.width;
    image.height = rendered.height;
    image.stride = rendered.width * 4;
    image.pixels.resize(qsizetype(image.stride) * image.height);
    const auto *from = reinterpret_cast<const unsigned char *>(rendered.rgb.constData());
    auto *to = reinterpret_cast<unsigned char *>(image.pixels.data());
    const qsizetype pixels = qsizetype(image.width) * image.height;
    for (qsizetype i = 0; i < pixels; ++i) {
        to[i * 4] = from[i * 3 + 2];
        to[i * 4 + 1] = from[i * 3 + 1];
        to[i * 4 + 2] = from[i * 3];
        to[i * 4 + 3] = 255;
    }
    return image;
}

void Document::setLayout(double, double, double) {}

QString Document::textInRect(int page, const QRectF &area)
{
    QStringList lines;
    for (const TextLine &line : textLines(page)) {
        if (!line.rect.intersects(area))
            continue;
        QString inside;
        for (qsizetype i = 0; i < line.text.size() && i < line.chars.size(); ++i) {
            if (area.contains(line.chars.at(i).center()))
                inside += line.text.at(i);
        }
        inside = inside.simplified();
        if (!inside.isEmpty())
            lines << inside;
    }
    return lines.join(u'\n');
}

qint64 Document::bookmarkForPage(int page)
{
    return page;
}

int Document::pageForBookmark(qint64 bookmark)
{
    return int(bookmark);
}

QString Document::placeForPage(int page)
{
    return QString::number(page);
}

int Document::pageForPlace(const QString &place)
{
    return place.toInt();
}

namespace {

void ignoreMessage(void *, const char *) {}

} // namespace

struct Engine::Private {
    fz_context *ctx = nullptr;
    DjvuBackend djvu;
};

Engine::Engine()
    : d(std::make_unique<Private>())
{
    d->ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!d->ctx)
        throw EngineError("could not start the document engine");

    // Bad files produce a stream of warnings, and the import reports problems in its own way.
    fz_set_warning_callback(d->ctx, ignoreMessage, nullptr);
    fz_set_error_callback(d->ctx, ignoreMessage, nullptr);
    fz_register_document_handlers(d->ctx);
}

Engine::~Engine()
{
    fz_drop_context(d->ctx);
}

std::unique_ptr<Document> Engine::open(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("djvu") || suffix == QStringLiteral("djv"))
        return d->djvu.open(path);
    return openMuPdfDocument(d->ctx, path);
}

void Engine::convertKf8ToEpub(const QString &path, const QString &epubPath)
{
    engine::convertKf8ToEpub(d->ctx, path, epubPath);
}

void Engine::saveJpeg(const RenderedPage &page, const QString &path, int quality)
{
    QFileInfo target(path);
    QDir().mkpath(target.absolutePath());
    const QString temporary = path + QStringLiteral(".tmp");
    const QByteArray temporaryUtf8 = temporary.toUtf8();

    QByteArray pixels = page.rgb; // MuPDF needs a writable pointer, though it won't change them.
    fz_context *ctx = d->ctx;
    fz_pixmap *pixmap = nullptr;
    bool failed = false;
    fz_var(pixmap);
    fz_try(ctx) {
        pixmap = fz_new_pixmap_with_data(ctx, fz_device_rgb(ctx), page.width, page.height, nullptr, 0,
                                         page.width * 3, reinterpret_cast<unsigned char *>(pixels.data()));
        fz_save_pixmap_as_jpeg(ctx, pixmap, temporaryUtf8.constData(), quality);
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pixmap);
    }
    fz_catch(ctx) {
        failed = true;
    }

    if (failed) {
        QFile::remove(temporary);
        throw EngineError("could not write cover image");
    }
    QFile::remove(path);
    if (!QFile::rename(temporary, path)) {
        QFile::remove(temporary);
        throw EngineError("could not save cover image");
    }
}

} // namespace papertrail::engine
