#include "engine/Backends.h"
#include "engine/Document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <mupdf/fitz.h>

namespace papertrail::engine {

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
