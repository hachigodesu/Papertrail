#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include <memory>
#include <stdexcept>

namespace papertrail::engine {

class EngineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A rendered page as tightly packed 8-bit RGB.
struct RenderedPage {
    int width = 0;
    int height = 0;
    QByteArray rgb;

    // The share of pixels that aren't close to white, from 0 to 1.
    double inkFraction() const;
};

// One open document. Nothing here ever writes to the document's file.
class Document {
public:
    virtual ~Document() = default;

    virtual bool needsPassword() = 0;
    virtual int pageCount() = 0;
    virtual bool isReflowable() = 0;

    // Throws EngineError if the page can't be read.
    virtual QString pageText(int page) = 0;
    virtual RenderedPage renderPage(int page, int maxDimension) = 0;

    // The text on the first page set in the largest type, which is often the title.
    virtual QString largestTextOnFirstPage() = 0;

    // Keys such as "info:Title", where the format has them.
    virtual QMap<QString, QString> metadata() = 0;

    // Raw embedded metadata. Empty if the format or file has none.
    virtual QByteArray xmpPacket() = 0;
    virtual QByteArray epubPackageDocument() = 0;
};

// Opens documents with the right engine for each format. Use one per thread.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Throws EngineError if the file can't be opened as a document.
    std::unique_ptr<Document> open(const QString &path);

    // Writes the file only if encoding succeeds, replacing any earlier version.
    void saveJpeg(const RenderedPage &page, const QString &path, int quality = 80);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace papertrail::engine
