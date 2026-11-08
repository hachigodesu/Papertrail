#pragma once

#include <QList>
#include <QString>

namespace papertrail::core {

class Database;

// A file whose publication the user removed, kept out of the library until it's brought back.
struct RemovedEntry {
    qint64 id = 0;
    QString title;    // The publication's, when it was removed.
    QString fileName; // Of the file it was removed with.
    qint64 removedAt = 0;
};

// Publications the user has removed. Their files are remembered by content, so the same file found again,
// by a rescan or in another folder, isn't added back. Files are never touched.
class RemovedContent {
public:
    explicit RemovedContent(Database &db);

    // Remembers each publication's files, then deletes the publications along with their articles, tags, notes
    // and highlights. An article chosen on its own has no files, so nothing about it needs remembering.
    void remove(const QList<qint64> &publicationIds, qint64 when);

    // Whether a file's publication was removed: by its contents, or for one that couldn't be read or is empty,
    // by the file itself. `sha256` is null for a file not hashed.
    bool covers(qint64 fileId, const QString &sha256, qint64 size);

    QList<RemovedEntry> entries();
    qint64 count();

    // Stops keeping these out. Their files are read again by the next scan, so they come back as new.
    // Returns how many files that will be.
    int bringBack(const QList<qint64> &entryIds);

private:
    Database &m_db;
};

} // namespace papertrail::core
