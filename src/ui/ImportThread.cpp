#include "ui/ImportThread.h"

#include "core/repo/FileRepository.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>

#include <exception>
#include <memory>

namespace papertrail::ui {

using namespace core;
using namespace import;

namespace {

constexpr qint64 kLibraryChangedEveryMs = 1500;
constexpr qint64 kProgressEveryMs = 100;

QStringList foldersIn(const QString &rootPath)
{
    QStringList folders = {rootPath};
    QDirIterator it(rootPath, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext())
        folders << it.next();
    return folders;
}

} // namespace

ImportThread::ImportThread(const LibraryPaths &paths, QObject *parent)
    : QThread(parent), m_paths(paths)
{
    qRegisterMetaType<ScanReport>();
    qRegisterMetaType<ImportReport>();
}

ImportThread::~ImportThread()
{
    {
        QMutexLocker lock(&m_mutex);
        m_shuttingDown = true;
        m_stop = true;
        m_wake.wakeAll();
    }
    wait();
}

void ImportThread::scanRoots(const QList<qint64> &rootIds)
{
    QMutexLocker lock(&m_mutex);
    for (const qint64 id : rootIds) {
        if (!m_rootsToScan.contains(id))
            m_rootsToScan << id;
    }
    m_workPending = true;
    m_stop = false;
    m_wake.wakeAll();
}

void ImportThread::resumeQueue()
{
    QMutexLocker lock(&m_mutex);
    m_workPending = true;
    m_stop = false;
    m_wake.wakeAll();
}

void ImportThread::setLookupMode(const QString &mode)
{
    QMutexLocker lock(&m_lookupMutex);
    m_lookupMode = mode;
}

void ImportThread::requestStop()
{
    QMutexLocker lock(&m_mutex);
    m_stop = true;
    m_rootsToScan.clear();
    m_workPending = false;
}

void ImportThread::run()
{
    std::unique_ptr<Database> db;
    std::unique_ptr<ImportService> service;
    std::unique_ptr<FileRepository> files;
    try {
        db = std::make_unique<Database>(m_paths.database);
        ImportOptions options;
        options.workerPath = m_paths.workerPath;
        options.cacheDirectory = m_paths.cacheDirectory;
        options.settleMs = m_paths.settleMs;
        options.cancelled = [this] { return m_stop.load(); };
        options.lookupMode = [this] {
            QMutexLocker lock(&m_lookupMutex);
            return m_lookupMode;
        };
        service = std::make_unique<ImportService>(*db, options);
        files = std::make_unique<FileRepository>(*db);
    } catch (const std::exception &error) {
        emit failed(QString::fromUtf8(error.what()));
        return;
    }

    while (true) {
        QList<qint64> roots;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_shuttingDown && !m_workPending)
                m_wake.wait(&m_mutex);
            if (m_shuttingDown)
                return;
            roots = m_rootsToScan;
            m_rootsToScan.clear();
            m_workPending = false;
        }

        m_busy = true;
        emit busyChanged(true);
        try {
            for (const qint64 rootId : roots) {
                if (m_stop)
                    break;
                const std::optional<LibraryRoot> root = files->root(rootId);
                if (!root)
                    continue; // Removed while it was waiting to be scanned.
                const ScanReport report = service->scan(rootId);
                emit rootScanned(rootId, report,
                                 report.rootAvailable ? foldersIn(root->path) : QStringList());
                emit libraryChanged();
            }

            // A missing worker would make every file look unreadable, and that would stick.
            if (service->pendingJobs() > 0 && !QFileInfo(m_paths.workerPath).isExecutable()) {
                emit failed(tr("Papertrail's helper program is missing, so nothing can be imported. "
                               "Reinstalling should fix this."));
                emit finished(ImportReport{});
            } else {
                QElapsedTimer sinceChanged;
                sinceChanged.start();
                QElapsedTimer sinceProgress;
                sinceProgress.start();
                const ImportReport report = service->processQueue([&](const ImportProgress &p) {
                    if (sinceProgress.elapsed() >= kProgressEveryMs) {
                        sinceProgress.restart();
                        emit progress(p.done, p.remaining, p.currentFile);
                    }
                    if (sinceChanged.elapsed() >= kLibraryChangedEveryMs) {
                        sinceChanged.restart();
                        emit libraryChanged();
                    }
                });
                emit libraryChanged();
                emit finished(report);
            }
        } catch (const std::exception &error) {
            emit failed(QString::fromUtf8(error.what()));
        }
        m_busy = false;
        emit busyChanged(false);
    }
}

} // namespace papertrail::ui
