#include "ui/ArticleCoverThread.h"

#include "core/db/Database.h"
#include "core/model/Publication.h"
#include "core/repo/FileRepository.h"
#include "import/ImportService.h"
#include "import/WorkerClient.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <exception>
#include <memory>

namespace papertrail::ui {

namespace {

constexpr int kCoverTimeoutMs = 30000;

} // namespace

ArticleCoverThread::ArticleCoverThread(const LibraryPaths &paths, QObject *parent)
    : QThread(parent), m_paths(paths)
{
}

ArticleCoverThread::~ArticleCoverThread()
{
    {
        QMutexLocker lock(&m_mutex);
        m_closing = true;
        m_waiting.clear();
        m_wake.wakeAll();
    }
    wait();
}

void ArticleCoverThread::request(const QString &key)
{
    if (core::coverKeyPage(key) < 1)
        return;
    QMutexLocker lock(&m_mutex);
    if (m_tried.contains(key))
        return;
    m_waiting.removeOne(key);
    m_waiting << key;
    if (m_waiting.size() > kMaxWaiting)
        m_waiting.removeFirst(); // Asked for again if it comes back into view.
    m_wake.wakeAll();
}

void ArticleCoverThread::run()
{
    std::unique_ptr<core::Database> db;
    try {
        db = std::make_unique<core::Database>(m_paths.database);
    } catch (const std::exception &) {
        return; // Articles keep showing their volume's cover.
    }
    import::WorkerClient worker(m_paths.workerPath, [this] { return m_closing.load(); });

    while (true) {
        QString key;
        {
            QMutexLocker lock(&m_mutex);
            while (m_waiting.isEmpty() && !m_closing)
                m_wake.wait(&m_mutex);
            if (m_closing)
                return;
            key = m_waiting.takeLast();
            m_tried.insert(key);
        }

        const QString hash = core::coverKeyHash(key);
        const QString coverPath = import::coverPathFor(m_paths.cacheDirectory, key);
        if (QFileInfo::exists(coverPath)) {
            emit made(key);
            continue;
        }

        // Any readable copy will do, since they all have the same contents.
        QString path;
        try {
            core::FileRepository files(*db);
            for (const core::FileRecord &file : files.withHash(hash)) {
                const QString candidate = files.absolutePath(file.id);
                if (file.status == core::FileStatus::Ok && QFileInfo::exists(candidate)) {
                    path = candidate;
                    break;
                }
            }
        } catch (const std::exception &) {
            continue;
        }
        if (path.isEmpty())
            continue;

        QDir().mkpath(QFileInfo(coverPath).absolutePath());
        const import::WorkerClient::Cover cover = worker.pageCover(
            path, import::convertedPathFor(m_paths.cacheDirectory, hash), core::coverKeyPage(key) - 1, coverPath, kCoverTimeoutMs);
        if (cover.failure == import::WorkerFailure::None && QFileInfo::exists(coverPath))
            emit made(key);
    }
}

} // namespace papertrail::ui
