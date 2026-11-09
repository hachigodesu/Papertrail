#pragma once

#include <vector>

namespace papertrail::core {

class Database;

struct Migration {
    int version;
    const char *sql;
    // In place of or after `sql`, for a change that SQL alone can't make safely more than once, such as rebuilding a
    // table. It must check whether it's been made already.
    void (*run)(Database &db) = nullptr;
    // For rebuilding a table that others refer to, which with foreign keys on would delete everything referring to it.
    // Nothing may be left referring to what isn't there once it's done, or it's undone.
    bool foreignKeysOff = false;
};

// The library database's schema changes, in order.
const std::vector<Migration> &libraryMigrations();

// Brings the database up to the newest version. Each migration is atomic. Throws DbError if a
// migration fails, or if the database comes from a newer version of Papertrail.
void migrate(Database &db, const std::vector<Migration> &migrations);
void migrateLibrary(Database &db);

} // namespace papertrail::core
