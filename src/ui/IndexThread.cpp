#include "ui/IndexThread.h"

#include "core/settings/SettingsStore.h"

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
    qRegisterMetaType<import::RecognitionReport>();
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
        const auto index = [&] {
            try {
                import::FullTextIndexer indexer(*db, options);
                report = indexer.run([this](qint64 done, qint64 remaining, const QString &file) { emit progress(done, remaining, file); });
            } catch (const std::exception &error) {
                emit problem(QString::fromUtf8(error.what()));
            }
        };
        index();

        // Then the words of scanned documents, which takes far longer, if this computer's to do it.
        if (canRecogniseText() && !m_paths.tessdataDirectory.isEmpty() && !report.stopped && !m_stop) {
            try {
                core::SettingsStore settings(*db);
                if (settings.value(QStringLiteral("ocr.enabled")).toBool()) {
                    prepareTessdata(m_paths.tessdataDirectory);
                    import::RecognitionOptions recognition;
                    recognition.workerPath = m_paths.workerPath;
                    recognition.cacheDirectory = m_paths.cacheDirectory;
                    recognition.tessdata = m_paths.tessdataDirectory;
                    recognition.languages = settings.value(QStringLiteral("ocr.languages")).toString();
                    recognition.cancelled = options.cancelled;
                    if (recognition.languages.isEmpty())
                        recognition.languages = QStringLiteral("eng");
                    import::TextRecognition recogniser(*db, recognition);
                    const import::RecognitionReport recognised = recogniser.run(
                        [this](qint64 left, int page, int pageCount, const QString &file) { emit recognitionProgress(left, page, pageCount, file); });
                    if (!recognised.problem.isEmpty())
                        emit problem(recognised.problem);
                    emit recognitionFinished(recognised);
                    // What was recognised is read into the index, so it can be searched.
                    if (recognised.pages > 0 && !m_stop)
                        index();
                }
            } catch (const std::exception &error) {
                emit problem(QString::fromUtf8(error.what()));
            }
        }
        m_busy = false;
        emit busyChanged(false);
        emit finished(report);
    }
}

} // namespace papertrail::ui
