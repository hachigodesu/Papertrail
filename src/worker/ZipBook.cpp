#include "ZipBook.h"

#include "engine/Document.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <memory>

namespace papertrail::worker {

namespace {

// More than any book has parts, and limits on what they unpack to, so a zip made to fill the disc is refused.
constexpr int kMostEntries = 400;
constexpr qint64 kMostPartBytes = qint64(1) << 30;
constexpr qint64 kMostTotalBytes = qint64(4) << 30;
constexpr int kPartCoverSize = 240;
// How much of a page's height at its top and bottom a page number is looked for in.
constexpr double kMarginShare = 0.12;

QJsonObject failure(const QString &message)
{
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

QString sha256Of(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

QStringList bookParts(engine::Engine &engine, const QString &path)
{
    QStringList pdfs;
    if (!import::isBookZip(engine.zipEntries(path, kMostEntries), &pdfs))
        throw engine::EngineError("the zip isn't a book's parts");
    return pdfs;
}

// Copies the named parts out of the zip into `folder`, one file each, in order, keeping to the limits.
QStringList extractParts(engine::Engine &engine, const QString &path, const QStringList &names, const QString &folder)
{
    QStringList files;
    qint64 total = 0;
    for (qsizetype i = 0; i < names.size(); ++i) {
        const QString file = QDir(folder).filePath(QStringLiteral("part%1.pdf").arg(i));
        engine.extractZipEntry(path, names.at(i), file, qMin(kMostPartBytes, kMostTotalBytes - total));
        total += QFileInfo(file).size();
        files << file;
    }
    return files;
}

void addNumber(import::ZipPart &part, int page, const QString &text)
{
    int value = 0;
    bool roman = false;
    if (import::printedNumberIn(text, value, roman))
        part.numbers << import::PrintedNumber{page, value, roman};
}

// What one part shows that tells where it goes.
import::ZipPart readPart(engine::Engine &engine, const QString &name, const QString &file, const QString &coverFolder)
{
    import::ZipPart part;
    part.name = name;
    part.size = QFileInfo(file).size();
    part.sha256 = sha256Of(file);
    std::unique_ptr<engine::Document> document;
    try {
        document = engine.open(file);
        if (document->needsPassword()) {
            part.problem = QStringLiteral("Needs a password");
            return part;
        }
        part.pageCount = document->pageCount();
    } catch (const engine::EngineError &) {
        part.problem = QStringLiteral("Can't be read");
        return part;
    }
    if (part.pageCount <= 0) {
        part.problem = QStringLiteral("Has no pages");
        return part;
    }

    try {
        part.title = document->metadata().value(QStringLiteral("info:Title")).simplified().left(300);
        part.heading = document->largestTextOnPage(0).simplified().left(300);
    } catch (const engine::EngineError &) {
        // Only clues: it can still be placed by its name or its numbers.
    }

    // Page numbers printed on its first two and last two pages, and its page labels.
    QList<int> pages{0, 1, part.pageCount - 2, part.pageCount - 1};
    QSet<int> looked;
    for (int page : std::as_const(pages)) {
        if (page < 0 || page >= part.pageCount || looked.contains(page))
            continue;
        looked.insert(page);
        try {
            const double height = document->pageSize(page).height();
            for (const engine::TextLine &line : document->textLines(page)) {
                if (line.rect.top() < height * kMarginShare || line.rect.bottom() > height * (1 - kMarginShare))
                    addNumber(part, page, line.text);
            }
        } catch (const engine::EngineError &) {
            // A page that can't be read gives no number.
        }
    }
    // A PDF without labels of its own still gives each page one, its place in the file, which says nothing.
    const QString firstLabel = document->pageLabel(0);
    const QString lastLabel = document->pageLabel(part.pageCount - 1);
    if (firstLabel != QStringLiteral("1") || lastLabel != QString::number(part.pageCount)) {
        addNumber(part, 0, firstLabel);
        if (part.pageCount > 1)
            addNumber(part, part.pageCount - 1, lastLabel);
    }

    if (!coverFolder.isEmpty() && !part.sha256.isEmpty()) {
        try {
            engine.saveJpeg(document->renderPage(0, kPartCoverSize),
                            QDir(coverFolder).filePath(part.sha256 + QStringLiteral(".jpg")));
        } catch (const engine::EngineError &) {
            // The dialog shows the part without a picture.
        }
    }
    return part;
}

QList<import::ZipPart> readParts(engine::Engine &engine, const QString &path, const QString &coverFolder)
{
    const QStringList names = bookParts(engine, path);
    QTemporaryDir folder;
    if (!folder.isValid())
        throw engine::EngineError("there's nowhere to unpack the zip");
    QList<import::ZipPart> parts;
    qint64 total = 0;
    for (qsizetype i = 0; i < names.size(); ++i) {
        const QString file = folder.filePath(QStringLiteral("part%1.pdf").arg(i));
        try {
            engine.extractZipEntry(path, names.at(i), file, qMin(kMostPartBytes, kMostTotalBytes - total));
        } catch (const engine::EngineError &) {
            import::ZipPart part;
            part.name = names.at(i);
            part.problem = QStringLiteral("Can't be unpacked");
            parts << part;
            continue;
        }
        total += QFileInfo(file).size();
        parts << readPart(engine, names.at(i), file, coverFolder);
        QFile::remove(file);
    }
    return parts;
}

// Writes `data` to `path` so it appears whole or not at all.
bool writeWhole(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(data);
    return file.commit();
}

} // namespace

QJsonObject zipCheck(engine::Engine &engine, const QString &path)
{
    QJsonObject reply{{QStringLiteral("ok"), true}};
    try {
        QStringList pdfs;
        reply.insert(QStringLiteral("book"), import::isBookZip(engine.zipEntries(path, kMostEntries), &pdfs));
        reply.insert(QStringLiteral("parts"), int(pdfs.size()));
    } catch (const engine::EngineError &) {
        reply.insert(QStringLiteral("book"), false); // Not a zip that can be read, or too many entries: not a book.
    }
    return reply;
}

QJsonObject zipParts(engine::Engine &engine, const QString &path, const QString &coverFolder)
{
    try {
        const QList<import::ZipPart> parts = readParts(engine, path, coverFolder);
        const import::StitchPlan plan = import::stitchOrder(parts);
        QStringList names;
        for (const import::ZipPart &part : parts)
            names << part.name;
        const import::BookFromNames book = import::bookFromNames(names);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("plan"), import::planToJson(plan)},
                           {QStringLiteral("certain"), plan.certain},
                           {QStringLiteral("bookTitle"), book.title},
                           {QStringLiteral("bookYear"), book.year}};
    } catch (const engine::EngineError &error) {
        return failure(QString::fromUtf8(error.what()));
    }
}

QString stitchedZip(engine::Engine &engine, const QString &zipPath, const QString &convertedPath)
{
    if (convertedPath.isEmpty())
        throw engine::EngineError("this book's parts need stitching, and there's nowhere to put it");

    const QString orderPath = import::orderPathFor(convertedPath);
    QList<import::OrderedPart> order;
    QFile kept(orderPath);
    if (kept.open(QIODevice::ReadOnly))
        order = import::orderFromJson(kept.read(1 << 20));
    kept.close();
    if (order.isEmpty()) {
        order = import::orderedParts(import::stitchOrder(readParts(engine, zipPath, QString())));
        writeWhole(orderPath, import::orderToJson(order));
    }

    const QString stitched = import::stitchedPathFor(convertedPath, order);
    if (QFileInfo::exists(stitched))
        return stitched;

    QStringList names;
    QStringList titles;
    for (const import::OrderedPart &part : std::as_const(order)) {
        if (part.included) {
            names << part.name;
            titles << part.title;
        }
    }
    if (names.isEmpty())
        throw engine::EngineError("every part of this book has been left out");

    QTemporaryDir folder;
    if (!folder.isValid())
        throw engine::EngineError("there's nowhere to unpack the zip");
    const QStringList files = extractParts(engine, zipPath, names, folder.path());
    QList<engine::StitchSource> sources;
    for (qsizetype i = 0; i < files.size(); ++i)
        sources << engine::StitchSource{files.at(i), titles.at(i)};
    engine.stitchPdfs(sources, stitched);
    return stitched;
}

} // namespace papertrail::worker
