#include "import/ImportService.h"

#include "core/db/Database.h"
#include "core/model/Identifiers.h"
#include "core/repo/LookupRepository.h"
#include "core/repo/PublicationMerge.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace papertrail::import {

using namespace core;

namespace {

const char *const kFingerprintJob = "fingerprint";
const char *const kInspectJob = "inspect";
const char *const kReclassifyJob = "reclassify";
const char *const kJoinFormatsJob = "join_formats";

// Commit this often during a scan, so a scan stopped part way keeps what it has found.
constexpr int kScanBatchSize = 500;

QString formatForFile(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix == QStringLiteral("djv"))
        return QStringLiteral("djvu");
    if (suffix == QStringLiteral("oxps"))
        return QStringLiteral("xps");
    if (suffix == QStringLiteral("azw"))
        return QStringLiteral("azw3");
    return suffix;
}

// A title without its subtitle, ready to compare. A PDF's metadata often has only one or the other.
QString mainTitleKey(const QString &title)
{
    return titleSortKey(title.section(u':', 0, 0));
}

// Long enough that two different works are unlikely to share it, unlike "Introduction" or "Notes".
bool isDistinctiveTitle(const QString &key)
{
    return key.size() >= 12 && key.contains(u' ');
}

bool sameMainTitle(const Publication &a, const Publication &b)
{
    const QString keyA = mainTitleKey(a.title);
    const QString keyB = mainTitleKey(b.title);
    return !keyA.isEmpty() && keyA == keyB;
}

QStringList formatsOf(const QList<FileRecord> &files)
{
    QStringList formats;
    for (const FileRecord &file : files) {
        if (!formats.contains(file.format))
            formats << file.format;
    }
    return formats;
}

QString jobPayload(qint64 fileId)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("file"), fileId}}).toJson(QJsonDocument::Compact));
}

qint64 fileIdFromPayload(const QString &payload)
{
    return QJsonDocument::fromJson(payload.toUtf8()).object().value(QStringLiteral("file")).toInteger();
}

} // namespace

const QStringList &supportedExtensions()
{
    static const QStringList extensions = {
        QStringLiteral("pdf"), QStringLiteral("epub"), QStringLiteral("djvu"), QStringLiteral("djv"),
        QStringLiteral("mobi"), QStringLiteral("azw"), QStringLiteral("azw3"), QStringLiteral("xps"),
        QStringLiteral("oxps"), QStringLiteral("cbz"), QStringLiteral("fb2"),
    };
    return extensions;
}

bool isSupportedFile(const QString &fileName)
{
    return supportedExtensions().contains(QFileInfo(fileName).suffix().toLower());
}

bool isPathInside(const QString &path, const QString &folder)
{
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString base = folder.endsWith(u'/') ? folder : folder + u'/';
    return path.compare(folder, sensitivity) == 0 || path.startsWith(base, sensitivity);
}

QString coverPathFor(const QString &cacheDirectory, const QString &sha256)
{
    // An article's key is its file's hash and its first page, as core::articleCoverKey makes it.
    const qsizetype mark = sha256.indexOf(u'#');
    if (mark > 0) {
        const QString hash = sha256.left(mark);
        return QDir(cacheDirectory).filePath(QStringLiteral("covers/%1/%2-p%3.jpg").arg(hash.left(2), hash, sha256.mid(mark + 1)));
    }
    return QDir(cacheDirectory).filePath(QStringLiteral("covers/%1/%2.jpg").arg(sha256.left(2), sha256));
}

QString convertedPathFor(const QString &cacheDirectory, const QString &sha256)
{
    return QDir(cacheDirectory).filePath(QStringLiteral("converted/%1/%2.epub").arg(sha256.left(2), sha256));
}

struct ImportService::Job {
    qint64 id = 0;
    QString kind;
    qint64 fileId = 0;
    int attempts = 0;
};

ImportService::ImportService(Database &db, const ImportOptions &options)
    : m_db(db), m_options(options), m_files(db), m_publications(db), m_worker(options.workerPath, options.cancelled)
{
    // Anything still marked as running was interrupted the last time the app stopped. That wasn't
    // the file's fault: the worker's own crashes are dealt with as they happen.
    m_db.exec("UPDATE job SET status = 'pending', attempts = max(attempts - 1, 0) WHERE status = 'running' AND kind <> 'lookup'");
}

