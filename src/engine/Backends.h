#pragma once

#include "engine/Document.h"

#include <memory>

struct fz_context;
struct ddjvu_context_s;

namespace papertrail::engine {

std::unique_ptr<Document> openMuPdfDocument(fz_context *ctx, const QString &path);

// Writes an EPUB made from a KF8 Kindle file. Throws KindleProtected or EngineError.
void convertKf8ToEpub(fz_context *ctx, const QString &path, const QString &epubPath);

// A zip of PDFs. These throw EngineError.
QStringList listZipEntries(fz_context *ctx, const QString &zipPath, int most);
void extractZipEntry(fz_context *ctx, const QString &zipPath, const QString &name, const QString &outPath, qint64 maxBytes);
void stitchPdfs(fz_context *ctx, const QList<StitchSource> &parts, const QString &outPath);

// A DjVu page's own pixels, for copying it: one byte a pixel for a bitonal page, three otherwise.
struct DjvuPagePixels {
    int width = 0;
    int height = 0;
    int dpi = 0; // What it was rendered at.
    bool bitonal = false;
    int channels = 3;
    QByteArray pixels;
    QSizeF size; // The page in points.
};

// Renders a page of a DjVu at its own resolution, or `mostDpi` if that's less and more than 0. Throws EngineError if
// `document` isn't a DjVu or the page can't be rendered.
DjvuPagePixels renderDjvuPage(Document &document, int page, int mostDpi);

class DjvuBackend {
public:
    DjvuBackend();
    ~DjvuBackend();
    DjvuBackend(const DjvuBackend &) = delete;
    DjvuBackend &operator=(const DjvuBackend &) = delete;

    std::unique_ptr<Document> open(const QString &path);

private:
    ddjvu_context_s *m_ctx = nullptr;
};

// Engine::writePdfCopy, with the engines it opens the book with.
PdfCopyResult writePdfCopy(fz_context *ctx, DjvuBackend &djvu, const QString &path, const QString &outPath,
                           const PdfCopyOptions &options, const PdfCopyProgress &progress);

} // namespace papertrail::engine
