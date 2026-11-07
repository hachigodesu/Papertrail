// Times exporting and importing a large library. Labelled "perf" and left out of the usual test run.
//
//   PAPERTRAIL_PERF_COUNT        publications to generate (default 100000)
//   PAPERTRAIL_PERF_DB           keep the library at this path and reuse it on later runs
//   PAPERTRAIL_PERF_REPORT_ONLY  print timings but never fail, for shared CI machines
#include "SyntheticLibrary.h"

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "transfer/LibraryTransfer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::transfer;

namespace {

// Generous, since nothing waits on these but the person who asked: a minute each way at 100,000.
constexpr qint64 kExportTargetMs = 60000;
constexpr qint64 kImportTargetMs = 120000;

} // namespace

class TransferPerfTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const int count = qEnvironmentVariableIntValue("PAPERTRAIL_PERF_COUNT");
        m_count = count > 0 ? count : 100000;
        QString path = qEnvironmentVariable("PAPERTRAIL_PERF_DB");
        if (path.isEmpty())
            path = m_dir.filePath(QStringLiteral("perf.db"));
        const bool reuse = QFile::exists(path);
        m_db = std::make_unique<Database>(path);
        migrateLibrary(*m_db);
        if (!reuse) {
            tools::SyntheticOptions options;
            options.publications = m_count;
            tools::generateSyntheticLibrary(*m_db, options);
        }
    }

    void aLargeLibraryExportsAndImportsInGoodTime()
    {
        const bool reportOnly = qEnvironmentVariableIsSet("PAPERTRAIL_PERF_REPORT_ONLY");
        QTextStream out(stdout);

        QElapsedTimer timer;
        timer.start();
        const QString path = m_dir.filePath(QStringLiteral("large.papertrail"));
        const ExportReport exported = exportLibrary(*m_db, path);
        const qint64 exportMs = timer.elapsed();
        out << "Exported " << exported.publications << " publications in " << exportMs / 1000.0 << " s, "
            << QFileInfo(path).size() / 1024 / 1024 << " MB\n";

        timer.restart();
        QString error;
        const std::optional<Bundle> bundle = readBundleFile(path, &error);
        QVERIFY2(bundle, qPrintable(error));
        const qint64 readMs = timer.elapsed();

        Database empty(m_dir.filePath(QStringLiteral("empty.db")));
        migrateLibrary(empty);
        timer.restart();
        const ImportReport report = importBundle(empty, *bundle, {});
        const qint64 importMs = timer.elapsed();
        out << "Read it in " << readMs / 1000.0 << " s, and imported it into an empty library in " << importMs / 1000.0 << " s\n";

        // Again, into the library that already has it all, which is the slowest case: everything is compared.
        timer.restart();
        const ImportReport again = importBundle(empty, *bundle, {});
        const qint64 againMs = timer.elapsed();
        out << "Imported it again, with nothing new, in " << againMs / 1000.0 << " s\n";
        out.flush();

        QCOMPARE(report.added, exported.publications);
        QCOMPARE(again.added, qint64(0));
        if (!reportOnly) {
            QVERIFY2(exportMs < kExportTargetMs, "exporting took too long");
            QVERIFY2(importMs + readMs < kImportTargetMs, "importing took too long");
            QVERIFY2(againMs + readMs < kImportTargetMs, "importing again took too long");
        }
    }

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Database> m_db;
    int m_count = 100000;
};

QTEST_GUILESS_MAIN(TransferPerfTest)
#include "test_transfer_perf.moc"
