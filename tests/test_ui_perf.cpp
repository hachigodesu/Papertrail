// Times the library window on a large synthetic library, against the plan's targets: the window up
// within 2 seconds, results within 100 ms (250 ms when ranking a search), and smooth scrolling.
//
// Environment:
//   PAPERTRAIL_PERF_COUNT  publications to generate (default 100000)
//   PAPERTRAIL_PERF_DB     keep the library at this path and reuse it on later runs
//   PAPERTRAIL_PERF_REPORT_ONLY  print timings but never fail, for shared CI machines

#include "SyntheticLibrary.h"

#include "core/db/Database.h"
#include "ui/BulkChanges.h"
#include "core/db/Schema.h"
#include "import/ImportService.h"
#include "ui/FilterSidebar.h"
#include "ui/ImportThread.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/PublicationListModel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <algorithm>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::ui;

namespace {

constexpr int kWindowLimitMs = 2000;
constexpr int kResultsLimitMs = 100;
constexpr int kRankedResultsLimitMs = 250;
constexpr double kFrameLimitMs = 16.7;

bool reportOnly()
{
    return qEnvironmentVariableIsSet("PAPERTRAIL_PERF_REPORT_ONLY");
}

} // namespace

class UiPerfTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const int count = qEnvironmentVariableIntValue("PAPERTRAIL_PERF_COUNT");
        m_count = count > 0 ? count : 100000;

        m_paths.database = qEnvironmentVariable("PAPERTRAIL_PERF_DB");
        if (m_paths.database.isEmpty())
            m_paths.database = m_dir.filePath(QStringLiteral("perf.db"));
        m_paths.cacheDirectory = m_dir.filePath(QStringLiteral("cache"));

        if (!QFile::exists(m_paths.database)) {
            Database db(m_paths.database);
            migrateLibrary(db);
            tools::SyntheticOptions options;
            options.publications = m_count;
            tools::generateSyntheticLibrary(db, options);
            db.exec("PRAGMA optimize");
        }

        m_session = std::make_unique<LibrarySession>(m_paths);
        m_session->settings().setValue(QStringLiteral("library.rescan_on_startup"), false);
        m_total = m_session->query().count({});
        writeCovers();
        m_out << "Library holds " << m_total << " publications\n\n";
    }

    void windowAppearsQuickly()
    {
        QElapsedTimer timer;
        timer.start();
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->resize(1400, 900);
        m_window->show();
        QCoreApplication::processEvents();
        const qint64 shown = timer.elapsed();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->model()->hasResults(), 30000);
        QCoreApplication::processEvents();
        const qint64 withRows = timer.elapsed();
        report("Window shown", shown);
        report("Window showing its first rows", withRows);
        check(withRows, kWindowLimitMs);
    }

    void filtersAnswerQuickly()
    {
        const auto allTab = [this] { m_window->setTab(LibraryTab::All); };
        const auto noSearch = [this] { searchNow(QString()); };
        timeResults("Change tab", [this] { m_window->setTab(LibraryTab::JournalArticles); }, allTab, kResultsLimitMs);
        timeResults("Search for a common word, best match first", [this] { searchNow(QStringLiteral("analysis")); },
                    noSearch, kRankedResultsLimitMs);
        timeResults("Search for a field", [this] { searchNow(QStringLiteral("year:>2015")); }, noSearch, kResultsLimitMs);
    }

    void sidebarCountsDoNotHoldAnythingUp()
    {
        QElapsedTimer timer;
        QSignalSpy updated(m_window.get(), &MainWindow::sidebarUpdated);
        timer.start();
        m_window->applyFiltersNow();
        const qint64 returned = timer.elapsed();
        QVERIFY(updated.wait(60000));
        report("Asking for new results returned to the window", returned);
        report("Sidebar counts arrived, in the background", timer.elapsed());
        check(returned, 20);
    }

    void gridScrollsSmoothly()
    {
        m_window->setViewMode(MainWindow::ViewMode::Grid);
        scrollTest(QStringLiteral("Grid"), m_window->gridView());
    }

    void gridRearrangesQuicklyWhenResized()
    {
        m_window->setViewMode(MainWindow::ViewMode::Grid);
        QList<double> frames;
        for (int width = 1400; width > 1000; width -= 40) {
            QElapsedTimer timer;
            timer.start();
            m_window->resize(width, 900);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QCoreApplication::processEvents();
            m_window->gridView()->viewport()->repaint();
            frames << double(timer.nsecsElapsed()) / 1e6;
        }
        m_window->resize(1400, 900);
        QCoreApplication::processEvents();
        reportFrames(QStringLiteral("Grid: resizing the window"), frames);
    }

    void listScrollsSmoothly()
    {
        m_window->setViewMode(MainWindow::ViewMode::List);
        scrollTest(QStringLiteral("List"), m_window->listView());
    }

    void changesToEverythingReport()
    {
        m_window->setViewMode(MainWindow::ViewMode::Grid);
        QElapsedTimer timer;
        timer.start();
        m_window->gridView()->selectAll();
        report("Select all", timer.elapsed());
        QCOMPARE(m_window->selectedCount(), m_total);

        timer.restart();
        const QList<qint64> ids = m_window->selectedIds();
        report("Work out which publications are selected", timer.elapsed());
        QCOMPARE(ids.size(), m_total);

        QSignalSpy finished(m_window->bulkChanges(), &BulkChanges::finished);
        timer.restart();
        m_window->tagSelected(QStringLiteral("Perf test > All"));
        const qint64 tagReturned = timer.elapsed();
        report("Tag all of them: back to the window", tagReturned);
        check(tagReturned, 200, QStringLiteral("Starting a change to everything"));

        // The window stays free while it happens.
        double worstPause = 0;
        while (finished.isEmpty()) {
            QElapsedTimer pause;
            pause.start();
            QTest::qWait(10);
            worstPause = qMax(worstPause, double(pause.nsecsElapsed()) / 1e6 - 10);
            QVERIFY(timer.elapsed() < 300000);
        }
        report("Tag all of them: finished, in the background", timer.elapsed());
        m_out << "Longest the window paused meanwhile: " << QString::number(worstPause, 'f', 1) << " ms\n";

        timer.restart();
        m_window->setSelectedReadStatus(ReadStatus::Reading);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 300000);
        report("Set the status of all of them, in the background", timer.elapsed());
    }

    void cleanupTestCase()
    {
        m_out.flush();
        m_window.reset();
        m_session.reset();
        if (!m_failures.isEmpty()) {
            if (reportOnly())
                qInfo().noquote() << "Over the limit (report only):" << m_failures.join(QStringLiteral(", "));
            else
                QFAIL(qPrintable(QStringLiteral("Over the limit: ") + m_failures.join(QStringLiteral(", "))));
        }
    }

