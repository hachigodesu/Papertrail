#pragma once

#include <QString>

namespace papertrail::app {

// Checks that an installed copy has everything it needs, without a window and without touching the user's
// library: the worker opens a PDF, an EPUB and a DjVu and writes their covers, the covers can be read back,
// a library in a temporary folder can be made and searched, and secure connections are possible for lookups.
// Packages run this to prove a fresh install works. Writes what it found to `reportPath`, or to standard
// output if that's empty, and returns whether everything passed.
bool runSelfTest(const QString &reportPath);

} // namespace papertrail::app
