// Parses untrusted files in its own process, so a crash here can't take down the app.
//
// Reads one JSON request per line on stdin, and writes one JSON reply per line on stdout. It never
// writes to the documents themselves, only to the cover paths it's given. With --reader, it instead
// serves one open document to the reader: see ReaderHost.h.

#include "ReaderHost.h"

#include "engine/Document.h"
#include "import/Extractors.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace papertrail;

namespace {

constexpr int kCoverSize = 400;
constexpr int kTextPages = 8;
constexpr qsizetype kMaxReplyText = 32 * 1024;
constexpr qsizetype kKindleHeadBytes = 256 * 1024;

// Below this much ink and text, a page has nothing on it worth showing.
constexpr double kBlankInk = 0.0005;
constexpr int kMinTextLetters = 32;

QJsonObject failure(const QString &error)
{
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), false);
    reply.insert(QStringLiteral("error"), error);
    return reply;
}

QJsonObject hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return failure(QStringLiteral("could not open file"));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return failure(QStringLiteral("could not read file"));

    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    return reply;
}

QJsonObject metadataToJson(const import::EmbeddedMetadata &m)
{
    QJsonObject json;
    const auto add = [&json](const char *key, const QString &value) {
        if (!value.isEmpty())
            json.insert(QString::fromLatin1(key), value);
    };
    add("title", m.title);
    add("publisher", m.publisher);
    add("date", m.date);
    add("language", m.language);
    add("doi", m.doi);
    add("isbn", m.isbn);
    add("issn", m.issn);
    add("description", m.description);
    add("keywords", m.keywords);
    if (!m.authors.isEmpty())
        json.insert(QStringLiteral("authors"), QJsonArray::fromStringList(m.authors));
    return json;
}

QString formatFromSuffix(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("djv") ? QStringLiteral("djvu") : suffix;
}

bool isKindleSuffix(const QString &suffix)
{
    return suffix == QStringLiteral("mobi") || suffix == QStringLiteral("azw") || suffix == QStringLiteral("azw3");
}

QJsonObject inspectFile(engine::Engine &engine, const QString &originalPath, const QString &coverPath,
                        const QString &convertedPath)
{
    QString path = originalPath;
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("format"), formatFromSuffix(path));

    import::EmbeddedMetadata metadata;

    // KF8 has to be caught here. MuPDF would open it and quietly give a blank page.
    if (isKindleSuffix(QFileInfo(path).suffix().toLower())) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return failure(QStringLiteral("could not open file"));
        const import::KindleHeader kindle = import::parseKindleHeader(file.read(kKindleHeadBytes));
        if (!kindle.isKindleFile)
            return failure(QStringLiteral("not a valid Kindle file"));
        metadata = kindle.metadata;
        metadata.decodeEntities();
        if (kindle.isKf8 || kindle.isEncrypted) {
            // Whatever goes wrong from here, what the header said still gets listed.
            reply.insert(QStringLiteral("kf8"), kindle.isKf8);
            reply.insert(QStringLiteral("meta"), metadataToJson(metadata));
            if (kindle.isEncrypted) {
                reply.insert(QStringLiteral("problem"), QStringLiteral("protected by DRM"));
                return reply;
            }
            try {
                path = worker::readablePath(engine, path, convertedPath);
            } catch (const engine::EngineError &error) {
                reply.insert(QStringLiteral("problem"), QString::fromUtf8(error.what()));
                return reply;
            }
        }
    }

    std::unique_ptr<engine::Document> document;
    try {
        document = engine.open(path);
    } catch (const engine::EngineError &error) {
        return failure(QString::fromUtf8(error.what()));
    }

    if (document->needsPassword()) {
        reply.insert(QStringLiteral("encrypted"), true);
        return reply;
    }

    const auto info = document->metadata();
    metadata.fillGapsFrom(import::parseXmp(document->xmpPacket()));
    metadata.fillGapsFrom(import::parseOpf(document->epubPackageDocument()));
    metadata.fillGapsFrom(import::parsePdfInfo(info.value(QStringLiteral("info:Title")), info.value(QStringLiteral("info:Author")),
                                               info.value(QStringLiteral("info:Subject")), info.value(QStringLiteral("info:Keywords"))));
    metadata.decodeEntities();
    reply.insert(QStringLiteral("meta"), metadataToJson(metadata));

    int pages = 0;
    try {
        pages = document->pageCount();
    } catch (const engine::EngineError &error) {
        return failure(QString::fromUtf8(error.what()));
    }
    reply.insert(QStringLiteral("pages"), pages);
    if (pages <= 0)
        return failure(QStringLiteral("document has no pages"));

    QString text;
    for (int page = 0; page < pages && page < kTextPages && text.size() < kMaxReplyText; ++page) {
        // A form feed ends each page, because where text appears matters: a DOI on the first page
        // is the paper's own, and one on page eight belongs to something it cites.
        try {
            text += document->pageText(page);
        } catch (const engine::EngineError &) {
            // Later pages may still be readable.
        }
        text += u'\f';
    }
    const auto letters = std::count_if(text.begin(), text.end(), [](QChar c) { return c.isLetter(); });
    const bool hasText = letters >= kMinTextLetters;
    reply.insert(QStringLiteral("hasText"), hasText);
    reply.insert(QStringLiteral("text"), text.left(kMaxReplyText));

    try {
        reply.insert(QStringLiteral("largestText"), document->largestTextOnFirstPage());
    } catch (const engine::EngineError &) {
        // A title guess is a bonus, so carry on without one.
    }

    // The cover is the first page with something on it, since books often start with a blank.
    bool foundInk = false;
    QString renderError;
    engine::RenderedPage firstRendered;
    for (int page = 0; page < pages && page < 3; ++page) {
        try {
            engine::RenderedPage rendered = document->renderPage(page, kCoverSize);
            const bool hasInk = rendered.inkFraction() >= kBlankInk;
            if (page == 0)
                firstRendered = rendered;
            if (hasInk) {
                foundInk = true;
                firstRendered = std::move(rendered);
                break;
            }
        } catch (const engine::EngineError &error) {
            renderError = QString::fromUtf8(error.what());
        }
    }

    if (firstRendered.width > 0 && !coverPath.isEmpty()) {
        try {
            engine.saveJpeg(firstRendered, coverPath);
            reply.insert(QStringLiteral("coverWritten"), true);
        } catch (const engine::EngineError &) {
            // No cover is a problem for the library view, not a reason to fail the file.
        }
    }

    if (!foundInk && !hasText) {
        if (firstRendered.width == 0)
            return failure(renderError.isEmpty() ? QStringLiteral("could not render any page") : renderError);
        reply.insert(QStringLiteral("blank"), true);
    }
    return reply;
}

