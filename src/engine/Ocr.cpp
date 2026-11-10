#include "engine/Ocr.h"

#ifdef PAPERTRAIL_WITH_OCR
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>
#endif

#include <QtGlobal>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace papertrail::engine {

namespace {

// Fewer letters than this is a page number or a stray mark, not a page of text.
constexpr int kFewestLetters = 20;

int letters(const QString &text)
{
    return int(std::count_if(text.begin(), text.end(), [](QChar c) { return c.isLetter(); }));
}

} // namespace

bool looksScanned(Document &document, int page)
{
    if (document.isReflowable())
        return false;
    const int own = letters(document.pageText(page));
    if (own >= kFewestLetters)
        return false;
    const QSizeF size = document.pageSize(page);
    const double area = size.width() * size.height();
    if (area <= 0)
        return false;
    const QList<QRectF> pictures = document.imageAreas(page);
    // A DjVu file says nothing about its pictures. With nothing to go by, a page with no text at all but something on
    // it is a scan, while a few words of its own, such as a heading on a page of its own, aren't.
    if (pictures.isEmpty())
        return own == 0 && document.renderPage(page, 300).inkFraction() > 0.002;
    for (const QRectF &picture : pictures) {
        const QRectF onPage = picture.intersected(QRectF(QPointF(0, 0), size));
        if (onPage.width() * onPage.height() >= area * 0.6)
            return true;
    }
    return false;
}

// ---- Recognising

#ifdef PAPERTRAIL_WITH_OCR

bool canRecognise()
{
    return true;
}

QString recogniserVersion()
{
    return QStringLiteral("tesseract ") + QString::fromLatin1(tesseract::TessBaseAPI::Version());
}

struct Recogniser::Private {
    tesseract::TessBaseAPI api;
    QString started; // The folder and languages it was started with.
};

Recogniser::Recogniser()
    : d(std::make_unique<Private>())
{
    // Tesseract reads its models' numbers in the C locale's way, and spreads its work over every core unless told,
    // which a background task shouldn't.
    std::setlocale(LC_NUMERIC, "C");
    if (!qEnvironmentVariableIsSet("OMP_THREAD_LIMIT"))
        qputenv("OMP_THREAD_LIMIT", "1");
}

Recogniser::~Recogniser()
{
    d->api.End();
}

QList<TextWord> Recogniser::recognise(const RenderedPage &page, double pointsPerPixel, const QString &tessdata,
                                      const QString &languages)
{
    if (page.width <= 0 || page.height <= 0 || page.rgb.size() < qsizetype(page.width) * page.height * 3)
        throw EngineError("there's no picture of the page to read");
    const QString wanted = tessdata + u'|' + languages;
    if (d->started != wanted) {
        d->api.End();
        d->started.clear();
        const QByteArray folder = tessdata.toUtf8();
        const QByteArray codes = languages.toUtf8();
        if (d->api.Init(folder.constData(), codes.constData(), tesseract::OEM_LSTM_ONLY) != 0)
            throw EngineError("text recognition couldn't start: its language files may be missing");
        d->api.SetPageSegMode(tesseract::PSM_AUTO);
        // Quiet, since the worker's own output is what's read.
#ifdef Q_OS_WIN
        d->api.SetVariable("debug_file", "NUL");
#else
        d->api.SetVariable("debug_file", "/dev/null");
#endif
        d->started = wanted;
    }

    d->api.SetImage(reinterpret_cast<const unsigned char *>(page.rgb.constData()), page.width, page.height, 3, page.width * 3);
    d->api.SetSourceResolution(qBound(70, int(std::lround(72.0 / pointsPerPixel)), 1200));
    if (d->api.Recognize(nullptr) != 0) {
        d->api.Clear();
        throw EngineError("the page's text couldn't be recognised");
    }

    QList<TextWord> words;
    std::unique_ptr<tesseract::ResultIterator> it(d->api.GetIterator());
    if (it) {
        const tesseract::PageIteratorLevel level = tesseract::RIL_WORD;
        do {
            std::unique_ptr<char[]> text(it->GetUTF8Text(level));
            if (!text)
                continue;
            const QString word = QString::fromUtf8(text.get()).trimmed();
            int left = 0, top = 0, right = 0, bottom = 0;
            if (word.isEmpty() || !it->BoundingBox(level, &left, &top, &right, &bottom))
                continue;
            // Very doubtful words are usually specks and smudges.
            if (it->Confidence(level) < 20)
                continue;
            TextWord found;
            found.text = word;
            found.rect = QRectF(left * pointsPerPixel, top * pointsPerPixel, (right - left) * pointsPerPixel,
                                (bottom - top) * pointsPerPixel);
            found.endsLine = it->IsAtFinalElement(tesseract::RIL_TEXTLINE, level);
            words << found;
        } while (it->Next(level));
    }
    if (!words.isEmpty())
        words.last().endsLine = true;
    d->api.Clear();
    return words;
}

