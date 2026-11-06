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

    ~MuPdfDocument() override { fz_drop_document(m_ctx, m_doc); }

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
    // Reflowable formats have no pages until they're laid out at some size.
    void layoutIfNeeded()
    {
        if (m_laidOut)
            return;
        m_laidOut = true;
        if (!isReflowable())
            return;
        fz_try(m_ctx) {
            fz_layout_document(m_ctx, m_doc, 595, 842, 11);
        }
        fz_catch(m_ctx) {
            // Page counts and rendering will report the problem if it matters.
        }
    }

    fz_context *m_ctx;
    QByteArray m_path;
    fz_document *m_doc = nullptr;
    bool m_laidOut = false;
};

} // namespace

std::unique_ptr<Document> openMuPdfDocument(fz_context *ctx, const QString &path)
{
    return std::make_unique<MuPdfDocument>(ctx, path);
}

} // namespace papertrail::engine
