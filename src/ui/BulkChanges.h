#pragma once

#include <QList>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <functional>

namespace papertrail::core {
class Database;
}

namespace papertrail::ui {

// Applies a change to many publications away from the window's thread. Tagging a whole library
// takes several seconds, because each one updates the search index. It's done in small batches, so
// that edits made in the meantime only ever have a moment to wait.
class BulkChanges : public QThread {
    Q_OBJECT

public:
    explicit BulkChanges(const QString &databasePath, QObject *parent = nullptr);
    ~BulkChanges() override;

    using Change = std::function<void(core::Database &db, const QList<qint64> &batch)>;

    // Changes run one at a time, in the order they were asked for.
    void enqueue(const QString &description, const QList<qint64> &ids, const Change &change);

    bool isBusy();

signals:
    void progress(const QString &description, qint64 done, qint64 total);
    void finished(const QString &description, qint64 total);
    void failed(const QString &description, const QString &error);

protected:
    void run() override;

private:
    struct Job {
        QString description;
        QList<qint64> ids;
        Change change;
    };

    QString m_path;
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_stopping = false;
    bool m_running = false;
    QList<Job> m_jobs;
};

} // namespace papertrail::ui
