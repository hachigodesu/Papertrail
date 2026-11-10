// Parses untrusted files in its own process, so a crash here can't take down the app.
//
// Reads one JSON request per line on stdin, and writes one JSON reply per line on stdout. It never
// writes to the documents themselves, only to the cover paths it's given. With --reader, it instead
// serves one open document to the reader: see ReaderHost.h.

#include "ReaderHost.h"
#include "RecognisedText.h"

#include "engine/Document.h"
#include "engine/Ocr.h"
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
        reply.insert(QStringLiteral("largestText"), document->largestTextOnPage(0));
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

// The document the last request was about, kept open since requests usually come in runs for one file. It
// has to be closed before the engine that opened it goes, so main() closes it before returning.
QString openPath;
QString openRecognised; // Where its recognised words are kept, if anywhere.
std::unique_ptr<engine::Document> document;

// The document a request is about. Returns null with `error` set if it can't be read.
engine::Document *documentFor(engine::Engine &engine, const QJsonObject &request, QString &error)
{

    const QString path = request.value(QStringLiteral("path")).toString();
    const QString recognised = request.value(QStringLiteral("ocrFolder")).toString();
    if (path != openPath || recognised != openRecognised || !document) {
        document.reset();
        openPath.clear();
        try {
            QString readable = path;
            if (isKindleSuffix(QFileInfo(path).suffix().toLower()))
                readable = worker::readablePath(engine, path, request.value(QStringLiteral("convertedPath")).toString());
            document = worker::withRecognisedText(engine.open(readable), recognised);
            openPath = path;
            openRecognised = recognised;
        } catch (const engine::EngineError &caught) {
            error = QString::fromUtf8(caught.what());
            return nullptr;
        }
    }
    if (document->needsPassword()) {
        error = QStringLiteral("it needs a password");
        return nullptr;
    }
    return document.get();
}

// A cover from one page, for an article that starts there.
QJsonObject pageCover(engine::Engine &engine, const QJsonObject &request)
{
    QString error;
    engine::Document *document = documentFor(engine, request, error);
    if (!document)
        return failure(error);
    const int page = request.value(QStringLiteral("page")).toInt(-1);
    const QString coverPath = request.value(QStringLiteral("coverPath")).toString();
    try {
        if (page < 0 || page >= document->pageCount())
            return failure(QStringLiteral("there's no such page"));
        if (coverPath.isEmpty())
            return failure(QStringLiteral("there's nowhere to put the cover"));
        engine.saveJpeg(document->renderPage(page, kCoverSize), coverPath);
    } catch (const engine::EngineError &caught) {
        return failure(QString::fromUtf8(caught.what()));
    }
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    return reply;
}

QJsonObject pageTexts(engine::Engine &engine, const QJsonObject &request)
{
    constexpr int kMostCharactersOnAPage = 400000; // More than any real page, and it keeps a reply bounded.
    QString error;
    engine::Document *document = documentFor(engine, request, error);
    if (!document)
        return failure(error);

    int pageCount = 0;
    try {
        pageCount = document->pageCount();
    } catch (const engine::EngineError &error) {
        return failure(QString::fromUtf8(error.what()));
    }

    const int start = request.value(QStringLiteral("start")).toInt();
    const int count = request.value(QStringLiteral("count")).toInt();
    auto *withWords = dynamic_cast<engine::RecognisedTextDocument *>(document);
    QJsonArray pages;
    QJsonArray recognised; // Which pages' text was recognised rather than the file's own.
    for (int page = start; page < pageCount && page < start + count; ++page) {
        // A page that can't be read is an empty one, and later pages may still be fine.
        QString text;
        try {
            text = document->pageText(page);
            if (withWords && withWords->hasRecognisedWords(page) && text != withWords->inner().pageText(page))
                recognised.append(page);
        } catch (const engine::EngineError &) {
        }
        pages.append(text.left(kMostCharactersOnAPage));
    }

    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("pageCount"), pageCount);
    reply.insert(QStringLiteral("pages"), pages);
    reply.insert(QStringLiteral("recognised"), recognised);
    return reply;
}