ImportService::~ImportService() = default;

QString ImportService::coverPath(const QString &sha256) const
{
    return coverPathFor(m_options.cacheDirectory, sha256);
}

void ImportService::enqueue(const char *kind, qint64 fileId)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Statement &insert = m_db.cached(
        "INSERT OR IGNORE INTO job (kind, payload, created_at, updated_at) VALUES (?, ?, ?, ?)");
    insert.bind(1, kind);
    insert.bind(2, jobPayload(fileId));
    insert.bind(3, now);
    insert.bind(4, now);
    insert.step();
    insert.reset();
}

qint64 ImportService::pendingJobs()
{
    auto select = m_db.prepare("SELECT count(*) FROM job WHERE status = 'pending' AND kind <> 'lookup'");
    select->step();
    return select->int64At(0);
}

ScanReport ImportService::scan(qint64 rootId)
{
    ScanReport report;
    QElapsedTimer timer;
    timer.start();
    const std::optional<LibraryRoot> root = m_files.root(rootId);
    if (!root)
        throw std::invalid_argument("library location doesn't exist");

    // An unplugged drive isn't the same as deleted files, so nothing is marked missing here.
    if (!QFileInfo::exists(root->path)) {
        m_files.setRootOnline(rootId, false);
        report.rootAvailable = false;
        return report;
    }
    m_files.setRootOnline(rootId, true);

    QHash<QString, FileRecord> known;
    for (const FileRecord &file : m_files.filesInRoot(rootId))
        known.insert(file.relativePath, file);

    const QDir base(root->path);
    const qint64 changedBefore = QDateTime::currentMSecsSinceEpoch() - m_options.settleMs;

    // A library folder inside this one looks after its own files. Otherwise each of them would be
    // seen twice, and appear to be its own duplicate.
    QStringList nestedRoots;
    for (const LibraryRoot &other : m_files.roots()) {
        if (other.id != rootId && isPathInside(other.path, root->path))
            nestedRoots << other.path;
    }

    QSet<QString> seen;
    int batch = 0;
    auto transaction = std::make_unique<Transaction>(m_db);

    QDirIterator it(root->path, QDir::Files | QDir::NoSymLinks | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (m_options.cancelled && m_options.cancelled()) {
            report.stoppedEarly = true;
            break;
        }
        const QString path = it.next();
        if (std::any_of(nestedRoots.cbegin(), nestedRoots.cend(),
                        [&](const QString &nested) { return isPathInside(path, nested); })) {
            continue;
        }
        if (!isSupportedFile(path)) {
            ++report.unsupportedSkipped;
            continue;
        }

        const QFileInfo info = it.fileInfo();
        const QString relative = QDir::fromNativeSeparators(base.relativeFilePath(path));
        const qint64 modified = info.lastModified().toMSecsSinceEpoch();
        seen.insert(relative);

        const auto existing = known.constFind(relative);
        if (existing != known.constEnd() && existing->size == info.size() && existing->modifiedAt == modified) {
            if (existing->status == FileStatus::Missing) {
                m_files.setStatus(existing->id, FileStatus::Ok);
                ++report.backAgain;
            } else {
                ++report.unchanged;
            }
            // Not hashed yet: an earlier import stopped first, or this file failed last time.
            if (existing->sha256.isNull())
                enqueue(kFingerprintJob, existing->id);
            continue;
        }

        // Windows keeps a copied file's original date, but locks it until the copy has finished.
        if (m_options.settleMs > 0 && (modified > changedBefore || !QFile(path).open(QIODevice::ReadOnly))) {
            ++report.stillChanging;
            continue;
        }

        FileRecord file;
        file.rootId = rootId;
        file.relativePath = relative;
        file.size = info.size();
        file.modifiedAt = modified;
        file.format = formatForFile(path);
        m_files.upsert(file);
        enqueue(kFingerprintJob, file.id);
        if (existing != known.constEnd())
            ++report.changed;
        else
            ++report.newFiles;

        if (++batch >= kScanBatchSize) {
            transaction->commit();
            transaction = std::make_unique<Transaction>(m_db);
            batch = 0;
        }
    }

    if (!report.stoppedEarly) {
        for (auto entry = known.constBegin(); entry != known.constEnd(); ++entry) {
            if (!seen.contains(entry.key()) && entry->status != FileStatus::Missing) {
                m_files.setStatus(entry->id, FileStatus::Missing);
                ++report.nowMissing;
            }
        }
    }
    transaction->commit();
    report.elapsedMs = timer.elapsed();
    return report;
}

