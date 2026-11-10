#pragma once

#include "engine/Document.h"
#include "engine/Ocr.h"

#include <QList>
#include <QString>

#include <memory>
#include <optional>

// Words recognised on a document's pages, kept in the cache folder beside it, one file a page, named by the file's
// contents so they follow it anywhere. The app never reads them itself: only the worker does, which is what renders
// and recognises the pages, and they're what was recognised, made again if lost.
namespace papertrail::worker {

// The file one page's words are kept in.
QString recognisedPagePath(const QString &folder, int page);

// A page's kept words, or nothing if it hasn't been recognised. A page recognised as having none has an empty list.
std::optional<QList<engine::TextWord>> readRecognised(const QString &folder, int page);

// Keeps a page's words, replacing any earlier ones, all at once so a crash never leaves half a file.
void writeRecognised(const QString &folder, int page, const QList<engine::TextWord> &words, const QString &languages);

// Whether every page of the document has been asked to be recognised, its own text being poor, so its recognised
// words are used in place of its own. Kept as a file in the same folder.
bool recognisingEveryPage(const QString &folder);
void markRecognisingEveryPage(const QString &folder);

// The document, answering text questions for its scanned pages from the words kept in `folder`, or for every page if
// they're all to be recognised. As it is, if `folder` is empty.
std::unique_ptr<engine::Document> withRecognisedText(std::unique_ptr<engine::Document> document, const QString &folder);

// Renders a page at a resolution good for reading, and recognises its words.
QList<engine::TextWord> recognisePage(engine::Recogniser &recogniser, engine::Document &document, int page,
                                      const QString &tessdata, const QString &languages);

} // namespace papertrail::worker
