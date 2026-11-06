#pragma once

#include <QtGlobal>

namespace papertrail::core {
class Database;
}

namespace papertrail::tools {

struct SyntheticOptions {
    int publications = 100000;
    quint32 seed = 20261106; // The same seed and count always give the same library contents.
};

struct SyntheticStats {
    qint64 publications = 0;
    qint64 people = 0;
    qint64 tags = 0;
    qint64 files = 0;
};

// Fills an empty, migrated database with fake publications, through the same repositories
// the app uses, with the kinds of proportions a real collection might have.
SyntheticStats generateSyntheticLibrary(core::Database &db, const SyntheticOptions &options);

} // namespace papertrail::tools
