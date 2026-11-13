#pragma once

#include <QString>

#include <cstdio>

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

// Replies carry binary data, so nothing else may reach them. Libraries sometimes print to stdout, so this takes a private
// copy of it, and points the original somewhere harmless.
FILE *claimStandardOutput();

// For testing how the worker copes with a page that crashes or hangs. Only with PAPERTRAIL_WORKER_TEST_FAULTS set,
// which the app itself never does: a file named __crashpage3__.pdf crashes on page 3.
void applyPageFaults(const QString &path, int page);

} // namespace papertrail::worker