bool ImportService::nextJob(Job &job)
{
    Statement &select = m_db.cached(
        "SELECT id, kind, payload, attempts FROM job WHERE status = 'pending' AND kind <> 'lookup' ORDER BY id LIMIT 1");
    if (!select.step()) {
        select.reset();
        return false;
    }
    job.id = select.int64At(0);
    job.kind = select.textAt(1);
    job.fileId = fileIdFromPayload(select.textAt(2));
    job.attempts = select.intAt(3);
    select.reset();

    Statement &claim = m_db.cached(
        "UPDATE job SET status = 'running', attempts = attempts + 1, updated_at = ? WHERE id = ?");
    claim.bind(1, QDateTime::currentMSecsSinceEpoch());
    claim.bind(2, job.id);
    claim.step();
    claim.reset();
    ++job.attempts;
    return true;
}

void ImportService::finishJob(const Job &job)
{
    Statement &remove = m_db.cached("DELETE FROM job WHERE id = ?");
    remove.bind(1, job.id);
    remove.step();
    remove.reset();
}

// Puts a job back as though it hadn't been tried.
void ImportService::requeueJob(const Job &job)
{
    Statement &update = m_db.cached(
        "UPDATE job SET status = 'pending', attempts = max(attempts - 1, 0), updated_at = ? WHERE id = ?");
    update.bind(1, QDateTime::currentMSecsSinceEpoch());
    update.bind(2, job.id);
    update.step();
    update.reset();
}

// Returns true if the job has used all its tries and has now been marked as failed.
bool ImportService::retryOrFailJob(const Job &job, const QString &error)
{
    const bool giveUp = job.attempts >= m_options.maxAttempts;
    Statement &update = m_db.cached("UPDATE job SET status = ?, last_error = ?, updated_at = ? WHERE id = ?");
    update.bind(1, giveUp ? QStringLiteral("failed") : QStringLiteral("pending"));
    update.bind(2, error);
    update.bind(3, QDateTime::currentMSecsSinceEpoch());
    update.bind(4, job.id);
    update.step();
    update.reset();
    return giveUp;
}

ImportReport ImportService::processQueue(const ProgressCallback &progress, const StopCheck &shouldStop)
{
    ImportReport report;
    qint64 done = 0;
    const int startsBefore = m_worker.startCount();

    Job job;
    while (true) {
        if ((shouldStop && shouldStop()) || (m_options.cancelled && m_options.cancelled())) {
            report.stoppedEarly = pendingJobs() > 0;
            break;
        }
        if (!nextJob(job))
            break;

        if (progress) {
            QString name;
            if (const auto file = m_files.get(job.fileId))
                name = file->relativePath;
            progress({done, pendingJobs(), name});
        }

        if (job.kind == QLatin1StringView(kFingerprintJob))
            processFingerprint(job, report);
        else if (job.kind == QLatin1StringView(kInspectJob))
            processInspect(job, report);
        else if (job.kind == QLatin1StringView(kReclassifyJob))
            processReclassify(job, report);
        else if (job.kind == QLatin1StringView(kJoinFormatsJob))
            processJoinFormats(job, report);
        else
            finishJob(job);
        ++done;
    }

    // The first start is expected, and anything beyond that is a recovery.
    report.workerRestarts = qMax(0, m_worker.startCount() - startsBefore - (startsBefore == 0 ? 1 : 0));
    return report;
}

void ImportService::markUnreadable(const FileRecord &file, const QString &problem, ImportReport &report)
{
    m_files.setStatus(file.id, FileStatus::Unreadable, problem);
    ++report.unreadable;
}

