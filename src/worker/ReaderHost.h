#pragma once

namespace papertrail::engine {
class Engine;
}

namespace papertrail::worker {

// Serves one open document to the reader, so that a page which crashes a parser only costs that
// process. The reader starts one of these for each document it has open.
//
// Reads one JSON request per line on stdin. Each reply is one JSON line on stdout. If it has a
// "bytes" value, that many bytes of raw data follow straight after the line.
int runReaderHost(engine::Engine &engine);

} // namespace papertrail::worker
