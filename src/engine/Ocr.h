#pragma once

#include "engine/Document.h"
#include "engine/WordLayer.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

// Reading the words on a page that's only a picture, such as a scan, with Tesseract. Only this part of the app ever
// calls Tesseract, and only in the worker, which renders the page itself and hands Tesseract its pixels: Tesseract
// and Leptonica never open a file of the user's.
namespace papertrail::engine {

// Whether this build can recognise text at all.
bool canRecognise();

// Tesseract's version, for noting with what it recognised.
QString recogniserVersion();

// Reads the words in a rendered page. Languages are Tesseract's codes joined by "+", such as "eng+fra", and their
// models must be in `tessdata`. Not thread-safe: one per worker.
class Recogniser {
public:
    Recogniser();
    ~Recogniser();
    Recogniser(const Recogniser &) = delete;
    Recogniser &operator=(const Recogniser &) = delete;

    // The words in `page`, which shows a page `pointsPerPixel` points to a pixel, with boxes in points. Throws
    // EngineError if Tesseract can't start with these languages, or can't read the picture.
    QList<TextWord> recognise(const RenderedPage &page, double pointsPerPixel, const QString &tessdata, const QString &languages);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// A document whose pages without text of their own are answered from words recognised on them, where there are
// some. Everything else is the document's own. `wordsFor` gives a page's recognised words, or nothing if it has none;
// it's asked once a page, and setWords replaces what it said. A document whose own text is poor, such as an old scan
// with a publisher's text layer, can prefer its recognised words wherever a page has them.
class RecognisedTextDocument : public Document {
public:
    using WordSource = std::function<std::optional<QList<TextWord>>(int page)>;
    RecognisedTextDocument(std::unique_ptr<Document> inner, WordSource wordsFor);

    void setWords(int page, const QList<TextWord> &words);
    bool hasRecognisedWords(int page);
    void setPreferRecognised(bool prefer) { m_prefer = prefer; }
    bool prefersRecognised() const { return m_prefer; }
    // Asks `wordsFor` again for every page, as after they've been recognised elsewhere.
    void forgetWords() { m_words.clear(); }
    // Whether a page's text comes from its recognised words rather than its own.
    bool usesRecognisedWords(int page);
    Document &inner() { return *m_inner; }

    bool needsPassword() override { return m_inner->needsPassword(); }
    int pageCount() override { return m_inner->pageCount(); }
    bool isReflowable() override { return m_inner->isReflowable(); }
    QString pageText(int page) override;
    RenderedPage renderPage(int page, int maxDimension) override { return m_inner->renderPage(page, maxDimension); }
    QSizeF pageSize(int page) override { return m_inner->pageSize(page); }
    DisplayImage renderForDisplay(int page, double scale) override { return m_inner->renderForDisplay(page, scale); }
    void setLayout(double width, double height, double fontSize) override;
    qint64 bookmarkForPage(int page) override { return m_inner->bookmarkForPage(page); }
    int pageForBookmark(qint64 bookmark) override { return m_inner->pageForBookmark(bookmark); }
    QString placeForPage(int page) override { return m_inner->placeForPage(page); }
    int pageForPlace(const QString &place) override { return m_inner->pageForPlace(place); }
    QList<OutlineItem> outline() override { return m_inner->outline(); }
    QList<PageLink> links(int page) override { return m_inner->links(page); }
    QList<QRectF> imageAreas(int page) override { return m_inner->imageAreas(page); }
    QList<Drawing> drawings(int page) override { return m_inner->drawings(page); }
    QList<QRectF> search(int page, const QString &text) override;
    TextSelection selectText(int page, const QPointF &a, const QPointF &b) override;
    QList<TextLine> textLines(int page) override;
    QString largestTextOnPage(int page) override;
    QMap<QString, QString> metadata() override { return m_inner->metadata(); }
    QByteArray xmpPacket() override { return m_inner->xmpPacket(); }
    QByteArray epubPackageDocument() override { return m_inner->epubPackageDocument(); }
    bool canSaveMarkedCopy() override { return m_inner->canSaveMarkedCopy(); }
    void saveMarkedCopy(const QString &path, const QList<MarkToWrite> &marks) override { m_inner->saveMarkedCopy(path, marks); }

private:
    const QList<TextWord> *wordsOn(int page);

    std::unique_ptr<Document> m_inner;
    WordSource m_wordsFor;
    QHash<int, std::optional<QList<TextWord>>> m_words;
    bool m_prefer = false;
};

// Whether a page is only a picture, worth recognising: almost no text of its own, and either a picture covering most
// of it, or, where the format says nothing of its pictures, no text at all but something on it. Fixed pages only.
bool looksScanned(Document &document, int page);

} // namespace papertrail::engine