void ImportService::processFingerprint(const Job &job, ImportReport &report)
{
    const std::optional<FileRecord> file = m_files.get(job.fileId);
    if (!file) {
        finishJob(job);
        return;
    }

    const QString path = m_files.absolutePath(file->id);
    if (!QFile::exists(path)) {
        m_files.setStatus(file->id, FileStatus::Missing);
        finishJob(job);
        return;
    }

    const qint64 hashSeconds = file->size / qMax<qint64>(1, m_options.hashBytesPerSecond);
    const int timeout = int(qMin<qint64>(qMax<qint64>(m_options.minHashTimeoutMs, hashSeconds * 1000), 3600000));
    const WorkerClient::HashResult hash = m_worker.hash(path, timeout);

    if (hash.failure == WorkerFailure::Cancelled) {
        requeueJob(job);
        return;
    }
    if (hash.failure == WorkerFailure::Crashed || hash.failure == WorkerFailure::TimedOut) {
        if (retryOrFailJob(job, hash.error)) {
            Transaction transaction(m_db);
            markUnreadable(*file, hash.error, report);
            createPublication(*file, nullptr, report);
            transaction.commit();
        }
        return;
    }
    if (hash.failure == WorkerFailure::Rejected) {
        Transaction transaction(m_db);
        markUnreadable(*file, hash.error, report);
        createPublication(*file, nullptr, report);
        finishJob(job);
        transaction.commit();
        return;
    }

    Transaction transaction(m_db);
    m_files.setHash(file->id, hash.sha256);

    if (!linkToKnownContents(*file, hash.sha256, report)) {
        if (!m_files.publicationsFor(file->id).isEmpty())
            ++report.updated; // Same path, new contents: the publication and its edits are kept.
        enqueue(kInspectJob, file->id);
    }
    finishJob(job);
    transaction.commit();
}

// Returns true if these contents already belong to a publication, under another name or in
// another place, and this file has now been connected to it.
bool ImportService::linkToKnownContents(const FileRecord &file, const QString &sha256, ImportReport &report)
{
    // All empty files have the same hash, but that doesn't make them the same document.
    if (file.size == 0)
        return false;

    for (const FileRecord &other : m_files.withHash(sha256)) {
        if (other.id == file.id)
            continue;
        const QList<qint64> publications = m_files.publicationsFor(other.id);
        if (publications.isEmpty())
            continue;

        m_files.setDocumentFacts(file.id, other.pageCount, other.hasTextLayer);
        if (other.status == FileStatus::Missing) {
            // The old copy has gone: this is the same file in a new place.
            m_files.replaceFile(other.id, file.id);
            ++report.moved;
        } else {
            // Both exist: one publication with two locations.
            m_files.link(publications.first(), file.id, false);
            ++report.duplicates;
        }
        return true;
    }
    return false;
}

void ImportService::processInspect(const Job &job, ImportReport &report)
{
    const std::optional<FileRecord> file = m_files.get(job.fileId);
    if (!file) {
        finishJob(job);
        return;
    }

    const bool alreadyLinked = !m_files.publicationsFor(file->id).isEmpty();

    // Two copies of a new file both get here, because neither had a publication when it was
    // hashed. By now, the first has created one.
    if (!alreadyLinked) {
        Transaction transaction(m_db);
        if (linkToKnownContents(*file, file->sha256, report)) {
            finishJob(job);
            transaction.commit();
            return;
        }
        transaction.commit();
    }

    const QString path = m_files.absolutePath(file->id);
    const WorkerClient::Inspection inspection = m_worker.inspect(
        path, coverPath(file->sha256), convertedPathFor(m_options.cacheDirectory, file->sha256), m_options.inspectTimeoutMs);

    if (inspection.failure == WorkerFailure::Cancelled) {
        requeueJob(job);
        return;
    }
    if (inspection.failure == WorkerFailure::Crashed || inspection.failure == WorkerFailure::TimedOut) {
        if (retryOrFailJob(job, inspection.error)) {
            Transaction transaction(m_db);
            markUnreadable(*file, inspection.error, report);
            if (!alreadyLinked)
                createPublication(*file, nullptr, report);
            transaction.commit();
        }
        return;
    }

    Transaction transaction(m_db);
    if (inspection.failure == WorkerFailure::Rejected) {
        markUnreadable(*file, inspection.error, report);
        if (!alreadyLinked)
            createPublication(*file, nullptr, report);
    } else {
        const InspectResult &result = inspection.result;
        m_files.setDocumentFacts(file->id, result.pageCount, result.hasTextLayer);
        if (!result.problem.isEmpty()) {
            markUnreadable(*file, result.problem, report);
        } else if (result.encrypted) {
            markUnreadable(*file, QStringLiteral("needs a password"), report);
        } else if (result.blank) {
            markUnreadable(*file, QStringLiteral("opened, but has no visible content"), report);
        } else if (file->status == FileStatus::Unreadable) {
            m_files.setStatus(file->id, FileStatus::Ok);
        }
        if (!alreadyLinked)
            createPublication(*file, &result, report);
    }
    finishJob(job);
    transaction.commit();
}

