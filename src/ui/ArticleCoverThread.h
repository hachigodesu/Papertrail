#pragma once

#include "ui/LibrarySession.h"

#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QWaitCondition>

#include <atomic>

namespace papertrail::ui {

// Makes covers for articles from the page each starts on, as they're first shown. The file is read by the
// worker, so a bad page costs only that cover. The newest request goes first, so scrolling quickly doesn't
// leave a backlog of covers that are no longer on screen.
class ArticleCoverThread : public QThread {
    Q_OBJECT

public:
    explicit ArticleCoverThread(const LibraryPaths &paths, QObject *parent = nullptr);
    ~ArticleCoverThread() override;

    // `key` is from core::articleCoverKey. Each is only tried once while the app is open.
    void request(const QString &key);

signals:
    void made(const QString &key);

protected:
    void run() override;

private:
    static constexpr int kMaxWaiting = 64;

    LibraryPaths m_paths;
    QMutex m_mutex;
    QWaitCondition m_wake;
    QStringList m_waiting;
    QSet<QString> m_tried;
    std::atomic_bool m_closing = false;
};

} // namespace papertrail::ui
