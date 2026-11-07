#include "ui/IndexThread.h"

#include "core/db/Database.h"
#include "core/repo/FullTextIndex.h"

#include <QMutexLocker>

#include <exception>
#include <memory>

namespace papertrail::ui {

IndexThread::IndexThread(const LibraryPaths &paths, QObject *parent)
    : QThread(parent), m_paths(paths)
{
    qRegisterMetaType<import::FullTextReport>();
}

IndexThread::~IndexThread()
{
    {
        QMutexLocker lock(&m_mutex);
        m_closing = true;
        m_stop = true;
        m_wake.wakeAll();
    }
    wait();
}

void IndexThread::kick()
{
    QMutexLocker lock(&m_mutex);
    m_kicked = true;
    m_stop = false;
    m_wake.wakeAll();
}

void IndexThread::stopIndexing()
{
    QMutexLocker lock(&m_mutex);
    m_kicked = false;
    m_stop = true;
}

void IndexThread::run()
{
    std::unique_ptr<core::Database> db;
    try {
        db = std::make_unique<core::Database>(m_paths.database);
        core::attachFullText(*db, core::fullTextPathFor(m_paths.database));
    } catch (const std::exception &error) {
        emit problem(QString::fromUtf8(error.what()));
        return;
    }

    import::FullTextOptions options;
    options.workerPath = m_paths.workerPath;
    options.cacheDirectory = m_paths.cacheDirectory;
    options.cancelled = [this] { return m_stop.load(); };

    while (true) {
        {
            QMutexLocker lock(&m_mutex);
            while (!m_kicked && !m_closing)
                m_wake.wait(&m_mutex);
            if (m_closing)
                return;
            m_kicked = false;
        }

        m_busy = true;
        emit busyChanged(true);
        import::FullTextReport report;
        try {
            import::FullTextIndexer indexer(*db, options);
            report = indexer.run([this](qint64 done, qint64 remaining, const QString &file) { emit progress(done, remaining, file); });
        } catch (const std::exception &error) {
            emit problem(QString::fromUtf8(error.what()));
        }
        m_busy = false;
        emit busyChanged(false);
        emit finished(report);
    }
}

} // namespace papertrail::ui
