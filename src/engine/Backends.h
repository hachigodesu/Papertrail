#pragma once

#include "engine/Document.h"

#include <memory>

struct fz_context;
struct ddjvu_context_s;

namespace papertrail::engine {

std::unique_ptr<Document> openMuPdfDocument(fz_context *ctx, const QString &path);

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