// Recognises the words on the scanned pages among `count` from `start`, and keeps them in `ocrFolder`. Pages with
// text of their own, or already recognised, are left alone unless `again`. The rest of the reply says how it went,
// so the app knows where to carry on from.
QJsonObject recognisePages(engine::Engine &engine, const QJsonObject &request)
{
    static engine::Recogniser recogniser;
    const QString folder = request.value(QStringLiteral("ocrFolder")).toString();
    const QString tessdata = request.value(QStringLiteral("tessdata")).toString();
    const QString languages = request.value(QStringLiteral("languages")).toString();
    if (!engine::canRecognise())
        return failure(QStringLiteral("this copy of Papertrail can't recognise text"));
    if (folder.isEmpty() || tessdata.isEmpty() || languages.isEmpty())
        return failure(QStringLiteral("there's nowhere to keep the words, or no language to read them in"));
    QString error;
    engine::Document *document = documentFor(engine, request, error);
    if (!document)
        return failure(error);
    auto *withWords = dynamic_cast<engine::RecognisedTextDocument *>(document);
    engine::Document &own = withWords ? withWords->inner() : *document;

    const bool again = request.value(QStringLiteral("again")).toBool();
    const int start = qMax(0, request.value(QStringLiteral("start")).toInt());
    const int count = qMax(0, request.value(QStringLiteral("count")).toInt());
    int pageCount = 0;
    int recognisedPages = 0;
    int words = 0;
    try {
        pageCount = document->pageCount();
        for (int page = start; page < pageCount && page < start + count; ++page) {
            if (!again && worker::readRecognised(folder, page))
                continue;
            // A page that can't be read is left, as the rest may be fine.
            try {
                if (!engine::looksScanned(own, page))
                    continue;
                worker::applyPageFaults(request.value(QStringLiteral("path")).toString(), page);
                const QList<engine::TextWord> found = worker::recognisePage(recogniser, own, page, tessdata, languages);
                worker::writeRecognised(folder, page, found, languages);
                if (withWords)
                    withWords->setWords(page, found);
                ++recognisedPages;
                words += int(found.size());
            } catch (const engine::EngineError &caught) {
                const QString message = QString::fromUtf8(caught.what());
                // A missing language is the same on every page, so it stops here.
                if (message.contains(QLatin1StringView("couldn't start")))
                    return failure(message);
            }
        }
    } catch (const engine::EngineError &caught) {
        return failure(QString::fromUtf8(caught.what()));
    }

    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("pageCount"), pageCount);
    reply.insert(QStringLiteral("recognised"), recognisedPages);
    reply.insert(QStringLiteral("words"), words);
    return reply;
}

// Writes a copy of a PDF with highlights and notes added, for reading elsewhere. The document is opened afresh,
// so nothing cached is changed, and the original file is never written.
QJsonObject markedCopy(engine::Engine &engine, const QJsonObject &request)
{
    const QString path = request.value(QStringLiteral("path")).toString();
    const QString out = request.value(QStringLiteral("out")).toString();
    if (out.isEmpty())
        return failure(QStringLiteral("there's nowhere to write the copy"));
    QList<engine::MarkToWrite> marks;
    for (const QJsonValue &value : request.value(QStringLiteral("marks")).toArray()) {
        const QJsonObject object = value.toObject();
        engine::MarkToWrite mark;
        mark.page = object.value(QStringLiteral("page")).toInt(-1);
        const QString kind = object.value(QStringLiteral("kind")).toString();
        mark.kind = kind == QLatin1StringView("underline") ? engine::MarkToWrite::Kind::Underline
            : kind == QLatin1StringView("note")            ? engine::MarkToWrite::Kind::Note
            : kind == QLatin1StringView("area")            ? engine::MarkToWrite::Kind::Area
                                                           : engine::MarkToWrite::Kind::Highlight;
        mark.colour = object.value(QStringLiteral("colour")).toString();
        mark.note = object.value(QStringLiteral("note")).toString().left(100000);
        for (const QJsonValue &rect : object.value(QStringLiteral("rects")).toArray()) {
            const QJsonArray numbers = rect.toArray();
            if (numbers.size() == 4)
                mark.rects << QRectF(numbers.at(0).toDouble(), numbers.at(1).toDouble(), numbers.at(2).toDouble(), numbers.at(3).toDouble());
        }
        marks << mark;
    }
    try {
        const std::unique_ptr<engine::Document> fresh = engine.open(path);
        if (fresh->needsPassword())
            return failure(QStringLiteral("the file needs a password"));
        if (!fresh->canSaveMarkedCopy())
            return failure(QStringLiteral("only a PDF can have its highlights written into a copy"));
        fresh->saveMarkedCopy(out, marks);
    } catch (const engine::EngineError &error) {
        return failure(QString::fromUtf8(error.what()));
    }
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("marks"), int(marks.size()));
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
        } else if (op == QStringLiteral("ocr")) {
            reply = recognisePages(engine, object);
        } else if (op == QStringLiteral("cover")) {
            reply = pageCover(engine, object);
        } else if (op == QStringLiteral("markedcopy")) {
            reply = markedCopy(engine, object);
        } else if (op == QStringLiteral("inspect")) {
            reply = inspectFile(engine, path, object.value(QStringLiteral("coverPath")).toString(),
                                object.value(QStringLiteral("convertedPath")).toString());
        } else {
            reply = failure(QStringLiteral("unknown request"));
        }
        reply.insert(QStringLiteral("id"), object.value(QStringLiteral("id")));

        std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData() << std::endl;
    }
    document.reset();
    return 0;
}
