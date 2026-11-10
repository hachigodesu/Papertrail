#pragma once

#include "import/FullTextIndexer.h"
#include "import/TextRecognition.h"
#include "ui/LibrarySession.h"

#include <QMetaType>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include <atomic>

namespace papertrail::ui {

// Reads document text into the full-text index in the background. A large library takes a long time, so it's
// separate from importing, and nothing else waits for it. Stopping is always safe.
class IndexThread : public QThread {
    Q_OBJECT

public:
    explicit IndexThread(const LibraryPaths &paths, QObject *parent = nullptr);
    ~IndexThread() override;

    // Looks for anything not yet indexed. Harmless if it's already at work.
    void kick();
    void stopIndexing();
    bool isBusy() const { return m_busy; }

signals:
    void busyChanged(bool busy);
    void progress(qint64 done, qint64 remaining, const QString &fileName);
    void finished(const papertrail::import::FullTextReport &report);
    // Recognising the words of scanned documents, after indexing: how many documents are left, and where it is.
    void recognitionProgress(qint64 documentsLeft, int page, int pageCount, const QString &fileName);
    void recognitionFinished(const papertrail::import::RecognitionReport &report);
    void problem(const QString &message);

protected:
    void run() override;

private:
    LibraryPaths m_paths;
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_kicked = false;
    bool m_closing = false;
    std::atomic_bool m_stop = false;
    std::atomic_bool m_busy = false;
};

} // namespace papertrail::ui

Q_DECLARE_METATYPE(papertrail::import::FullTextReport)
Q_DECLARE_METATYPE(papertrail::import::RecognitionReport)