#else

bool canRecognise()
{
    return false;
}

QString recogniserVersion()
{
    return {};
}

struct Recogniser::Private {};

Recogniser::Recogniser() : d(std::make_unique<Private>()) {}
Recogniser::~Recogniser() = default;

QList<TextWord> Recogniser::recognise(const RenderedPage &, double, const QString &, const QString &)
{
    throw EngineError("this copy of Papertrail can't recognise text");
}

#endif

// ---- A document with recognised words

RecognisedTextDocument::RecognisedTextDocument(std::unique_ptr<Document> inner, WordSource wordsFor)
    : m_inner(std::move(inner)), m_wordsFor(std::move(wordsFor))
{
}

const QList<TextWord> *RecognisedTextDocument::wordsOn(int page)
{
    if (m_inner->isReflowable())
        return nullptr;
    auto found = m_words.find(page);
    if (found == m_words.end())
        found = m_words.insert(page, m_wordsFor ? m_wordsFor(page) : std::nullopt);
    return found->has_value() && !(*found)->isEmpty() ? &**found : nullptr;
}

void RecognisedTextDocument::setWords(int page, const QList<TextWord> &words)
{
    m_words.insert(page, words);
}

bool RecognisedTextDocument::hasRecognisedWords(int page)
{
    return wordsOn(page) != nullptr;
}

bool RecognisedTextDocument::usesRecognisedWords(int page)
{
    if (!wordsOn(page))
        return false;
    return m_prefer || letters(m_inner->pageText(page)) < kFewestLetters;
}

void RecognisedTextDocument::setLayout(double width, double height, double fontSize)
{
    m_inner->setLayout(width, height, fontSize);
}

QString RecognisedTextDocument::pageText(int page)
{
    // The page's own text wins wherever it has any, unless the document prefers what was recognised: recognised words
    // otherwise only fill in for a page without.
    if (const QList<TextWord> *words = wordsOn(page); words && m_prefer)
        return words::pageText(*words);
    const QString own = m_inner->pageText(page);
    if (const QList<TextWord> *words = wordsOn(page); words && letters(own) < kFewestLetters)
        return words::pageText(*words);
    return own;
}

QList<QRectF> RecognisedTextDocument::search(int page, const QString &text)
{
    if (const QList<TextWord> *words = wordsOn(page); words && m_prefer)
        return words::search(*words, text);
    const QList<QRectF> own = m_inner->search(page, text);
    if (!own.isEmpty())
        return own;
    if (const QList<TextWord> *words = wordsOn(page))
        return words::search(*words, text);
    return {};
}

TextSelection RecognisedTextDocument::selectText(int page, const QPointF &a, const QPointF &b)
{
    if (const QList<TextWord> *words = wordsOn(page); words && (m_prefer || m_inner->textLines(page).isEmpty()))
        return words::selectText(*words, a, b);
    return m_inner->selectText(page, a, b);
}

QList<TextLine> RecognisedTextDocument::textLines(int page)
{
    if (const QList<TextWord> *words = wordsOn(page); words && m_prefer)
        return words::textLines(*words);
    QList<TextLine> own = m_inner->textLines(page);
    if (!own.isEmpty())
        return own;
    if (const QList<TextWord> *words = wordsOn(page))
        return words::textLines(*words);
    return {};
}

QString RecognisedTextDocument::largestTextOnPage(int page)
{
    if (const QList<TextWord> *words = wordsOn(page); words && m_prefer)
        return words::largestText(*words);
    const QString own = m_inner->largestTextOnPage(page);
    if (!own.isEmpty())
        return own;
    if (const QList<TextWord> *words = wordsOn(page))
        return words::largestText(*words);
    return {};
}

} // namespace papertrail::engine
