// Creates a fake library for performance testing.
// Usage: gen-library OUTPUT.db [PUBLICATION_COUNT]

#include "SyntheticLibrary.h"

#include "core/db/Database.h"
#include "core/db/Schema.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 2) {
        out << "Usage: gen-library OUTPUT.db [PUBLICATION_COUNT]\n";
        return 2;
    }
    if (QFile::exists(args.at(1))) {
        out << "Refusing to overwrite " << args.at(1) << '\n';
        return 2;
    }

    papertrail::tools::SyntheticOptions options;
    if (args.size() > 2)
        options.publications = args.at(2).toInt();

    QElapsedTimer timer;
    timer.start();
    papertrail::core::Database db(args.at(1));
    papertrail::core::migrateLibrary(db);
    const auto stats = papertrail::tools::generateSyntheticLibrary(db, options);
    db.exec("PRAGMA optimize");

    out << stats.publications << " publications, " << stats.people << " people, " << stats.tags << " tags, "
        << stats.files << " files in " << timer.elapsed() / 1000.0 << " s\n";
    return 0;
}
