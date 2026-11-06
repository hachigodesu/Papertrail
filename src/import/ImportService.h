#pragma once

#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "import/WorkerClient.h"

#include <QString>

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
};

struct ScanReport {
    bool rootAvailable = true;
    int unsupportedSkipped = 0;
    int newFiles = 0;
    int changed = 0;
    int unchanged = 0;
    int nowMissing = 0;
    int backAgain = 0;
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
    int unreadable = 0;         // Couldn't be read, or need a password.
    int workerRestarts = 0;     // Crashes or hangs recovered from.
    int needsReview = 0;
    bool stoppedEarly = false;  // Stopped on request. Running again carries on from here.
};

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
    void enqueue(const char *kind, qint64 fileId);

    bool linkToKnownContents(const core::FileRecord &file, const QString &sha256, ImportReport &report);
    void processFingerprint(const Job &job, ImportReport &report);
    void processInspect(const Job &job, ImportReport &report);
    void createPublication(const core::FileRecord &file, const InspectResult *inspection, ImportReport &report);
    void markUnreadable(const core::FileRecord &file, const QString &problem, ImportReport &report);

    core::Database &m_db;
    ImportOptions m_options;
    core::FileRepository m_files;
    core::PublicationRepository m_publications;
    WorkerClient m_worker;
};

} // namespace papertrail::import
