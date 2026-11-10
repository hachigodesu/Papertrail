// Every sample document, damaged in many ways, through both the import's worker and the reader's process. Whatever a
// parser makes of a file, Papertrail must carry on: each request ends within its time limit, and the next good file
// still works. A parser crashing on a damaged file is expected, and counted, but never fails the test.
//
// PAPERTRAIL_DAMAGE_ROUNDS sets how many randomly damaged copies each sample gets on top of the fixed ones (2 by
// default), and PAPERTRAIL_DAMAGE_SEED which ones, so a failure can be found again.

#include "import/WorkerClient.h"
#include "ui/DocumentClient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail;

namespace {

constexpr int kInspectTimeoutMs = 10000;
// The client's own limit, plus starting a worker again, plus a slow test machine.
constexpr int kMostPerRequestMs = kInspectTimeoutMs + 15000;
constexpr qint64 kLargestSample = 1024 * 1024;

struct Damaged {
    QString name;
    QByteArray bytes;
};

// Fixed kinds of damage, which every run tries, then random ones.
QList<Damaged> damage(const QString &name, const QByteArray &original, QRandomGenerator &random, int rounds)
{
    QList<Damaged> copies;
    const qsizetype size = original.size();
    for (const int percent : {1, 10, 50, 90, 99})
        copies << Damaged{QStringLiteral("%1, cut to %2%").arg(name).arg(percent), original.left(size * percent / 100)};
    QByteArray zeroed = original;
    for (qsizetype i = size / 3; i < qMin(size, size / 3 + 512); ++i)
        zeroed[i] = 0;
    copies << Damaged{name + QStringLiteral(", a block zeroed"), zeroed};
    QByteArray doubled = original.left(size / 2) + original + original.mid(size / 2);
    copies << Damaged{name + QStringLiteral(", spliced into itself"), doubled};
    QByteArray end = original;
    end.append(QByteArray(4096, '\xff'));
    copies << Damaged{name + QStringLiteral(", rubbish on the end"), end};

    for (int round = 0; round < rounds; ++round) {
        QByteArray flipped = original;
        const int flips = 1 + int(random.bounded(32));
        for (int i = 0; i < flips && size > 0; ++i)
            flipped[random.bounded(size)] = char(random.bounded(256));
        copies << Damaged{QStringLiteral("%1, %2 bytes changed (round %3)").arg(name).arg(flips).arg(round), flipped};
    }
    return copies;
}

QByteArray hashOf(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

} // namespace

class DamagedFilesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QDir samples(QStringLiteral(PAPERTRAIL_TEST_SAMPLES));
        for (const QFileInfo &info : samples.entryInfoList(QDir::Files, QDir::Name)) {
            if (info.suffix() == QLatin1StringView("md") || info.suffix() == QLatin1StringView("txt") || info.size() == 0
                || info.size() > kLargestSample)
                continue;
            m_samples << info.absoluteFilePath();
            m_hashes << hashOf(info.absoluteFilePath());
        }
        QVERIFY(m_samples.size() >= 20);
        bool ok = false;
        m_rounds = qEnvironmentVariableIntValue("PAPERTRAIL_DAMAGE_ROUNDS", &ok);
        if (!ok)
            m_rounds = 2;
        m_seed = quint32(qEnvironmentVariableIntValue("PAPERTRAIL_DAMAGE_SEED", &ok));
        if (!ok)
            m_seed = 20261109;
        qInfo("%lld samples, %d random rounds each, seed %u", qlonglong(m_samples.size()), m_rounds, m_seed);
    }

    void importCarriesOnWhateverTheFileIs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        QRandomGenerator random(m_seed);
        int tried = 0, read = 0, refused = 0, crashed = 0, timedOut = 0;
        QStringList slow;
        for (const QString &sample : std::as_const(m_samples)) {
            QFile file(sample);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray original = file.readAll();
            const QString suffix = QFileInfo(sample).suffix();
            for (const Damaged &copy : damage(QFileInfo(sample).fileName(), original, random, m_rounds)) {
                const QString path = dir.filePath(QStringLiteral("damaged-%1.%2").arg(tried).arg(suffix));
                QFile out(path);
                QVERIFY(out.open(QIODevice::WriteOnly));
                out.write(copy.bytes);
                out.close();

                QElapsedTimer timer;
                timer.start();
                const auto inspection = worker.inspect(path, path + QStringLiteral(".jpg"), path + QStringLiteral(".epub"), kInspectTimeoutMs);
                switch (inspection.failure) {
                case import::WorkerFailure::None: ++read; break;
                case import::WorkerFailure::Crashed: ++crashed; break;
                case import::WorkerFailure::TimedOut: ++timedOut; break;
                default: ++refused; break;
                }
                if (inspection.failure == import::WorkerFailure::None && inspection.result.pageCount.value_or(0) > 0)
                    worker.pageTexts(path, path + QStringLiteral(".epub"), 0, 3, kInspectTimeoutMs);
                if (timer.elapsed() > kMostPerRequestMs)
                    slow << copy.name;
                ++tried;
                QFile::remove(path);
            }
        }
        qInfo("Import: %d damaged files, %d read, %d refused, %d crashed the worker, %d timed out", tried, read, refused, crashed,
              timedOut);
        QVERIFY2(slow.isEmpty(), qPrintable(QStringLiteral("Took too long: ") + slow.join(QStringLiteral("; "))));

        // And afterwards, a good file reads as it should.
        const auto good = worker.inspect(QStringLiteral(PAPERTRAIL_TEST_SAMPLES "/article.pdf"), dir.filePath(QStringLiteral("good.jpg")),
                                         QString(), kInspectTimeoutMs);
        QCOMPARE(good.failure, import::WorkerFailure::None);
        QCOMPARE(good.result.pageCount.value_or(0), 1);
    }

    // Recognising the words of a damaged scan, which renders whatever's left of its page for Tesseract.
    void recognitionCarriesOnWhateverTheFileIs()
    {
#ifndef PAPERTRAIL_TEST_OCR
        QSKIP("This build can't recognise text");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        import::WorkerClient worker(QStringLiteral(PAPERTRAIL_WORKER_PATH));
        QRandomGenerator random(m_seed);
        QFile file(QStringLiteral(PAPERTRAIL_TEST_SAMPLES "/scanned_text.pdf"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        int tried = 0, recognised = 0;
        for (const Damaged &copy : damage(QStringLiteral("scanned_text.pdf"), file.readAll(), random, m_rounds)) {
            const QString path = dir.filePath(QStringLiteral("damaged-%1.pdf").arg(tried));
            QFile out(path);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(copy.bytes);
            out.close();
            const auto result = worker.recognise(path, QString(), dir.filePath(QStringLiteral("ocr-%1").arg(tried)),
                                                 QStringLiteral(PAPERTRAIL_TEST_TESSDATA), QStringLiteral("eng"), 0, 2, false,
                                                 kInspectTimeoutMs * 4);
            QVERIFY2(result.failure != import::WorkerFailure::TimedOut, qPrintable(copy.name));
            recognised += result.pages;
            ++tried;
        }
        qInfo("Recognition: %d damaged scans, %d pages recognised", tried, recognised);
        // And afterwards, the worker still recognises a good one.
        const auto good = worker.recognise(QStringLiteral(PAPERTRAIL_TEST_SAMPLES "/scanned_text.pdf"), QString(),
                                           dir.filePath(QStringLiteral("good")), QStringLiteral(PAPERTRAIL_TEST_TESSDATA),
                                           QStringLiteral("eng"), 0, 1, false, kInspectTimeoutMs * 4);
        QCOMPARE(good.failure, import::WorkerFailure::None);
        QCOMPARE(good.pages, 1);
#endif
    }

    void theReaderCarriesOnWhateverTheFileIs()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QRandomGenerator random(m_seed + 1);
        int tried = 0, opened = 0, failed = 0, pagesShown = 0;
        QStringList stuck;
        for (const QString &sample : std::as_const(m_samples)) {
            QFile file(sample);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray original = file.readAll();
            const QString suffix = QFileInfo(sample).suffix();
            // Fewer here, since each opens a process of its own.
            const QList<Damaged> copies = damage(QFileInfo(sample).fileName(), original, random, 1);
            for (const Damaged &copy : {copies.at(1), copies.at(3), copies.last()}) {
                const QString path = dir.filePath(QStringLiteral("reading-%1.%2").arg(tried).arg(suffix));
                QFile out(path);
                QVERIFY(out.open(QIODevice::WriteOnly));
                out.write(copy.bytes);
                out.close();

                ui::DocumentClient client(QStringLiteral(PAPERTRAIL_WORKER_PATH));
                client.setTimeouts(kInspectTimeoutMs, kInspectTimeoutMs);
                QSignalSpy openSpy(&client, &ui::DocumentClient::opened);
                QSignalSpy failSpy(&client, &ui::DocumentClient::failed);
                client.open(path, ui::ReaderLayout{}, path + QStringLiteral(".epub"));
                QElapsedTimer timer;
                timer.start();
                while (openSpy.isEmpty() && failSpy.isEmpty() && timer.elapsed() < kMostPerRequestMs)
                    QTest::qWait(20);
                if (!openSpy.isEmpty()) {
                    ++opened;
                    // Its first page either shows or is marked as failed, but doesn't leave the reader waiting.
                    QSignalSpy rendered(&client, &ui::DocumentClient::pageRendered);
                    QSignalSpy pageFailed(&client, &ui::DocumentClient::pageFailed);
                    client.setWantedPages({ui::RenderRequest{0, 1.0}});
                    timer.restart();
                    while (rendered.isEmpty() && pageFailed.isEmpty() && failSpy.isEmpty() && timer.elapsed() < kMostPerRequestMs)
                        QTest::qWait(20);
                    if (!rendered.isEmpty())
                        ++pagesShown;
                    else if (pageFailed.isEmpty() && failSpy.isEmpty())
                        stuck << copy.name + QStringLiteral(" (its first page)");
                } else if (!failSpy.isEmpty()) {
                    ++failed;
                } else {
                    stuck << copy.name;
                }
                ++tried;
            }
        }
        qInfo("Reader: %d damaged files, %d opened (%d showed their first page), %d refused", tried, opened, pagesShown, failed);
        QVERIFY2(stuck.isEmpty(), qPrintable(QStringLiteral("Left waiting: ") + stuck.join(QStringLiteral("; "))));
    }

    void theSamplesThemselvesAreUntouched()
    {
        for (int i = 0; i < m_samples.size(); ++i)
            QCOMPARE(hashOf(m_samples.at(i)), m_hashes.at(i));
    }

private:
    QStringList m_samples;
    QList<QByteArray> m_hashes;
    int m_rounds = 2;
    quint32 m_seed = 0;
};

QTEST_MAIN(DamagedFilesTest)
#include "test_damaged_files.moc"