// Takes a publication out of Preprints, if only an earlier version's automatic sorting put it there.
// If the file can't be read now, its type is worked out from what's already known about it. A type
// the user chose is never touched, including if they've marked something as a preprint themselves.
void ImportService::processReclassify(const Job &job, ImportReport &report)
{
    const std::optional<FileRecord> file = m_files.get(job.fileId);
    const QList<qint64> publications = file ? m_files.publicationsFor(file->id) : QList<qint64>();
    if (!file || publications.isEmpty()) {
        finishJob(job);
        return;
    }

    InspectResult found;
    bool gaveUp = false;
    const QString path = m_files.absolutePath(file->id);
    if (QFile::exists(path)) {
        const WorkerClient::Inspection inspection = m_worker.inspect(
            path, QString(), convertedPathFor(m_options.cacheDirectory, file->sha256), m_options.inspectTimeoutMs);
        if (inspection.failure == WorkerFailure::Cancelled) {
            requeueJob(job);
            return;
        }
        if (inspection.failure == WorkerFailure::Crashed || inspection.failure == WorkerFailure::TimedOut) {
            if (!retryOrFailJob(job, inspection.error))
                return;
            gaveUp = true;
        } else if (inspection.failure == WorkerFailure::None) {
            found = inspection.result;
        }
    }

    Transaction transaction(m_db);
    const QString firstPage = found.text.section(u'\f', 0, 0);
    const QString laterPages = found.text.section(u'\f', 1);
    for (const qint64 id : publications) {
        std::optional<Publication> p = m_publications.get(id);
        if (!p || p->type != PublicationType::Preprint)
            continue;
        const QList<ProvenanceEntry> history = m_publications.provenance(id, QStringLiteral("type"));
        if (std::any_of(history.cbegin(), history.cend(),
                        [](const ProvenanceEntry &entry) { return entry.source == QLatin1StringView("manual"); })) {
            continue;
        }

        const Classification classification = classify(
            {file->format, found.pageCount ? found.pageCount : file->pageCount, firstPage, laterPages, p->title, p->doi,
             p->isbn13.isEmpty() ? p->isbn10 : p->isbn13, p->issn, p->arxivId});
        p->type = classification.type;
        p->needsReview = p->needsReview || !classification.confident;
        m_publications.updateWithProvenance(*p, QStringLiteral("classifier"));
        ++report.reclassified;
    }
    if (!gaveUp)
        finishJob(job);
    transaction.commit();
}

bool ImportService::holdsAnyFormat(qint64 publicationId, const QStringList &formats)
{
    const QStringList held = formatsOf(m_files.filesFor(publicationId));
    return std::any_of(formats.cbegin(), formats.cend(), [&](const QString &format) { return held.contains(format); });
}

