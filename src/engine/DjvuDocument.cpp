#include "engine/Backends.h"

#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>

#include <algorithm>

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

class DjvuDocument : public Document {
public:
    DjvuDocument(ddjvu_context_t *ctx, ddjvu_document_t *doc)
        : m_ctx(ctx), m_doc(doc)
    {
    }

    ~DjvuDocument() override { ddjvu_document_release(m_doc); }

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
        ddjvu_status_t status;
        while ((status = ddjvu_document_get_pageinfo(m_doc, page, &info)) < DDJVU_JOB_OK)
            waitForMessages(m_ctx);
        if (status >= DDJVU_JOB_FAILED || info.dpi <= 0)
            throw EngineError("could not read page size");
        return {info.width * 72.0 / info.dpi, info.height * 72.0 / info.dpi};
    }

    QString largestTextOnFirstPage() override { return {}; } // DjVu text has no font sizes.
    QMap<QString, QString> metadata() override { return {}; }
    QByteArray xmpPacket() override { return {}; }
    QByteArray epubPackageDocument() override { return {}; }

private:
    ddjvu_context_t *m_ctx;
    ddjvu_document_t *m_doc;
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
