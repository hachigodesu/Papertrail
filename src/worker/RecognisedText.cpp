#include "RecognisedText.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace papertrail::worker {

namespace {

constexpr int kVersion = 1;
constexpr qint64 kLargestFile = 8 * 1024 * 1024; // Far more than any page's words.
constexpr double kDotsPerInch = 300;             // What Tesseract reads best at.
constexpr int kLargestSide = 6000;               // In pixels, for a page claiming to be huge.

} // namespace

QString recognisedPagePath(const QString &folder, int page)
{
    return QDir(folder).filePath(QStringLiteral("p%1.json").arg(page));
}

std::optional<QList<engine::TextWord>> readRecognised(const QString &folder, int page)
{
    if (folder.isEmpty())
        return std::nullopt;
    QFile file(recognisedPagePath(folder, page));
    if (!file.open(QIODevice::ReadOnly) || file.size() > kLargestFile)
        return std::nullopt;
    const QJsonObject kept = QJsonDocument::fromJson(file.readAll()).object();
    if (kept.value(QStringLiteral("version")).toInt() != kVersion)
        return std::nullopt;
    QList<engine::TextWord> words;
    for (const QJsonValue &value : kept.value(QStringLiteral("words")).toArray()) {
        const QJsonArray w = value.toArray();
        if (w.size() < 5)
            continue;
        engine::TextWord word;
        word.rect = QRectF(w.at(0).toDouble(), w.at(1).toDouble(), w.at(2).toDouble(), w.at(3).toDouble());
        word.text = w.at(4).toString().left(200);
        word.endsLine = w.size() > 5 && w.at(5).toBool();
        if (!word.text.isEmpty() && word.rect.width() >= 0 && word.rect.height() >= 0)
            words << word;
    }
    return words;
}

void writeRecognised(const QString &folder, int page, const QList<engine::TextWord> &words, const QString &languages)
{
    QDir().mkpath(folder);
    QJsonArray list;
    for (const engine::TextWord &word : words) {
        // To a hundredth of a point, which is plenty, and keeps the files small.
        const auto rounded = [](double value) { return std::round(value * 100) / 100; };
        list.append(QJsonArray{rounded(word.rect.x()), rounded(word.rect.y()), rounded(word.rect.width()),
                               rounded(word.rect.height()), word.text, word.endsLine});
    }
    const QJsonObject kept{{QStringLiteral("version"), kVersion},
                           {QStringLiteral("recogniser"), engine::recogniserVersion()},
                           {QStringLiteral("languages"), languages},
                           {QStringLiteral("words"), list}};
    QSaveFile file(recognisedPagePath(folder, page));
    if (!file.open(QIODevice::WriteOnly))
        throw engine::EngineError("the page's words couldn't be kept");
    file.write(QJsonDocument(kept).toJson(QJsonDocument::Compact));
    if (!file.commit())
        throw engine::EngineError("the page's words couldn't be kept");
}

bool recognisingEveryPage(const QString &folder)
{
    return !folder.isEmpty() && QFile::exists(QDir(folder).filePath(QStringLiteral("every-page")));
}

void markRecognisingEveryPage(const QString &folder)
{
    QDir().mkpath(folder);
    QSaveFile file(QDir(folder).filePath(QStringLiteral("every-page")));
    if (!file.open(QIODevice::WriteOnly) || file.write("1") != 1 || !file.commit())
        throw engine::EngineError("the document couldn't be marked for recognising");
}

std::unique_ptr<engine::Document> withRecognisedText(std::unique_ptr<engine::Document> document, const QString &folder)
{
    if (folder.isEmpty())
        return document;
    auto recognised = std::make_unique<engine::RecognisedTextDocument>(std::move(document),
                                                                        [folder](int page) { return readRecognised(folder, page); });
    recognised->setPreferRecognised(recognisingEveryPage(folder));
    return recognised;
}

QList<engine::TextWord> recognisePage(engine::Recogniser &recogniser, engine::Document &document, int page,
                                      const QString &tessdata, const QString &languages)
{
    const QSizeF size = document.pageSize(page);
    const double longest = qMax(size.width(), size.height());
    if (longest <= 0)
        throw engine::EngineError("the page has no size");
    const int pixels = qBound(500, int(longest * kDotsPerInch / 72.0), kLargestSide);
    const engine::RenderedPage rendered = document.renderPage(page, pixels);
    const double pointsPerPixel = longest / double(qMax(rendered.width, rendered.height));
    return recogniser.recognise(rendered, pointsPerPixel, tessdata, languages);
}

} // namespace papertrail::worker
