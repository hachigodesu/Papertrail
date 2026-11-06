#pragma once

#include <QString>

namespace papertrail::engine {
class Engine;
}

namespace papertrail::worker {

// The file to open for this path: a Kindle book in KF8 format goes through its converted copy, which
// is made now if it doesn't exist yet. Throws EngineError if that can't be done.
QString readablePath(engine::Engine &engine, const QString &path, const QString &convertedPath);

// Serves one open document to the reader, so that a page which crashes a parser only costs that
// process. The reader starts one of these for each document it has open.
//
// Reads one JSON request per line on stdin. Each reply is one JSON line on stdout. If it has a
// "bytes" value, that many bytes of raw data follow straight after the line.
int runReaderHost(engine::Engine &engine);

} // namespace papertrail::worker
