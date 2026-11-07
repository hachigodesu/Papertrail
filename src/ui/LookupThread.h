#pragma once

#include "metadata/LookupEngine.h"
#include "ui/LibrarySession.h"

#include <QString>
#include <QThread>

#include <atomic>

namespace papertrail::ui {

// Runs online lookups away from the window. It only ever works on what's in the queue, so with nothing
// queued, which is always the case in offline mode, nothing here touches the network.
class LookupThread : public QThread {
    Q_OBJECT

public:
    explicit LookupThread(const LibraryPaths &paths, QObject *parent = nullptr);
    ~LookupThread() override;

    // Works through whatever's queued. Harmless if it's already doing that.
    void kick();
    void stopLookups();
    bool isBusy() const { return m_busy; }

signals:
    void progress(qint64 done, qint64 remaining, const QString &source);
    void publicationChanged(qint64 publicationId);
    void reviewChanged();
    void lookupsFinished(const papertrail::metadata::LookupSummary &summary);
    void problem(const QString &message);
    void busyChanged(bool busy);

    void kickRequested();
    void stopRequested();

protected:
    void run() override;

private:
    LibraryPaths m_paths;
    std::atomic_bool m_kickWaiting = false;
    std::atomic_bool m_ready = false;
    std::atomic_bool m_busy = false;
};

} // namespace papertrail::ui
