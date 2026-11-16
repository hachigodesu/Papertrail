// A book downloaded as a zip of PDFs, one per part: listing the zip, copying a part out, and stitching parts into one
// PDF. Only the worker calls these, so a damaged zip can only take the worker down.
//
// MuPDF reports errors with setjmp and longjmp, which skip C++ destructors. So inside each fz_try block there's
// nothing with a destructor, and nothing that can throw a C++ exception.

#include "engine/Backends.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vector>

namespace papertrail::engine {

namespace {

[[noreturn]] void fail(const char *message)
{
    throw EngineError(message);
}

} // namespace

QStringList listZipEntries(fz_context *ctx, const QString &zipPath, int most)
{
    const QByteArray path = zipPath.toUtf8();
    fz_archive *archive = nullptr;
    int count = 0;
    bool failed = false;
    fz_var(archive);
    fz_try(ctx) {
        archive = fz_open_zip_archive(ctx, path.constData());
        count = fz_count_archive_entries(ctx, archive);
    }
    fz_catch(ctx) {
        failed = true;
    }
    if (failed) {
        fz_drop_archive(ctx, archive);
        fail("not a zip that can be read");
    }
    if (count > most) {
        fz_drop_archive(ctx, archive);
        fail("the zip has too many entries");
    }

    QStringList names;
    for (int i = 0; i < count; ++i) {
        const char *name = nullptr;
        fz_var(name);
        fz_try(ctx) {
            name = fz_list_archive_entry(ctx, archive, i);
        }
        fz_catch(ctx) {
            failed = true;
        }
        if (failed)
            break;
        if (name)
            names.append(QString::fromUtf8(name));
    }
    fz_drop_archive(ctx, archive);
    if (failed)
        fail("not a zip that can be read");
    return names;
}

void extractZipEntry(fz_context *ctx, const QString &zipPath, const QString &name, const QString &outPath, qint64 maxBytes)
{
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    const QByteArray path = zipPath.toUtf8();
    const QByteArray entry = name.toUtf8();
    const QByteArray out = outPath.toUtf8();
    fz_archive *archive = nullptr;
    fz_stream *stream = nullptr;
    fz_output *output = nullptr;
    bool failed = false;
    bool tooBig = false;
    fz_var(archive);
    fz_var(stream);
    fz_var(output);
    fz_try(ctx) {
        archive = fz_open_zip_archive(ctx, path.constData());
        stream = fz_open_archive_entry(ctx, archive, entry.constData());
        output = fz_new_output_with_path(ctx, out.constData(), 0);
        unsigned char chunk[65536];
        qint64 written = 0;
        while (true) {
            const size_t got = fz_read(ctx, stream, chunk, sizeof chunk);
            if (got == 0)
                break;
            written += qint64(got);
            // An entry can claim one size and unpack to far more, so what it actually gives is counted.
            if (written > maxBytes) {
                tooBig = true;
                break;
            }
            fz_write_data(ctx, output, chunk, got);
        }
        fz_close_output(ctx, output);
    }
    fz_always(ctx) {
        fz_drop_output(ctx, output);
        fz_drop_stream(ctx, stream);
        fz_drop_archive(ctx, archive);
    }
    fz_catch(ctx) {
        failed = true;
    }
    if (failed || tooBig)
        QFile::remove(outPath);
    if (tooBig)
        fail("a part of the zip is too large");
    if (failed)
        fail("a part of the zip could not be read");
}

namespace {

// Adds `node` and the items after it, with their children, before the iterator's place, which stays at the end.
void insertOutline(fz_context *ctx, fz_outline_iterator *iterator, fz_outline *node, int offset, int depth)
{
    char uri[48];
    for (; node; node = node->next) {
        fz_outline_item item = {};
        item.title = node->title;
        item.uri = nullptr;
        if (node->page.page >= 0) {
            fz_snprintf(uri, sizeof uri, "#page=%d", node->page.page + offset + 1);
            item.uri = uri;
        }
        fz_outline_iterator_insert(ctx, iterator, &item);
        // A file's outline can nest without end, so only a reasonable depth is kept.
        if (node->down && depth < 16 && fz_outline_iterator_prev(ctx, iterator) >= 0) {
            if (fz_outline_iterator_down(ctx, iterator) >= 0) {
                insertOutline(ctx, iterator, node->down, offset, depth + 1);
                fz_outline_iterator_up(ctx, iterator);
            }
            fz_outline_iterator_next(ctx, iterator);
        }
    }
}

} // namespace

void stitchPdfs(fz_context *ctx, const QList<StitchSource> &parts, const QString &outPath)
{
    if (parts.isEmpty())
        fail("there are no parts to stitch");
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    // Named for this process, since the library's worker and a reader's can both stitch one book at once.
    const QString temporary = outPath + QStringLiteral(".%1.part").arg(QCoreApplication::applicationPid());
    const QByteArray temporaryUtf8 = temporary.toUtf8();

    std::vector<QByteArray> paths;
    std::vector<QByteArray> titles;
    for (const StitchSource &part : parts) {
        paths.push_back(part.path.toUtf8());
        titles.push_back(part.title.toUtf8());
    }
    std::vector<int> starts(parts.size(), 0);
    std::vector<fz_outline *> outlines(parts.size(), nullptr);

    pdf_document *stitched = nullptr;
    pdf_document *source = nullptr;
    pdf_graft_map *map = nullptr;
    fz_outline_iterator *iterator = nullptr;
    bool failed = false;
    bool locked = false;
    fz_var(stitched);
    fz_var(source);
    fz_var(map);
    fz_var(iterator);
    fz_try(ctx) {
        stitched = pdf_create_document(ctx);
        int next = 0;
        for (size_t i = 0; i < paths.size(); ++i) {
            source = pdf_open_document(ctx, paths[i].constData());
            if (pdf_needs_password(ctx, source)) {
                locked = true;
                fz_throw(ctx, FZ_ERROR_GENERIC, "a part needs a password");
            }
            const int count = pdf_count_pages(ctx, source);
            map = pdf_new_graft_map(ctx, stitched);
            for (int page = 0; page < count; ++page)
                pdf_graft_mapped_page(ctx, map, -1, source, page);
            pdf_drop_graft_map(ctx, map);
            map = nullptr;
            starts[i] = next;
            next += count;
            fz_try(ctx) {
                outlines[i] = fz_load_outline(ctx, reinterpret_cast<fz_document *>(source));
            }
            fz_catch(ctx) {
                outlines[i] = nullptr; // A part with a broken outline is still worth having.
            }
            pdf_drop_document(ctx, source);
            source = nullptr;
        }

        // One item for each part, leading to its first page, with the part's own outline beneath it.
        iterator = fz_new_outline_iterator(ctx, reinterpret_cast<fz_document *>(stitched));
        char uri[48];
        for (size_t i = 0; i < paths.size(); ++i) {
            fz_outline_item item = {};
            item.title = const_cast<char *>(titles[i].constData());
            fz_snprintf(uri, sizeof uri, "#page=%d", starts[i] + 1);
            item.uri = uri;
            fz_outline_iterator_insert(ctx, iterator, &item);
            if (outlines[i] && fz_outline_iterator_prev(ctx, iterator) >= 0) {
                if (fz_outline_iterator_down(ctx, iterator) >= 0) {
                    insertOutline(ctx, iterator, outlines[i], starts[i], 1);
                    fz_outline_iterator_up(ctx, iterator);
                }
                fz_outline_iterator_next(ctx, iterator);
            }
        }
        fz_drop_outline_iterator(ctx, iterator);
        iterator = nullptr;

        pdf_write_options options = pdf_default_write_options;
        options.do_garbage = 1;
        options.do_compress = 1;
        pdf_save_document(ctx, stitched, temporaryUtf8.constData(), &options);
    }
    fz_always(ctx) {
        fz_drop_outline_iterator(ctx, iterator);
        pdf_drop_graft_map(ctx, map);
        pdf_drop_document(ctx, source);
        pdf_drop_document(ctx, stitched);
        for (fz_outline *outline : outlines)
            fz_drop_outline(ctx, outline);
    }
    fz_catch(ctx) {
        failed = true;
    }

    if (failed) {
        QFile::remove(temporary);
        if (locked)
            fail("a part of the zip needs a password");
        fail("the parts could not be stitched together");
    }
    // Another process may have finished the same book first, and may have it open, so it's kept.
    if (QFileInfo::exists(outPath)) {
        QFile::remove(temporary);
        return;
    }
    if (!QFile::rename(temporary, outPath)) {
        QFile::remove(temporary);
        if (!QFileInfo::exists(outPath))
            fail("the stitched document could not be saved");
    }
}

} // namespace papertrail::engine
