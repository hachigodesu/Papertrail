#pragma once

#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "import/WorkerClient.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace papertrail::core {
class Database;
}

namespace papertrail::import {

// The file types Papertrail can index, in lower case and without the dot.
const QStringList &supportedExtensions();
bool isSupportedFile(const QString &fileName);

struct ImportOptions {
    QString workerPath;
    QString cacheDirectory;   // Covers go in a folder inside this one.
    int inspectTimeoutMs = 120000;
    int minHashTimeoutMs = 120000;
    qint64 hashBytesPerSecond = 20 * 1024 * 1024; // A slow disk, used to set hash timeouts.
    int maxAttempts = 2;      // Tries before a file that crashes or hangs the worker is given up on.

    // Files changed more recently than this are left for a later scan, because they may still be
    // being copied. They're counted in ScanReport::stillChanging.
    qint64 settleMs = 0;

    // If set, new publications are queued for online lookup in this mode. Asked each time, because it
    // can change from one import to the next. Empty means no lookup, and nothing sent anywhere.
    std::function<QString()> lookupMode;

    // Checked during a scan and while waiting for the worker, so that stopping doesn't have to
    // wait for a large file. Whatever it interrupts is simply done again next time.
    std::function<bool()> cancelled;
};

struct ScanReport {
    bool rootAvailable = true;
    int unsupportedSkipped = 0;
    int newFiles = 0;
    int changed = 0;
    int unchanged = 0;
    int nowMissing = 0;
    int backAgain = 0;
    int stillChanging = 0;
    bool stoppedEarly = false; // Nothing is known about files that weren't reached.
    qint64 elapsedMs = 0;
};

struct ImportProgress {
    qint64 done = 0;
    qint64 remaining = 0;
    QString currentFile;
};

struct ImportReport {
    int publicationsCreated = 0;
    int duplicates = 0;         // Files with the same contents as one already in the library.
    int moved = 0;              // Files found at a new location.
    int updated = 0;            // Files whose contents changed, keeping their publication.
    int reclassified = 0;       // Publications taken back out of Preprints.
    int formatsJoined = 0;      // Files recognised as another format of an existing publication.
    int relinked = 0;           // Files of publications imported from another computer, found here.
    int unreadable = 0;         // Couldn't be read, or need a password.
    int workerRestarts = 0;     // Crashes or hangs recovered from.
    int needsReview = 0;
    bool stoppedEarly = false;  // Stopped on request. Running again carries on from here.
};

// Whether a path is this folder, or anywhere inside it. Both use forward slashes.
bool isPathInside(const QString &path, const QString &folder);

// Where the cover for a file with these contents is cached, whether or not it exists yet.
QString coverPathFor(const QString &cacheDirectory, const QString &sha256);

// Where a Kindle book that had to be converted keeps its readable copy.
QString convertedPathFor(const QString &cacheDirectory, const QString &sha256);

// Finds files and brings them into the library. All the work goes through a job queue stored in
// the database, so a large import carries on after the app is closed.
class ImportService {
public:
    ImportService(core::Database &db, const ImportOptions &options);
    ~ImportService();

    // Records what's on disk for a root, and queues work for anything new or changed.
    ScanReport scan(qint64 rootId);

    using ProgressCallback = std::function<void(const ImportProgress &)>;
    using StopCheck = std::function<bool()>;

    // Works through the queue until it's empty, or until `shouldStop` returns true.
    ImportReport processQueue(const ProgressCallback &progress = {}, const StopCheck &shouldStop = {});

    qint64 pendingJobs();

    // Where a file's cover is cached, whether or not it exists yet.
    QString coverPath(const QString &sha256) const;

private:
    struct Job;
    bool nextJob(Job &job);
    void finishJob(const Job &job);
    bool retryOrFailJob(const Job &job, const QString &error);
    void requeueJob(const Job &job);
    void enqueue(const char *kind, qint64 fileId);

    bool linkToKnownContents(const core::FileRecord &file, const QString &sha256, ImportReport &report);
    void processFingerprint(const Job &job, ImportReport &report);
    void processInspect(const Job &job, ImportReport &report);
    void processReclassify(const Job &job, ImportReport &report);
    void processJoinFormats(const Job &job, ImportReport &report);

    struct OtherFormat {
        qint64 publicationId = 0;
        bool certain = true; // If not, the result gets marked for checking.
    };
    std::optional<OtherFormat> findOtherFormat(const core::Publication &publication, qint64 selfId,
                                               const QStringList &formats, const QList<core::FileRecord> &files);
    bool holdsAnyFormat(qint64 publicationId, const QStringList &formats);
    void createPublication(const core::FileRecord &file, const InspectResult *inspection, ImportReport &report);
    void markUnreadable(const core::FileRecord &file, const QString &problem, ImportReport &report);

    core::Database &m_db;
    ImportOptions m_options;
    core::FileRepository m_files;
    core::PublicationRepository m_publications;
    WorkerClient m_worker;
};

} // namespace papertrail::import
