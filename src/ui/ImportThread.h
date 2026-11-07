#pragma once

#include "import/ImportService.h"
#include "ui/LibrarySession.h"

#include <QList>
#include <QMetaType>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QWaitCondition>

#include <atomic>

namespace papertrail::ui {

// Runs scans and the import queue in the background. Stopping is always safe: the queue is kept
// in the database, and carries on from there next time.
class ImportThread : public QThread {
    Q_OBJECT

public:
    explicit ImportThread(const LibraryPaths &paths, QObject *parent = nullptr);
    ~ImportThread() override;

    // Scans these folders, then works through the queue.
    void scanRoots(const QList<qint64> &rootIds);

    // Works through whatever's in the queue, such as an import left over from last time.
    void resumeQueue();

    void requestStop();

    // How new publications get looked up online. Empty means they aren't, and nothing's sent anywhere.
    void setLookupMode(const QString &mode);
    bool isBusy() const { return m_busy; }

signals:
    void busyChanged(bool busy);
    void progress(qint64 done, qint64 remaining, const QString &file);
    void rootScanned(qint64 rootId, const papertrail::import::ScanReport &report, const QStringList &folders);

    // New publications may be there to show. Sent now and then during an import.
    void libraryChanged();

    void finished(const papertrail::import::ImportReport &report);
    void failed(const QString &message);

protected:
    void run() override;

private:
    LibraryPaths m_paths;
    QMutex m_lookupMutex;
    QString m_lookupMode;
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_shuttingDown = false;
    QList<qint64> m_rootsToScan;
    bool m_workPending = false;
    std::atomic_bool m_stop = false;
    std::atomic_bool m_busy = false;
};

} // namespace papertrail::ui

Q_DECLARE_METATYPE(papertrail::import::ScanReport)
Q_DECLARE_METATYPE(papertrail::import::ImportReport)
