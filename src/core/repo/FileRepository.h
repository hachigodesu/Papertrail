#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

struct LibraryRoot {
    qint64 id = 0;
    QString uuid;
    QString name;
    QString path;
};

enum class FileStatus { Ok, Missing, Offline, Unreadable };

struct FileRecord {
    qint64 id = 0;
    qint64 rootId = 0;
    QString relativePath; // Always stored with forward slashes.
    qint64 size = 0;
    qint64 modifiedAt = 0;
    QString sha256; // Null until the file has been hashed.
    QString format;
    std::optional<int> pageCount;
    std::optional<bool> hasTextLayer; // Unset until the file has been read.
    FileStatus status = FileStatus::Ok;
    QString problem; // Why a file couldn't be read, for showing to the user.
};

class FileRepository {
public:
    explicit FileRepository(Database &db) : m_db(db) {}

    qint64 addRoot(const QString &name, const QString &path);
    QList<LibraryRoot> roots(); // The ones being indexed, so not any that have been removed.
    QHash<qint64, qint64> fileCountsByRoot();

    // Points a root at a new location. Its files keep their relative paths and links.
    void repointRoot(qint64 rootId, const QString &newPath);

    // Marks a root's files offline (an unplugged drive, say), or brings them back.
    // Files with another problem keep that status.
    void setRootOnline(qint64 rootId, bool online);

    // What removing a root would do to the library: publications that would leave it, and those that
    // stay because they also have a file in another folder.
    struct RemovalImpact {
        qint64 leaving = 0;
        qint64 staying = 0;
    };
    RemovalImpact removalImpact(qint64 rootId);
    // For removing several at once, where a publication in two of them leaves rather than stays.
    RemovalImpact removalImpact(const QList<qint64> &rootIds);

    // Stops indexing a root, and hides what's only there. Its records are kept, so adding the folder
    // again brings everything back. Anything also found elsewhere just loses this location.
    void hideRoot(qint64 rootId);
    void restoreRoot(qint64 rootId);
    QList<LibraryRoot> removedRoots();

    // Kept for removed folders. Publications waiting for their files are hidden too, but aren't these.
    qint64 hiddenCount();
    QList<qint64> hiddenPublications();
    // Hidden publications from this root, along with any articles inside them. A hidden publication's
    // files are all in one root, because removing a folder drops its copy of anything found elsewhere.
    QList<qint64> hiddenIn(qint64 rootId);

    // Forgets a root and its file records for good. Publications themselves stay.
    void removeRoot(qint64 rootId);

    // Inserts a file, or updates the existing record for the same root and relative path.
    qint64 upsert(FileRecord &file);

    std::optional<FileRecord> get(qint64 id);
    QList<FileRecord> withHash(const QString &sha256);
    void setHash(qint64 fileId, const QString &sha256);
    void setStatus(qint64 fileId, FileStatus status, const QString &problem = QString());
    void setDocumentFacts(qint64 fileId, std::optional<int> pageCount, std::optional<bool> hasTextLayer);

    QList<FileRecord> filesInRoot(qint64 rootId);
    std::optional<LibraryRoot> root(qint64 rootId);

    // Gives another file this one's publication links, then removes this one's record. For a file
    // that's been moved or renamed.
    void replaceFile(qint64 oldFileId, qint64 newFileId);

    QList<qint64> publicationsFor(qint64 fileId);

    // Links a file to a publication. Making it primary replaces any earlier primary file.
    void link(qint64 publicationId, qint64 fileId, bool primary = false);
    void unlink(qint64 publicationId, qint64 fileId);
    QList<FileRecord> filesFor(qint64 publicationId);

    // For publications imported from another computer: the contents of the files each had there, so that a
    // file with those contents can be connected to it whenever one turns up here.
    struct ExpectedFile {
        QString sha256;
        qint64 size = 0;
        QString format;
        QString fileName;
        bool primary = false;
    };
    void expectFile(qint64 publicationId, const ExpectedFile &file);
    // Connects this file to a publication waiting for its contents, and returns which. Nothing happens to a file
    // in a removed folder, or to an empty one, since all empty files have the same contents.
    std::optional<qint64> linkWaitingPublication(qint64 fileId);
    // Those still waiting are hidden until a file turns up, with the articles inside them.
    void hideWaitingPublications();
    qint64 waitingPublicationCount();
    QList<qint64> waitingPublications(); // With the articles inside them.

    QString absolutePath(qint64 fileId);

private:
    Database &m_db;
};

} // namespace papertrail::core
