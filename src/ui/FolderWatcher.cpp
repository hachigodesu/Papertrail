#include "ui/FolderWatcher.h"

namespace papertrail::ui {

namespace {

// Each watched folder uses up a limited system resource.
constexpr qsizetype kMaxWatched = 5000;

// A scan takes up at most about 1% of the time, however slow the disk is.
constexpr qint64 kPollTimeToScanRatio = 100;

} // namespace

FolderWatcher::FolderWatcher(QObject *parent)
    : QObject(parent)
{
    m_quiet.setSingleShot(true);
    m_quiet.setInterval(3000);
    m_recheckTimer.setSingleShot(true);
    m_recheckTimer.setInterval(30000);
    m_pollTimer.setInterval(m_pollIntervalMs);
    m_sincePoll.start();

    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FolderWatcher::onFolderChanged);
    connect(&m_quiet, &QTimer::timeout, this, [this] {
        const QList<qint64> roots(m_changed.cbegin(), m_changed.cend());
        m_changed.clear();
        if (!roots.isEmpty())
            emit rootsChanged(roots);
    });
    connect(&m_recheckTimer, &QTimer::timeout, this, [this] {
        const QList<qint64> roots(m_recheck.cbegin(), m_recheck.cend());
        m_recheck.clear();
        if (!roots.isEmpty())
            emit rootsChanged(roots);
    });
    connect(&m_pollTimer, &QTimer::timeout, this, &FolderWatcher::poll);
}

void FolderWatcher::setDelays(int quietMs, int recheckMs)
{
    m_quiet.setInterval(quietMs);
    m_recheckTimer.setInterval(recheckMs);
}

void FolderWatcher::setPollDelays(int intervalMs, int minimumGapMs)
{
    m_pollIntervalMs = intervalMs;
    m_pollGapMs = minimumGapMs;
    m_pollTimer.setInterval(intervalMs);
    updatePollTimer();
}

bool FolderWatcher::isPolling() const
{
#ifdef Q_OS_WIN
    return true;
#else
    return m_failed > 0;
#endif
}

void FolderWatcher::updatePollTimer()
{
    if (m_enabled && isPolling() && !m_requested.isEmpty()) {
        if (!m_pollTimer.isActive())
            m_pollTimer.start();
    } else {
        m_pollTimer.stop();
    }
}

void FolderWatcher::poll()
{
    if (!m_enabled || m_requested.isEmpty())
        return;
    m_sincePoll.restart();
    emit pollDue(m_requested.keys());
}

void FolderWatcher::windowActivated()
{
    if (m_enabled && isPolling() && m_sincePoll.elapsed() >= m_pollGapMs)
        poll();
}

void FolderWatcher::noteScanDuration(qint64 milliseconds)
{
    const qint64 interval = qMax<qint64>(m_pollIntervalMs, milliseconds * kPollTimeToScanRatio);
    m_pollTimer.setInterval(int(qMin<qint64>(interval, 24 * 60 * 60 * 1000)));
}

void FolderWatcher::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        if (!m_watcher.directories().isEmpty())
            m_watcher.removePaths(m_watcher.directories());
        m_rootForFolder.clear();
        m_changed.clear();
        m_failed = 0;
        updatePollTimer();
        return;
    }
    const QHash<qint64, QStringList> requested = m_requested;
    for (auto it = requested.constBegin(); it != requested.constEnd(); ++it)
        setFolders(it.key(), it.value());
}

void FolderWatcher::setFolders(qint64 rootId, const QStringList &folders)
{
    m_requested.insert(rootId, folders);
    if (!m_enabled)
        return;

    const QSet<QString> wanted(folders.cbegin(), folders.cend());
    QStringList toRemove;
    for (auto it = m_rootForFolder.cbegin(); it != m_rootForFolder.cend(); ++it) {
        if (it.value() == rootId && !wanted.contains(it.key()))
            toRemove << it.key();
    }
    for (const QString &path : toRemove)
        m_rootForFolder.remove(path);

    QStringList toAdd;
    for (const QString &folder : folders) {
        if (!m_rootForFolder.contains(folder))
            toAdd << folder;
    }

#ifdef Q_OS_WIN
    for (const QString &folder : toAdd)
        m_rootForFolder.insert(folder, rootId);
#else
    if (!toRemove.isEmpty())
        m_watcher.removePaths(toRemove);
    if (!toAdd.isEmpty()) {
        const qsizetype room = qMax<qsizetype>(0, kMaxWatched - m_rootForFolder.size());
        m_failed += int(qMax<qsizetype>(0, toAdd.size() - room));
        toAdd = toAdd.first(qMin(room, toAdd.size()));
        const QStringList failed = m_watcher.addPaths(toAdd);
        const QSet<QString> failedSet(failed.cbegin(), failed.cend());
        for (const QString &folder : toAdd) {
            if (!failedSet.contains(folder))
                m_rootForFolder.insert(folder, rootId);
        }
        m_failed += int(failed.size());
    }
#endif
    updatePollTimer();
}

void FolderWatcher::removeRoot(qint64 rootId)
{
    m_requested.remove(rootId);
    QStringList toRemove;
    for (auto it = m_rootForFolder.cbegin(); it != m_rootForFolder.cend(); ++it) {
        if (it.value() == rootId)
            toRemove << it.key();
    }
#ifndef Q_OS_WIN
    if (!toRemove.isEmpty())
        m_watcher.removePaths(toRemove);
#endif
    for (const QString &path : toRemove)
        m_rootForFolder.remove(path);
    m_changed.remove(rootId);
    m_recheck.remove(rootId);
    updatePollTimer();
}

void FolderWatcher::scheduleRecheck(qint64 rootId)
{
    m_recheck.insert(rootId);
    if (!m_recheckTimer.isActive())
        m_recheckTimer.start();
}

void FolderWatcher::onFolderChanged(const QString &path)
{
    const auto root = m_rootForFolder.constFind(path);
    if (root == m_rootForFolder.constEnd())
        return;
    m_changed.insert(root.value());
    m_quiet.start();
}

} // namespace papertrail::ui