// Looks for a publication that this one is another format of. Two copies in one format aren't that:
// they may be different editions or scans, so they stay apart unless they're the same contents.
//
// In order of confidence:
//   1. The same name in the same folder, differing only by extension. That's someone's own filing.
//   2. The same DOI or ISBN, and a title that agrees. The title's checked because a chapter or
//      article can carry the identifier of the book or volume it came from.
//   3. The same title, first author and year, with no identifier to go on. This is a judgement, so the
//      result gets marked for checking.
std::optional<ImportService::OtherFormat> ImportService::findOtherFormat(const Publication &publication, qint64 selfId,
                                                                         const QStringList &formats,
                                                                         const QList<FileRecord> &files)
{
    if (publication.parentId)
        return std::nullopt;

    const auto usable = [&](qint64 id) -> std::optional<Publication> {
        if (id == selfId || holdsAnyFormat(id, formats))
            return std::nullopt;
        std::optional<Publication> candidate = m_publications.get(id);
        if (!candidate || candidate->parentId || !m_publications.childIds(id).isEmpty())
            return std::nullopt;
        return candidate;
    };

    for (const FileRecord &file : files) {
        const QFileInfo info(file.relativePath);
        const QString base = info.completeBaseName();
        auto sameName = m_db.prepare(
            "SELECT id, relative_path FROM file WHERE root_id = ? AND file_name >= ? AND file_name < ?");
        sameName->bind(1, file.rootId);
        sameName->bind(2, base + u'.');
        sameName->bind(3, base + u'/'); // The character after the full stop.
        while (sameName->step()) {
            const QFileInfo other(sameName->textAt(1));
            if (other.path() != info.path() || other.completeBaseName() != base)
                continue;
            for (const qint64 id : m_files.publicationsFor(sameName->int64At(0))) {
                if (usable(id))
                    return OtherFormat{id, true};
            }
        }
    }

    const auto byIdentifier = [&](const char *sql, const QString &value) -> std::optional<OtherFormat> {
        if (value.isEmpty())
            return std::nullopt;
        QList<qint64> ids;
        {
            auto select = m_db.prepare(sql);
            select->bind(1, value);
            while (select->step())
                ids << select->int64At(0);
        }
        for (const qint64 id : ids) {
            const std::optional<Publication> candidate = usable(id);
            if (candidate && sameMainTitle(publication, *candidate))
                return OtherFormat{id, true};
        }
        return std::nullopt;
    };
    if (const auto found = byIdentifier("SELECT id FROM publication WHERE doi = ?", publication.doi))
        return found;
    if (const auto found = byIdentifier("SELECT id FROM publication WHERE isbn13 = ?", publication.isbn13))
        return found;
    if (const auto found = byIdentifier("SELECT id FROM publication WHERE isbn10 = ?", publication.isbn10))
        return found;

    const QString key = mainTitleKey(publication.title);
    if (!isDistinctiveTitle(key) || publication.authors.isEmpty() || publication.authors.first().family.isEmpty())
        return std::nullopt;
    QList<qint64> sameStart;
    {
        auto select = m_db.prepare("SELECT id FROM publication WHERE title_sort >= ? AND title_sort < ? LIMIT 50");
        select->bind(1, key);
        select->bind(2, key + QString::fromUcs4(U"\U0010FFFF"));
        while (select->step())
            sameStart << select->int64At(0);
    }
    for (const qint64 id : sameStart) {
        const std::optional<Publication> candidate = usable(id);
        if (!candidate || !sameMainTitle(publication, *candidate) || candidate->authors.isEmpty())
            continue;
        if (candidate->authors.first().family.compare(publication.authors.first().family, Qt::CaseInsensitive) != 0)
            continue;
        if (candidate->year && publication.year && *candidate->year != *publication.year)
            continue;
        if (!candidate->edition.isEmpty() && !publication.edition.isEmpty() && candidate->edition != publication.edition)
            continue;
        return OtherFormat{id, false};
    }
    return std::nullopt;
}

// Looks through the library once, for formats imported as separate publications before they could be
// recognised. What's joined is what a new import would have joined.
void ImportService::processJoinFormats(const Job &job, ImportReport &report)
{
    QList<qint64> ids;
    {
        auto select = m_db.prepare("SELECT DISTINCT publication_id FROM publication_file ORDER BY publication_id");
        while (select->step())
            ids << select->int64At(0);
    }

    for (const qint64 id : ids) {
        const std::optional<Publication> publication = m_publications.get(id);
        if (!publication || !m_publications.childIds(id).isEmpty())
            continue;
        const QList<FileRecord> files = m_files.filesFor(id);
        const std::optional<OtherFormat> other = findOtherFormat(*publication, id, formatsOf(files), files);
        if (!other)
            continue;

        Transaction transaction(m_db);
        mergePublications(m_db, other->publicationId, id, QStringLiteral("import"));
        if (!other->certain)
            m_publications.setNeedsReview({other->publicationId}, true);
        transaction.commit();
        ++report.formatsJoined;
    }
    finishJob(job);
}

