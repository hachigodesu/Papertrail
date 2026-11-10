#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace papertrail::core {

class Database;

// A file found by a check, with the publication it belongs to, if any.
struct CheckedFile {
    qint64 fileId = 0;
    qint64 publicationId = 0; // 0 for a file in no publication.
    QString title;            // The publication's, or empty.
    QString path;             // Within its folder.
    QString folder;           // The folder it's in, as the library names it.
    QString problem;          // Why it can't be read, for an unreadable one.
    bool otherFiles = false;  // Its publication has another file, so this one can be forgotten.
};

// A publication waiting for a file from another computer, by the name it had there.
struct WaitingForFile {
    qint64 publicationId = 0;
    QString title;
    QString fileName;
    bool suggested = false; // A file here may be it, waiting in Check suggested files.
};

// Publications with the same identifier, which may be one work entered twice.
struct SharedIdentifier {
    QString kind;  // "DOI", "ISBN", "arXiv" or "PubMed".
    QString value;
    QList<qint64> publicationIds; // The one to keep first: one with a file, then the oldest.
    QStringList titles;           // In the same order.
};

// What checking the library found. Publications kept for removed folders, and anything removed, are left out.
struct LibraryCheckResult {
    QList<CheckedFile> missing;
    QStringList offlineFolders; // Folders whose drive isn't there, which isn't a problem with their files.
    QList<CheckedFile> unreadable;
    QList<WaitingForFile> waiting;
    QList<CheckedFile> unattached; // Read, and in the library's folders, but in no publication.
    QList<SharedIdentifier> shared;

    qsizetype problems() const { return missing.size() + unreadable.size() + waiting.size() + unattached.size() + shared.size(); }
};

// Looks for files that are missing or can't be read, publications waiting for files, files in no publication, and
// identifiers shared by publications that haven't been said to differ. Only reads.
LibraryCheckResult checkLibrary(Database &db);

// Says these publications aren't the same work, so sharing an identifier no longer puts them in a check. Every pair
// among them is kept, by id, and goes when either does.
void markDistinct(Database &db, const QList<qint64> &publicationIds);
bool areDistinct(Database &db, qint64 a, qint64 b);

// Fixes, for the window to offer. Each changes only Papertrail's records, never a file.

// Forgets one location of a publication that has another file. A scan finds it again if it comes back.
void forgetFileLocation(Database &db, qint64 fileId);
// Has the next scan read a file again from the start: for one that couldn't be read, or one in no publication.
void readFileAgain(Database &db, qint64 fileId);
// Keeps a file in no publication out of the library, as removing a publication keeps its files out.
void keepFileOut(Database &db, qint64 fileId, qint64 when);

} // namespace papertrail::core