private:
    void searchNow(const QString &text)
    {
        m_window->searchBox()->setText(text);
        m_window->applyFiltersNow();
    }

    void timeResults(const QString &what, const std::function<void()> &change, const std::function<void()> &undo,
                     int limitMs)
    {
        // Runs three times with the change undone in between, and reports the median.
        QList<qint64> times;
        for (int run = 0; run < 3; ++run) {
            QSignalSpy results(m_window->model(), &PublicationListModel::resultsChanged);
            QElapsedTimer timer;
            timer.start();
            change();
            QVERIFY(results.wait(60000));
            times << timer.elapsed();
            undo();
            QVERIFY(results.wait(60000));
        }
        std::sort(times.begin(), times.end());
        report(what, times.at(1));
        check(times.at(1), limitMs, what);
    }

    void scrollTest(const QString &name, QAbstractItemView *view)
    {
        QScrollBar *bar = view->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);

        // Waits for the covers on screen, then scrolls a step at a time, as a mouse wheel would.
        const auto settle = [] {
            QTest::qWait(300);
        };
        bar->setValue(0);
        settle();

        QList<double> frames;
        const auto frame = [&](int value) {
            QElapsedTimer timer;
            timer.start();
            bar->setValue(value);
            view->viewport()->repaint();
            QCoreApplication::processEvents();
            frames << double(timer.nsecsElapsed()) / 1e6;
        };

        const int step = view == m_window->gridView() ? 40 : 46;
        for (int i = 1; i <= 300; ++i)
            frame(i * step);
        reportFrames(name + QStringLiteral(": wheel scrolling"), frames);
        const double wheel = percentile(frames, 0.95);
        check(wheel, kFrameLimitMs, name + QStringLiteral(" wheel scrolling"));

        // Dragging the scroll bar: rows that have never been fetched, far apart.
        frames.clear();
        for (int i = 0; i < 100; ++i)
            frame(QRandomGenerator::global()->bounded(bar->maximum()));
        reportFrames(name + QStringLiteral(": dragging the scroll bar"), frames);
        check(percentile(frames, 0.95), kFrameLimitMs, name + QStringLiteral(" dragging"));

        // How long the far end of the list takes to fill in.
        QElapsedTimer timer;
        timer.start();
        bar->setValue(bar->maximum());
        view->viewport()->repaint();
        const QModelIndex last = m_window->model()->index(m_window->model()->rowCount() - 1, 0);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->model()->data(last, PublicationListModel::IsLoadedRole).toBool(), 30000);
        report(name + QStringLiteral(": last rows filled in after jumping to the end"), timer.elapsed());
    }

    static double percentile(QList<double> values, double fraction)
    {
        std::sort(values.begin(), values.end());
        return values.at(qMin(values.size() - 1, qsizetype(values.size() * fraction)));
    }

    void reportFrames(const QString &what, const QList<double> &frames)
    {
        m_out << what.leftJustified(56) << " median " << QString::number(percentile(frames, 0.5), 'f', 1) << " ms, 95% "
              << QString::number(percentile(frames, 0.95), 'f', 1) << " ms, worst "
              << QString::number(percentile(frames, 1.0), 'f', 1) << " ms\n";
        m_out.flush();
    }

    void report(const QString &what, qint64 milliseconds)
    {
        m_out << what.leftJustified(56) << " " << milliseconds << " ms\n";
        m_out.flush();
    }

    void check(double value, double limit, const QString &what = QString())
    {
        if (value > limit) {
            m_failures << QStringLiteral("%1 (%2 ms, limit %3 ms)")
                              .arg(what.isEmpty() ? QStringLiteral("window") : what)
                              .arg(value, 0, 'f', 1)
                              .arg(limit);
        }
    }

    // The first few thousand rows get real cover files, of the size the worker writes.
    void writeCovers()
    {
        QImage image(400, 566, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(QStringLiteral("Arial"), 14));
        for (int y = 30; y < 540; y += 18)
            painter.drawText(30, y, QStringLiteral("Lorem ipsum dolor sit amet, consectetur adipiscing."));
        painter.end();

        const QString folder = m_paths.cacheDirectory;
        const QString source = m_dir.filePath(QStringLiteral("cover.jpg"));
        QVERIFY(image.save(source, "JPG", 80));
        const QList<PublicationSummary> rows = m_session->query().page({}, {}, 0, 3000);
        for (const PublicationSummary &row : rows) {
            if (row.coverSha.isEmpty())
                continue;
            const QString target = import::coverPathFor(folder, row.coverSha);
            QDir().mkpath(QFileInfo(target).absolutePath());
            QFile::copy(source, target);
        }
    }

    QTemporaryDir m_dir;
    QTextStream m_out{stdout};
    LibraryPaths m_paths;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
    int m_count = 0;
    qint64 m_total = 0;
    QStringList m_failures;
};

QTEST_MAIN(UiPerfTest)
#include "test_ui_perf.moc"
