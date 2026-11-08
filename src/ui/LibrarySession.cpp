#include "ui/LibrarySession.h"

#include "import/JournalIssues.h"
#include "core/repo/FullTextIndex.h"

#include "core/db/Backups.h"
#include "core/db/Schema.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace papertrail::ui {

LibraryPaths LibraryPaths::standard()
{
    LibraryPaths paths;
    paths.database = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                         .filePath(QStringLiteral("library.db"));
    paths.cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
#ifdef Q_OS_WIN
    const QString workerName = QStringLiteral("papertrail-worker.exe");
#else
    const QString workerName = QStringLiteral("papertrail-worker");
#endif
    paths.workerPath = QDir(QCoreApplication::applicationDirPath()).filePath(workerName);
    return paths;
}

LibrarySession::LibrarySession(const LibraryPaths &paths)
    : m_paths(paths)
{
    QDir().mkpath(QFileInfo(paths.database).absolutePath());
    QDir().mkpath(paths.cacheDirectory);

    // Two copies working on one library would each think the other's import jobs had been
    // interrupted, and do them again.
    m_lock = std::make_unique<QLockFile>(paths.database + QStringLiteral(".lock"));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0))
        throw LibraryInUse("this library is already open in another copy of Papertrail");

    m_db = std::make_unique<core::Database>(paths.database);
    // A copy of the library as it was, before a newer version changes its layout. Without one, it isn't changed.
    try {
        core::backUpBeforeUpgrade(*m_db, paths.database);
    } catch (const core::DbError &error) {
        throw core::DbError(error.code(), std::string("it couldn't be backed up before being upgraded: ") + error.what());
    }
    core::migrateLibrary(*m_db);
    core::attachFullText(*m_db, core::fullTextPathFor(paths.database));
    // Issues titled from their covers before this was done get their proper titles.
    try {
        import::tidyJournalIssues(*m_db);
    } catch (const std::exception &) {
        // Only tidying: the library opens either way.
    }
    m_publications = std::make_unique<core::PublicationRepository>(*m_db);
    m_tags = std::make_unique<core::TagRepository>(*m_db);
    m_collections = std::make_unique<core::CollectionRepository>(*m_db);
    m_files = std::make_unique<core::FileRepository>(*m_db);
    m_settings = std::make_unique<core::SettingsStore>(*m_db);
    m_query = std::make_unique<core::LibraryQuery>(*m_db);
}

} // namespace papertrail::ui
