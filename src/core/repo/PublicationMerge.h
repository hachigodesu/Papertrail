#pragma once

#include <QString>

namespace papertrail::core {

class Database;

// Makes two publications into one, as when they turn out to be the same book in two formats. `keep`
// gains everything of `other`'s: files, tags, notes, highlights and reading places. Its own values
// stay wherever it has them. `other` is then removed. Your files aren't touched either way.
void mergePublications(Database &db, qint64 keep, qint64 other, const QString &source);

// Takes one format's files out of a publication and into a new one of their own, with a copy of the
// metadata and tags, marked for checking. Returns the new publication's id. Throws
// std::invalid_argument if those are the only files the publication has.
qint64 separateFormat(Database &db, qint64 publication, const QString &format);

} // namespace papertrail::core
