// Times the reader on a generated 3,000-page book, against what M4 asks for: large textbooks that
// open promptly, and pages that appear without a wait.
//
// Environment:
//   PAPERTRAIL_PERF_REPORT_ONLY  print timings but never fail, for shared CI machines

#include "ui/DocumentClient.h"
#include "ui/PageView.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <algorithm>

using namespace papertrail::ui;

namespace {

constexpr int kPages = 3000;
constexpr int kOpenLimitMs = 2000;
constexpr int kFirstPageLimitMs = 500;
constexpr int kPageLimitMs = 200; // At a size for a large window.
constexpr double kFrameLimitMs = 16.7;

// A book with a page of ordinary text on each page, and some simple drawing, as a textbook has.
void writeBook(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QList<qint64> offsets(2 + 2 * kPages + 2, 0);
    qint64 written = 0;
    const auto put = [&](const QByteArray &bytes) {
        file.write(bytes);
        written += bytes.size();
    };
    const auto object = [&](int number, const QByteArray &body) {
        offsets[number] = written;
        put(QByteArray::number(number) + " 0 obj\n" + body + "\nendobj\n");
    };

    put("%PDF-1.4\n");
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    QByteArray kids;
    for (int i = 0; i < kPages; ++i)
        kids += QByteArray::number(4 + i * 2) + " 0 R ";
    object(2, "<< /Type /Pages /Count " + QByteArray::number(kPages) + " /Kids [" + kids + "] >>");
    object(3, "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>");

    for (int i = 0; i < kPages; ++i) {
        QByteArray content = "BT /F1 18 Tf 60 780 Td (Page " + QByteArray::number(i + 1) + ") Tj ET\n";
        for (int line = 0; line < 45; ++line) {
            content += "BT /F1 11 Tf 60 " + QByteArray::number(750 - line * 15) + " Td (Line "
                + QByteArray::number(line) + " of a long textbook, with enough words on it to fill the width of the page.) Tj ET\n";
        }
        for (int shape = 0; shape < 40; ++shape) {
            content += QByteArray::number(60 + shape * 11) + " 60 m " + QByteArray::number(70 + shape * 11) + " 40 l S\n";
        }
        object(4 + i * 2, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents " + QByteArray::number(5 + i * 2)
                              + " 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
        object(5 + i * 2, "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream");
    }

    const qint64 xref = written;
    const int size = 4 + kPages * 2;
    put("xref\n0 " + QByteArray::number(size) + "\n0000000000 65535 f \n");
    for (int i = 1; i < size; ++i)
        put(QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n");
    put("trailer\n<< /Size " + QByteArray::number(size) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n");
}

double median(QList<double> values)
{
    std::sort(values.begin(), values.end());
    return values.isEmpty() ? 0 : values.at(values.size() / 2);
}

double percentile(QList<double> values, double fraction)
{
    std::sort(values.begin(), values.end());
    return values.isEmpty() ? 0 : values.at(qMin(values.size() - 1, qsizetype(values.size() * fraction)));
}

} // namespace

class ReaderPerfTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_path = m_dir.filePath(QStringLiteral("book.pdf"));
        writeBook(m_path);
        m_out << "A " << kPages << "-page book of " << QFile(m_path).size() / (1024 * 1024) << " MB\n\n";
        m_out.flush();

        QElapsedTimer timer;
        timer.start();
        m_client = std::make_unique<DocumentClient>(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        QSignalSpy opened(m_client.get(), &DocumentClient::opened);
        QSignalSpy sizes(m_client.get(), &DocumentClient::sizesReady);
        m_client->open(m_path, {});
        QVERIFY(opened.wait(60000));
        report("Opened, with its pages counted", timer.elapsed());
        check(QStringLiteral("opening"), timer.elapsed(), kOpenLimitMs);

        QSignalSpy rendered(m_client.get(), &DocumentClient::pageRendered);
        m_client->setWantedPages({{0, 2.0}});
        QVERIFY(rendered.wait(60000));
        report("First page on hand", timer.elapsed());
        check(QStringLiteral("first page"), timer.elapsed(), kFirstPageLimitMs);

        QTRY_VERIFY_WITH_TIMEOUT(sizes.size() >= kPages / 500, 120000);
        report("Every page measured, in the background", timer.elapsed());
    }

    void pagesAppearQuickly()
    {
        QSignalSpy rendered(m_client.get(), &DocumentClient::pageRendered);
        for (const double scale : {1.5, 2.0, 3.0}) {
            QList<double> times;
            for (int run = 0; run < 25; ++run) {
                rendered.clear();
                QElapsedTimer timer;
                timer.start();
                m_client->setWantedPages({{QRandomGenerator::global()->bounded(kPages), scale}});
                QVERIFY(rendered.wait(60000));
                times << double(timer.nsecsElapsed()) / 1e6;
            }
            const QImage image = rendered.first().at(2).value<QImage>();
            m_out << QStringLiteral("A random page at %1 by %2 pixels").arg(image.width()).arg(image.height()).leftJustified(54)
                  << " median " << QString::number(median(times), 'f', 1) << " ms, worst "
                  << QString::number(percentile(times, 1.0), 'f', 1) << " ms\n";
            m_out.flush();
            if (scale <= 2.0)
                check(QStringLiteral("page at %1x").arg(scale), median(times), kPageLimitMs);
        }
    }

    void searchingTheWholeBook()
    {
        QSignalSpy finished(m_client.get(), &DocumentClient::searchFinished);
        QSignalSpy hits(m_client.get(), &DocumentClient::searchHits);
        QElapsedTimer timer;
        timer.start();
        m_client->startSearch(QStringLiteral("Page 2999"), 0);
        QVERIFY(finished.wait(300000));
        report("Searching every page", timer.elapsed());
        QCOMPARE(hits.size(), 1);
    }

    void scrollingStaysSmooth()
    {
        // The view's own work, with pages on hand. Rendering happens elsewhere, so it can't hold this up.
        PageView view;
        view.resize(1200, 800);
        view.show();
        view.setPageCount(kPages, QSizeF(595, 842));
        view.setScale(2.0);
        QObject::connect(&view, &PageView::pagesWanted, m_client.get(), &DocumentClient::setWantedPages);
        QObject::connect(m_client.get(), &DocumentClient::pageRendered, &view, &PageView::setPageImage);

        QList<double> frames;
        QScrollBar *bar = view.verticalScrollBar();
        for (int i = 0; i < 400; ++i) {
            QElapsedTimer timer;
            timer.start();
            bar->setValue(bar->value() + 60);
            view.viewport()->repaint();
            QCoreApplication::processEvents();
            frames << double(timer.nsecsElapsed()) / 1e6;
        }
        m_out << QStringLiteral("Scrolling, with rendering going on behind").leftJustified(54) << " median "
              << QString::number(median(frames), 'f', 1) << " ms, 95% " << QString::number(percentile(frames, 0.95), 'f', 1)
              << " ms, worst " << QString::number(percentile(frames, 1.0), 'f', 1) << " ms\n";
        m_out.flush();
        check(QStringLiteral("scrolling"), percentile(frames, 0.95), kFrameLimitMs);

        QElapsedTimer timer;
        timer.start();
        view.goToPage(2500);
        view.viewport()->repaint();
        report("Jump to a far page: back to the window", timer.elapsed());
        QTRY_VERIFY_WITH_TIMEOUT(view.isPageLoaded(2500), 30000);
        report("Jump to a far page: page on hand", timer.elapsed());
        QObject::disconnect(m_client.get(), nullptr, &view, nullptr);
    }

    void cleanupTestCase()
    {
        m_out.flush();
        m_client.reset();
        if (!m_failures.isEmpty()) {
            const QString message = QStringLiteral("Over the limit: ") + m_failures.join(QStringLiteral(", "));
            if (qEnvironmentVariableIsSet("PAPERTRAIL_PERF_REPORT_ONLY"))
                qInfo().noquote() << message;
            else
                QFAIL(qPrintable(message));
        }
    }

private:
    void report(const char *what, qint64 milliseconds)
    {
        m_out << QString::fromLatin1(what).leftJustified(54) << " " << milliseconds << " ms\n";
        m_out.flush();
    }

    void check(const QString &what, double value, double limit)
    {
        if (value > limit)
            m_failures << QStringLiteral("%1 (%2 ms, limit %3 ms)").arg(what).arg(value, 0, 'f', 1).arg(limit);
    }

    QTemporaryDir m_dir;
    QTextStream m_out{stdout};
    QString m_path;
    std::unique_ptr<DocumentClient> m_client;
    QStringList m_failures;
};

QTEST_MAIN(ReaderPerfTest)
#include "test_reader_perf.moc"
