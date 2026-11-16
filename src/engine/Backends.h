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

} // namespace papertrail::engine
