#pragma once

#include <vector>

namespace papertrail::core {

class Database;

struct Migration {
    int version;
    const char *sql;
};

// The library database's schema changes, in order.
const std::vector<Migration> &libraryMigrations();

// Brings the database up to the newest version. Each migration is atomic. Throws DbError if a
// migration fails, or if the database comes from a newer version of Papertrail.
void migrate(Database &db, const std::vector<Migration> &migrations);
void migrateLibrary(Database &db);

} // namespace papertrail::core
