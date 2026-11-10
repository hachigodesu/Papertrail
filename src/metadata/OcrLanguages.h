#pragma once

#include "metadata/Transport.h"

#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

// The languages text can be recognised in. English comes with Papertrail. Others are fetched when chosen, from
// Tesseract's own collection of fast models at a fixed version, and kept only if they're exactly the file expected.
namespace papertrail::metadata {

struct OcrLanguage {
    const char *code; // Tesseract's, such as "fra" or "chi_sim".
    const char *name; // In English, such as "French".
    qint64 bytes;
    const char *sha256;
};

// Every language offered, by name, English first.
const QList<OcrLanguage> &ocrLanguages();
const OcrLanguage *ocrLanguage(const QString &code);

// Where a language's model is. Only ever this address, built here: nothing from elsewhere chooses where it comes from.
QUrl ocrLanguageUrl(const OcrLanguage &language);

// Fetches a language's model into `tessdata`, checking its size and SHA-256 before keeping it. Calls `done` with an
// empty string when it's there, or what went wrong. Only ever called when the user asks for a language, and only while
// lookups are allowed online, which the caller checks.
void downloadOcrLanguage(Transport &transport, const OcrLanguage &language, const QString &tessdata,
                         const std::function<void(const QString &error)> &done);

} // namespace papertrail::metadata