void ImportService::createPublication(const FileRecord &file, const InspectResult *inspection, ImportReport &report)
{
    // A file that keeps failing can come through here again after a rescan.
    if (!m_files.publicationsFor(file.id).isEmpty())
        return;

    // With no inspection, the file couldn't be read at all, so it's listed under its file name.
    static const InspectResult nothing;
    const InspectResult &found = inspection ? *inspection : nothing;
    const EmbeddedMetadata &meta = found.metadata;

    // Identifiers are only believed near the start. Later pages hold reference lists, and an
    // identifier there belongs to a cited work rather than to this one.
    const QString firstPage = found.text.section(u'\f', 0, 0);
    const QString firstTwoPages = found.text.section(u'\f', 0, 1);
    const QString laterPages = found.text.section(u'\f', 1);

    Publication p;
    bool titleIsReliable = true;
    QString titleSource;
    const QString fileTitle = titleFromFileName(QFileInfo(file.relativePath).fileName());
    if (!meta.title.isEmpty() && !isJunkTitle(meta.title)) {
        p.title = meta.title;
        titleSource = QStringLiteral("embedded");
    } else if (looksLikeFullTitle(found.largestText) && found.largestText.size() <= 300) {
        p.title = found.largestText;
        titleSource = QStringLiteral("first_page");
        titleIsReliable = false;
    } else if (found.largestText.size() >= 4 && !looksLikeFullTitle(fileTitle)) {
        // A short heading, and a file name that's no better. "Example.pdf" is an example.
        p.title = found.largestText;
        titleSource = QStringLiteral("first_page");
        titleIsReliable = false;
    } else {
        p.title = fileTitle;
        titleSource = QStringLiteral("file_name");
        titleIsReliable = false;
    }

    for (const QString &entry : meta.authors) {
        for (const QString &author : splitAuthorList(entry)) {
            if (!isJunkAuthor(author))
                p.authors << parsePersonName(author);
        }
    }

    p.doi = meta.doi.isEmpty() ? findDoi(firstTwoPages) : meta.doi;

    // An ISBN found inside a short document belongs to the proceedings or book it's part of.
    const bool longEnoughForIsbn = !found.pageCount || *found.pageCount >= 50;
    const QString isbn = !meta.isbn.isEmpty() ? meta.isbn : (longEnoughForIsbn ? findIsbn(found.text) : QString());
    if (isbn.size() == 13)
        p.isbn13 = isbn;
    else
        p.isbn10 = isbn;
    p.issn = meta.issn.isEmpty() ? findIssn(firstTwoPages) : meta.issn;
    p.arxivId = findArxivId(firstPage);
    p.publisher = meta.publisher;
    p.language = meta.language;
    p.abstract = meta.description;
    p.year = yearFromDate(meta.date);
    if (!p.year)
        p.year = findCopyrightYear(firstTwoPages);
    if (!meta.keywords.isEmpty()) {
        p.extraJson = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("keywords"), meta.keywords}})
                                            .toJson(QJsonDocument::Compact));
    }

    const Classification classification = classify({file.format, found.pageCount, firstPage, laterPages, p.title,
                                                    p.doi, isbn, p.issn, p.arxivId});
    p.type = classification.type;
    p.needsReview = !titleIsReliable || !classification.confident;

    if (const std::optional<OtherFormat> other = findOtherFormat(p, 0, {file.format}, {file})) {
        std::optional<Publication> existing = m_publications.get(other->publicationId);
        if (existing) {
            // This file may know more than the first one did. A real title replaces one that was
            // only ever a guess, as long as nobody has chosen it by hand.
            const QList<ProvenanceEntry> titles = m_publications.provenance(existing->id, QStringLiteral("title"));
            const bool titleWasGuess = !titles.isEmpty()
                && std::none_of(titles.cbegin(), titles.cend(), [](const ProvenanceEntry &entry) {
                       return entry.source == QLatin1StringView("manual") || entry.source == QLatin1StringView("embedded");
                   });
            if (titleWasGuess && titleIsReliable)
                existing->title = p.title;
            PublicationRepository::fillGaps(*existing, p);
            if (existing->type == PublicationType::Other)
                existing->type = p.type;
            if (titleWasGuess && titleIsReliable && classification.confident)
                existing->needsReview = false;
            if (!other->certain)
                existing->needsReview = true;
            m_publications.updateWithProvenance(*existing, QStringLiteral("import"));
            m_files.link(existing->id, file.id, false);
            ++report.formatsJoined;
            return;
        }
    }

    const qint64 id = m_publications.create(p);
    m_files.link(id, file.id, true);
    if (m_options.lookupMode) {
        const QString mode = m_options.lookupMode();
        if (!mode.isEmpty())
            LookupRepository(m_db).enqueue({id}, mode);
    }
    m_publications.recordProvenance(id, {QStringLiteral("title"), titleSource, std::nullopt, QString(), p.title, 0});
    ++report.publicationsCreated;
    if (p.needsReview)
        ++report.needsReview;
}

} // namespace papertrail::import
