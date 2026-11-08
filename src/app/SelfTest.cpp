#include "SelfTest.h"

#include "core/model/Publication.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/PublicationRepository.h"
#include "import/WorkerClient.h"
#include "ui/LibrarySession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#include <exception>

namespace papertrail::app {

namespace {

constexpr int kWorkerTimeoutMs = 60000;

struct Report {
    QStringList lines;
    bool passed = true;

    void check(bool ok, const QString &what, const QString &detail = QString())
    {
        lines << (ok ? QStringLiteral("PASS ") : QStringLiteral("FAIL ")) + what + (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        passed = passed && ok;
    }
};

void checkDocument(Report &report, import::WorkerClient &worker, const QDir &folder, const QString &resource)
{
    const QString name = QFileInfo(resource).fileName();
    const QString path = folder.filePath(name);
    if (!QFile::copy(resource, path)) {
        report.check(false, name, QStringLiteral("couldn't be copied out of the program"));
        return;
    }
    const QString cover = folder.filePath(name + QStringLiteral(".jpg"));
    const import::WorkerClient::Inspection inspection = worker.inspect(path, cover, folder.filePath(name + QStringLiteral(".epub")), kWorkerTimeoutMs);
    if (inspection.failure != import::WorkerFailure::None) {
        report.check(false, name, inspection.error.isEmpty() ? QStringLiteral("the worker couldn't read it") : inspection.error);
        return;
    }
    const int pages = inspection.result.pageCount.value_or(0);
    report.check(pages > 0, name + QStringLiteral(" opens"),
                 QStringLiteral("%1 pages, format %2").arg(pages).arg(inspection.result.format));
    const QImage image(cover);
    report.check(inspection.result.coverWritten && !image.isNull(), name + QStringLiteral(" has a cover that can be shown"),
                 image.isNull() ? QStringLiteral("no image") : QStringLiteral("%1x%2").arg(image.width()).arg(image.height()));
}

} // namespace

bool runSelfTest(const QString &reportPath)
{
    Report report;
    report.lines << QStringLiteral("Papertrail %1 self-test").arg(QCoreApplication::applicationVersion());
    QTemporaryDir temp;
    if (!temp.isValid()) {
        report.check(false, QStringLiteral("a temporary folder"), temp.errorString());
    } else {
        const QDir folder(temp.path());
        try {
            // A library of its own, never the user's.
            ui::LibraryPaths paths = ui::LibraryPaths::inFolder(folder.filePath(QStringLiteral("library")));
            report.check(QFileInfo::exists(paths.workerPath), QStringLiteral("the worker is installed"), QDir::toNativeSeparators(paths.workerPath));

            import::WorkerClient worker(paths.workerPath);
            checkDocument(report, worker, folder, QStringLiteral(":/selftest/article.pdf"));
            checkDocument(report, worker, folder, QStringLiteral(":/selftest/book.epub"));
            checkDocument(report, worker, folder, QStringLiteral(":/selftest/with_text.djvu"));

            ui::LibrarySession session(paths);
            core::Publication publication;
            publication.title = QStringLiteral("Fatigue of Steel Beams");
            session.publications().create(publication);
            core::PublicationFilter search;
            search.searchText = QStringLiteral("steel");
            report.check(session.query().count(search) == 1, QStringLiteral("a library can be made and searched"));
        } catch (const std::exception &error) {
            report.check(false, QStringLiteral("the library"), QString::fromUtf8(error.what()));
        }
    }
    report.check(QSslSocket::supportsSsl(), QStringLiteral("secure connections for online lookups"),
                 QSslSocket::activeBackend() + u' ' + QSslSocket::sslLibraryVersionString());
    report.lines << (report.passed ? QStringLiteral("PASSED") : QStringLiteral("FAILED"));

    const QByteArray text = report.lines.join(u'\n').toUtf8() + '\n';
    if (reportPath.isEmpty()) {
        std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
        std::fflush(stdout);
    } else {
        QFile file(reportPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(text);
    }
    return report.passed;
}

} // namespace papertrail::app
