#pragma once

#include "metadata/LookupEngine.h"
#include "ui/LibrarySession.h"

#include <QHash>
#include <QMutex>
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

    // Asks Crossref about retractions and other notices on whatever's due, once lookups have finished. Nothing
    // is asked unless online lookups are on and checking is too. Harmless if it's already doing that.
    void checkNotices();
    bool isCheckingNotices() const { return m_checkingNotices; }

    // Asks OpenAlex for a publication's related papers, only if online lookups are on, and keeps them. relatedFound
    // or relatedFailed says how it went.
    void findRelated(qint64 publicationId, bool refresh);

    // A key of the user's own for an online source, such as metadata::kOpenAlex, held only in memory and used from the
    // next request on. Empty for none. Safe to call from any thread.
    void setKey(const QString &provider, const QString &key);
    bool hasKey(const QString &provider) const { return !keyFor(provider).isEmpty(); }

signals:
    void progress(qint64 done, qint64 remaining, const QString &source);
    void publicationChanged(qint64 publicationId);
    void reviewChanged();
    void lookupsFinished(const papertrail::metadata::LookupSummary &summary);
    void problem(const QString &message);
    // `provider` didn't accept the user's key. It's asked without one until the key is set again.
    void keyRefused(const QString &provider);
    void busyChanged(bool busy);
    void noticesChanged(const QList<qint64> &publicationIds);
    void noticesChecked(int checked, int newWarnings);

    void relatedFound(qint64 publicationId);
    void relatedFailed(qint64 publicationId, int problem, const QString &detail); // See metadata::RelatedProblem.

    void relatedRequested(qint64 publicationId, bool refresh);
    void kickRequested();
    void stopRequested();
    void noticesRequested();

protected:
    void run() override;

private:
    QString keyFor(const QString &provider) const;

    LibraryPaths m_paths;
    mutable QMutex m_keysLock;
    QHash<QString, QString> m_keys;
    std::atomic_bool m_kickWaiting = false;
    std::atomic_bool m_ready = false;
    std::atomic_bool m_busy = false;
    std::atomic_bool m_noticesWaiting = false;
    std::atomic_bool m_checkingNotices = false;
    std::atomic<qint64> m_relatedWaiting = 0; // Asked for before the thread was ready.
    std::atomic_bool m_relatedRefresh = false;
};

} // namespace papertrail::ui
