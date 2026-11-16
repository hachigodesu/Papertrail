#pragma once

// Books downloaded as a zip of PDFs, one per part. The zip and its parts are only ever opened here, in the worker.

#include "import/Stitching.h"

#include <QJsonObject>
#include <QString>

namespace papertrail::engine {
class Engine;
}

namespace papertrail::worker {

// Whether a zip holds a book's parts, from its list of entries alone, so other zips cost almost nothing.
QJsonObject zipCheck(engine::Engine &engine, const QString &path);

// What each part shows, and the order worked out from that, with a picture of each part's first page written to
// `coverFolder` as <its SHA-256>.jpg when one's given.
QJsonObject zipParts(engine::Engine &engine, const QString &path, const QString &coverFolder);

// The zip's stitched PDF, in the order kept beside `convertedPath`, stitched now if it hasn't been. With no order kept
// yet, the order worked out is kept and used. Throws EngineError.
QString stitchedZip(engine::Engine &engine, const QString &zipPath, const QString &convertedPath);

} // namespace papertrail::worker