// A run of pages' text for the full-text index. A document stays open between requests for the same file,
// since reading a long one takes many of them.
QJsonObject pageTexts(engine::Engine &engine, const QJsonObject &request)
{
    constexpr int kMostCharactersOnAPage = 400000; // More than any real page, and it keeps a reply bounded.
    static QString openPath;
    static std::unique_ptr<engine::Document> document;

    const QString path = request.value(QStringLiteral("path")).toString();
    if (path != openPath || !document) {
        document.reset();
        openPath.clear();
        try {
            QString readable = path;
            if (isKindleSuffix(QFileInfo(path).suffix().toLower()))
                readable = worker::readablePath(engine, path, request.value(QStringLiteral("convertedPath")).toString());
            document = engine.open(readable);
            openPath = path;
        } catch (const engine::EngineError &error) {
            return failure(QString::fromUtf8(error.what()));
        }
    }
    if (document->needsPassword())
        return failure(QStringLiteral("it needs a password"));

    int pageCount = 0;
    try {
        pageCount = document->pageCount();
    } catch (const engine::EngineError &error) {
        return failure(QString::fromUtf8(error.what()));
    }

    const int start = request.value(QStringLiteral("start")).toInt();
    const int count = request.value(QStringLiteral("count")).toInt();
    QJsonArray pages;
    for (int page = start; page < pageCount && page < start + count; ++page) {
        // A page that can't be read is an empty one, and later pages may still be fine.
        QString text;
        try {
            text = document->pageText(page);
        } catch (const engine::EngineError &) {
        }
        pages.append(text.left(kMostCharactersOnAPage));
    }

    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("pageCount"), pageCount);
    reply.insert(QStringLiteral("pages"), pages);
    return reply;
}

// Fault injection for testing how the app copes with a worker that dies or hangs. It only ever
// happens with this environment variable set, which the app itself never does.
void applyTestFaults(const QString &path)
{
    static const bool enabled = qEnvironmentVariableIsSet("PAPERTRAIL_WORKER_TEST_FAULTS");
    if (!enabled)
        return;
    const QString name = QFileInfo(path).fileName();
    if (name.contains(QStringLiteral("__crash__")))
        std::abort();
    if (name.contains(QStringLiteral("__hang__"))) {
        while (true)
            QThread::sleep(60);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("papertrail-worker"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PAPERTRAIL_VERSION));

    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.contains(QStringLiteral("--version"))) {
        std::cout << "papertrail-worker " << PAPERTRAIL_VERSION << std::endl;
        return 0;
    }

    engine::Engine engine;

    if (arguments.contains(QStringLiteral("--reader")))
        return worker::runReaderHost(engine);

    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonDocument request = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        const QJsonObject object = request.object();
        const QString op = object.value(QStringLiteral("op")).toString();
        const QString path = object.value(QStringLiteral("path")).toString();

        applyTestFaults(path);

        QJsonObject reply;
        if (op == QStringLiteral("hash")) {
            reply = hashFile(path);
        } else if (op == QStringLiteral("pagetext")) {
            reply = pageTexts(engine, object);
        } else if (op == QStringLiteral("inspect")) {
            reply = inspectFile(engine, path, object.value(QStringLiteral("coverPath")).toString(),
                                object.value(QStringLiteral("convertedPath")).toString());
        } else {
            reply = failure(QStringLiteral("unknown request"));
        }
        reply.insert(QStringLiteral("id"), object.value(QStringLiteral("id")));

        std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData() << std::endl;
    }
    return 0;
}
