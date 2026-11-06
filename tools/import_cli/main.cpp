// Imports folders into a library from the command line, for testing until the app has a window.
//
//   papertrail-import --db LIBRARY.db --cache CACHE_DIR FOLDER...
//
// Stop it at any point: running the same command again carries on from where it stopped.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "import/ImportService.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTextStream>

using namespace papertrail;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("db"), QStringLiteral("Library database file."), QStringLiteral("file")});
    parser.addOption({QStringLiteral("cache"), QStringLiteral("Folder for covers."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("worker"), QStringLiteral("Path to papertrail-worker."), QStringLiteral("file"),
                      QStringLiteral(PAPERTRAIL_WORKER_BUILD_PATH)});
    parser.addOption({QStringLiteral("max-jobs"), QStringLiteral("Stop after this many files, to test resuming."),
                      QStringLiteral("count")});
    parser.addPositionalArgument(QStringLiteral("folders"), QStringLiteral("Folders to import."));
    parser.process(app);

    if (!parser.isSet(QStringLiteral("db")) || !parser.isSet(QStringLiteral("cache"))) {
        parser.showHelp(2);
    }

    core::Database db(parser.value(QStringLiteral("db")));
    core::migrateLibrary(db);

    import::ImportOptions options;
    options.workerPath = parser.value(QStringLiteral("worker"));
    options.cacheDirectory = parser.value(QStringLiteral("cache"));
    import::ImportService service(db, options);
    core::FileRepository files(db);

    QElapsedTimer timer;
    timer.start();

    const auto roots = files.roots();
    for (const QString &folder : parser.positionalArguments()) {
        const QString path = QDir::fromNativeSeparators(QFileInfo(folder).absoluteFilePath());
        qint64 rootId = 0;
        for (const core::LibraryRoot &root : roots) {
            if (root.path == path)
                rootId = root.id;
        }
        if (rootId == 0)
            rootId = files.addRoot(QFileInfo(folder).fileName(), path);

        const import::ScanReport scan = service.scan(rootId);
        out << "Scanned " << path << ": " << scan.newFiles << " new, " << scan.changed << " changed, "
            << scan.unchanged << " unchanged, " << scan.nowMissing << " now missing, " << scan.backAgain
            << " back again, " << scan.unsupportedSkipped << " unsupported skipped ("
            << timer.elapsed() / 1000.0 << " s)\n";
        out.flush();
    }

    const qint64 maxJobs = parser.value(QStringLiteral("max-jobs")).toLongLong();
    qint64 lastDone = 0;
    QElapsedTimer lastReport;
    lastReport.start();
    const import::ImportReport report = service.processQueue(
        [&](const import::ImportProgress &progress) {
            lastDone = progress.done;
            if (lastReport.elapsed() >= 5000) {
                out << "  " << progress.done << " done, " << progress.remaining << " still queued\n";
                out.flush();
                lastReport.restart();
            }
        },
        [&] { return maxJobs > 0 && lastDone >= maxJobs - 1; });

    const double seconds = timer.elapsed() / 1000.0;
    core::LibraryQuery query(db);
    const core::FlagCounts flags = query.flagCounts({});

    out << "\nFinished in " << seconds << " s\n"
        << "  Publications created: " << report.publicationsCreated << '\n'
        << "  Duplicate copies:     " << report.duplicates << '\n'
        << "  Moved files:          " << report.moved << '\n'
        << "  Updated files:        " << report.updated << '\n'
        << "  Unreadable files:     " << report.unreadable << '\n'
        << "  Worker recoveries:    " << report.workerRestarts << '\n'
        << "  Stopped early:        " << (report.stoppedEarly ? "yes" : "no") << '\n'
        << "Library now holds " << query.count({}) << " publications: " << flags.needsReview << " need review, "
        << flags.noText << " have no text, " << flags.unreadable << " unreadable\n";

    out << "By type:\n";
    for (const core::FacetValue &value : query.facet({}, core::Facet::Type))
        out << "  " << value.key.leftJustified(26) << value.count << '\n';
    out << "By format:\n";
    for (const core::FacetValue &value : query.facet({}, core::Facet::Format))
        out << "  " << value.key.leftJustified(26) << value.count << '\n';
    return 0;
}
