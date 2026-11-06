#include "ui/BulkChanges.h"

#include "core/db/Database.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#include <exception>
#include <memory>

namespace papertrail::ui {

namespace {

constexpr qsizetype kBatchSize = 2000;
constexpr qint64 kProgressEveryMs = 100;

} // namespace

BulkChanges::BulkChanges(const QString &databasePath, QObject *parent)
    : QThread(parent), m_path(databasePath)
{
}

BulkChanges::~BulkChanges()
{
    {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_wake.wakeAll();
    }
    wait();
}

void BulkChanges::enqueue(const QString &description, const QList<qint64> &ids, const Change &change)
{
    QMutexLocker lock(&m_mutex);
    m_jobs << Job{description, ids, change};
    m_wake.wakeAll();
}

bool BulkChanges::isBusy()
{
    QMutexLocker lock(&m_mutex);
    return m_running || !m_jobs.isEmpty();
}

void BulkChanges::run()
{
    std::unique_ptr<core::Database> db;
    QString openError;
    try {
        db = std::make_unique<core::Database>(m_path);
    } catch (const std::exception &error) {
        openError = QString::fromUtf8(error.what());
    }

    while (true) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            m_running = false;
            while (!m_stopping && m_jobs.isEmpty())
                m_wake.wait(&m_mutex);
            // Whatever was asked for still gets done before the app closes.
            if (m_stopping && m_jobs.isEmpty())
                return;
            job = m_jobs.takeFirst();
            m_running = true;
        }

        if (!db) {
            emit failed(job.description, openError);
            continue;
        }

        try {
            QElapsedTimer sinceProgress;
            sinceProgress.start();
            for (qsizetype start = 0; start < job.ids.size(); start += kBatchSize) {
                core::Transaction transaction(*db);
                job.change(*db, job.ids.mid(start, kBatchSize));
                transaction.commit();
                if (sinceProgress.elapsed() >= kProgressEveryMs) {
                    sinceProgress.restart();
                    emit progress(job.description, qMin(start + kBatchSize, job.ids.size()), job.ids.size());
                }
            }
            emit finished(job.description, job.ids.size());
        } catch (const std::exception &error) {
            emit failed(job.description, QString::fromUtf8(error.what()));
        }
    }
}

} // namespace papertrail::ui
