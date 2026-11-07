// MuPDF opens KF8 files without an error, then shows one blank page. So they're turned into an EPUB
// first, which goes in the cache folder and is named after the file's contents. The original is never
// written to. The files this reads are untrusted, so this only ever runs in a worker process.

#include "engine/Backends.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <mobi.h>
#include <mupdf/fitz.h>

#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <cwchar>
#endif

namespace papertrail::engine {

namespace {

const char kContainer[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                          "  <rootfiles>\n"
                          "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
                          "  </rootfiles>\n"
                          "</container>\n";
const char kMimetype[] = "application/epub+zip";

// libmobi opens files with fopen, which on Windows doesn't understand the UTF-8 file names everything
// else here uses, so a book with an accent in its name wouldn't open.
FILE *openForReading(const QString &path)
{
#ifdef Q_OS_WIN
    return _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"rb");
#else
    return std::fopen(QFile::encodeName(path).constData(), "rb");
#endif
}

struct MobiDeleter {
    void operator()(MOBIData *data) const { mobi_free(data); }
};

struct RawmlDeleter {
    void operator()(MOBIRawml *rawml) const { mobi_free_rawml(rawml); }
};

// Every step here checks for errors rather than letting MuPDF's longjmp leave through C++ code.
class EpubWriter {
public:
    EpubWriter(fz_context *ctx, const QByteArray &path)
        : m_ctx(ctx)
    {
        const char *pathText = path.constData();
        fz_try(m_ctx) {
            m_zip = fz_new_zip_writer(m_ctx, pathText);
        }
        fz_catch(m_ctx) {
            m_zip = nullptr;
        }
        if (!m_zip)
            throw EngineError("could not create the converted book");
    }

    ~EpubWriter() { fz_drop_zip_writer(m_ctx, m_zip); }

    void add(const char *name, const void *data, size_t size, bool compress)
    {
        const unsigned char *bytes = static_cast<const unsigned char *>(data);
        fz_buffer *buffer = nullptr;
        bool failed = false;
        fz_var(buffer);
        fz_try(m_ctx) {
            buffer = fz_new_buffer_from_copied_data(m_ctx, bytes, size);
            fz_write_zip_entry(m_ctx, m_zip, name, buffer, compress ? 1 : 0);
        }
        fz_always(m_ctx) {
            fz_drop_buffer(m_ctx, buffer);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed)
            throw EngineError("could not write the converted book");
    }

    void close()
    {
        bool failed = false;
        fz_try(m_ctx) {
            fz_close_zip_writer(m_ctx, m_zip);
        }
        fz_catch(m_ctx) {
            failed = true;
        }
        if (failed)
            throw EngineError("could not finish the converted book");
    }

private:
    fz_context *m_ctx;
    fz_zip_writer *m_zip = nullptr;
};

void addParts(EpubWriter &writer, const MOBIPart *part, const char *prefix)
{
    for (; part; part = part->next) {
        if (part->size == 0)
            continue;
        const MOBIFileMeta meta = mobi_get_filemeta_by_type(part->type);
        // These are the names that the book's own contents and links were rewritten to use.
        const QByteArray name = meta.type == T_OPF
            ? QByteArrayLiteral("OEBPS/content.opf")
            : QByteArrayLiteral("OEBPS/") + prefix + QByteArray::number(qulonglong(part->uid)).rightJustified(5, '0') + '.' + meta.extension;
        writer.add(name.constData(), part->data, part->size, true);
    }
}

} // namespace

void convertKf8ToEpub(fz_context *ctx, const QString &path, const QString &epubPath)
{
    std::unique_ptr<MOBIData, MobiDeleter> mobi(mobi_init());
    if (!mobi)
        throw EngineError("could not start the Kindle converter");

    FILE *file = openForReading(path);
    if (!file)
        throw EngineError("could not open the file");
    const MOBI_RET loaded = mobi_load_file(mobi.get(), file);
    std::fclose(file);
    if (loaded != MOBI_SUCCESS)
        throw EngineError("this isn't a Kindle file that can be read");
    if (mobi_is_encrypted(mobi.get()))
        throw KindleProtected("this book is protected by DRM");

    std::unique_ptr<MOBIRawml, RawmlDeleter> rawml(mobi_init_rawml(mobi.get()));
    if (!rawml)
        throw EngineError("could not read the Kindle book");
    if (mobi_parse_rawml(rawml.get(), mobi.get()) != MOBI_SUCCESS)
        throw EngineError("could not read the Kindle book");
    if (!rawml->markup)
        throw EngineError("this Kindle book has no text to convert");

    // Written under another name first, so that an interrupted conversion can't look like a finished one.
    // The name is this process's own, because the reader and the indexer can convert one book at the same time.
    QDir().mkpath(QFileInfo(epubPath).absolutePath());
    const QString temporary = epubPath + QStringLiteral(".%1.tmp").arg(QCoreApplication::applicationPid());
    try {
        EpubWriter writer(ctx, temporary.toUtf8());
        writer.add("mimetype", kMimetype, sizeof(kMimetype) - 1, false);
        writer.add("META-INF/container.xml", kContainer, sizeof(kContainer) - 1, true);
        addParts(writer, rawml->markup, "part");
        // The first flow part is the book's raw markup, already split up into the parts above.
        addParts(writer, rawml->flow ? rawml->flow->next : nullptr, "flow");
        addParts(writer, rawml->resources, "resource");
        writer.close();
    } catch (const EngineError &) {
        QFile::remove(temporary);
        throw;
    }

    // Another process may have got there first, and may have it open. Theirs is just as good.
    if (QFile::rename(temporary, epubPath))
        return;
    QFile::remove(temporary);
    if (!QFile::exists(epubPath))
        throw EngineError("could not save the converted book");
}

} // namespace papertrail::engine
