#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

namespace papertrail::ui {

// Notices changes in the library's folders, and says which to scan again.
//
// Linux gets told about changes as they happen. Windows only offers that through an open handle on
// each folder, which stops people renaming or moving their own folders while Papertrail is running.
// So on Windows, this instead asks for a scan every few minutes and whenever the user comes back to
// the window, as they will after copying files in. Linux does the same if it runs out of watches.
//
// Either way this is only a convenience: a scan at startup is what guarantees nothing gets missed.
class FolderWatcher : public QObject {
    Q_OBJECT

public:
    explicit FolderWatcher(QObject *parent = nullptr);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Replaces the folders covered for one library location.
    void setFolders(qint64 rootId, const QStringList &folders);
    void removeRoot(qint64 rootId);

    // Asks for another scan after a delay, however many times it's called in between. For files
    // that were still being copied during the last scan.
    void scheduleRecheck(qint64 rootId);

    // The user has come back to the window.
    void windowActivated();

    // How long the last scan took, so that a slow disk isn't kept busy with scans.
    void noteScanDuration(qint64 milliseconds);

    void setDelays(int quietMs, int recheckMs);
    void setPollDelays(int intervalMs, int minimumGapMs);

    int watchedCount() const { return int(m_rootForFolder.size()); }
    int failedCount() const { return m_failed; }
    bool isPolling() const;

signals:
    // Something has changed in these locations, and they should be scanned.
    void rootsChanged(const QList<qint64> &rootIds);

    // These locations are due a routine look. Nothing is known to have changed.
    void pollDue(const QList<qint64> &rootIds);

private:
    void onFolderChanged(const QString &path);
    void poll();
    void updatePollTimer();

    QFileSystemWatcher m_watcher;
    QHash<QString, qint64> m_rootForFolder;
    QHash<qint64, QStringList> m_requested; // Kept while disabled, for turning back on.
    QSet<qint64> m_changed;
    QSet<qint64> m_recheck;
    QTimer m_quiet;
    QTimer m_recheckTimer;
    QTimer m_pollTimer;
    QElapsedTimer m_sincePoll;
    int m_pollIntervalMs = 5 * 60 * 1000;
    int m_pollGapMs = 60 * 1000;
    bool m_enabled = true;
    int m_failed = 0;
};

} // namespace papertrail::ui
