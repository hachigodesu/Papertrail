#include "ui/LibrarySession.h"

#include "import/JournalIssues.h"
#include "core/repo/FullTextIndex.h"

#include "core/db/Backups.h"
#include "core/db/Schema.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace papertrail::ui {

namespace {

QString installedWorker()
{
#ifdef Q_OS_WIN
    const QString workerName = QStringLiteral("papertrail-worker.exe");
#else
    const QString workerName = QStringLiteral("papertrail-worker");
#endif
    // Beside the program, as every package but the AppImage has it. An AppImage starts the program from its own
    // root, with the worker in usr/bin, and its runtime says where that root is in APPDIR.
    const QDir program(QCoreApplication::applicationDirPath());
    QStringList candidates{program.filePath(workerName)};
    if (const QString appDir = qEnvironmentVariable("APPDIR"); !appDir.isEmpty())
        candidates << QDir(appDir).filePath(QStringLiteral("usr/bin/") + workerName);
    candidates << program.filePath(QStringLiteral("usr/bin/") + workerName);
    for (const QString &candidate : std::as_const(candidates)) {
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return candidates.first();
}

} // namespace

LibraryPaths LibraryPaths::standard()
{
    LibraryPaths paths;
    paths.database = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                         .filePath(QStringLiteral("library.db"));
    paths.cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    paths.workerPath = installedWorker();
    paths.tessdataDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("tessdata"));
    return paths;
}

LibraryPaths LibraryPaths::inFolder(const QString &folder)
{
    LibraryPaths paths;
    const QDir dir(QDir::cleanPath(QDir(folder).absolutePath()));
    paths.database = dir.filePath(QStringLiteral("library.db"));
    paths.cacheDirectory = dir.filePath(QStringLiteral("cache"));
    paths.workerPath = installedWorker();
    paths.tessdataDirectory = dir.filePath(QStringLiteral("tessdata"));
    return paths;
}

bool canRecogniseText()
{
#ifdef PAPERTRAIL_WITH_OCR
    return true;
#else
    return false;
#endif
}

QString bundledTessdata()
{
    if (const QString chosen = qEnvironmentVariable("PAPERTRAIL_TESSDATA"); !chosen.isEmpty())
        return chosen;
    const QDir program(QCoreApplication::applicationDirPath());
    QStringList places = {program.filePath(QStringLiteral("tessdata")),
                          program.filePath(QStringLiteral("../share/papertrail/tessdata"))};
    // An AppImage starts the program from its own root, as for the worker.
    if (const QString appDir = qEnvironmentVariable("APPDIR"); !appDir.isEmpty())
        places << QDir(appDir).filePath(QStringLiteral("usr/share/papertrail/tessdata"));
    places << program.filePath(QStringLiteral("usr/share/papertrail/tessdata"));
#if defined(PAPERTRAIL_SOURCE_TESSDATA) && defined(PAPERTRAIL_BUILD_DIR)
    // The checkout's own, only for a program run from where it was built, so that a package without English can't
    // pass its self-test by finding it in the folder it was built from.
    const QString built = QDir::cleanPath(QStringLiteral(PAPERTRAIL_BUILD_DIR)) + u'/';
    if ((QDir::cleanPath(program.absolutePath()) + u'/').startsWith(built, Qt::CaseInsensitive))
        places << QStringLiteral(PAPERTRAIL_SOURCE_TESSDATA);
#endif
    for (const QString &place : places) {
        if (QFileInfo::exists(QDir(place).filePath(QStringLiteral("eng.traineddata"))))
            return QDir::cleanPath(place);
    }
    return {};
}

void prepareTessdata(const QString &tessdata)
{
    const QString bundled = bundledTessdata();
    if (bundled.isEmpty() || tessdata.isEmpty() || QDir::cleanPath(tessdata) == bundled)
        return;
    QDir().mkpath(tessdata);
    for (const QFileInfo &model : QDir(bundled).entryInfoList({QStringLiteral("*.traineddata")}, QDir::Files)) {
        const QString target = QDir(tessdata).filePath(model.fileName());
        if (!QFileInfo::exists(target) || QFileInfo(target).size() != model.size()) {
            QFile::remove(target);
            QFile::copy(model.absoluteFilePath(), target);
        }
    }
}

LibraryPaths LibraryPaths::forThisCopy()
{
    const QDir program(QCoreApplication::applicationDirPath());
    if (QFileInfo::exists(program.filePath(QString::fromLatin1(kPortableMarker))))
        return inFolder(program.filePath(QString::fromLatin1(kPortableFolder)));
    return standard();
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
