// Tests of online lookups. Nothing here leaves this computer: answers are recorded, in the shapes the
// real sources use with made-up contents, and the one real network path talks to a server in this test.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/model/PublicationFields.h"
#include "core/repo/FileRepository.h"
#include "core/repo/LookupRepository.h"
#include "import/ImportService.h"
#include "metadata/LookupEngine.h"
#include "metadata/Matching.h"
#include "metadata/NoticeChecker.h"
#include "metadata/OcrLanguages.h"
#include "metadata/RelatedFinder.h"
#include "ui/RelatedPapersDialog.h"
#include "metadata/Providers.h"
#include "metadata/Transport.h"
#include "metadata/CaseLaw.h"
#include "ui/ApiKeyStore.h"
#include "ui/Delegates.h"
#include "ui/DetailsPanel.h"
#include "ui/FilterSidebar.h"
#include "ui/ImportThread.h"
#include "ui/LibrarySession.h"
#include "ui/LookupThread.h"
#include "ui/MainWindow.h"
#include "ui/OnlineLookupDialog.h"
#include "ui/PublicationListModel.h"
#include "ui/ReviewTab.h"

#include <QListView>
#include <QPainter>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFileInfo>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::metadata;
using namespace papertrail::ui;

namespace {

constexpr int kWaitMs = 30000;

QByteArray fixture(const char *name)
{
    QFile file(QDir(QStringLiteral(PAPERTRAIL_TEST_FIXTURES)).filePath(QString::fromLatin1(name)));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString sample(const QString &name)
{
    return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name);
}

// Answers from a list, and keeps a record of everything it was asked. It can be shared across threads.
struct FakeState {
    QMutex mutex;
    QList<QPair<QString, TransportReply>> answers; // First whose text is in the URL.
    QList<QPair<QString, TransportReply>> keyedAnswers; // The same, for requests carrying a key, looked at first.
    QStringList asked;
    QList<QByteArray> headers; // Each request's headers, as "Name: value" lines, in the same order as `asked`.
    QList<qint64> times;
    QList<TransportReply> oneOff; // Used first, then the list above.

    void answer(const QString &urlPart, const QByteArray &body, int status = 200)
    {
        TransportReply reply;
        reply.status = status;
        reply.body = body;
        answers << qMakePair(urlPart, reply);
    }

    int count()
    {
        QMutexLocker lock(&mutex);
        return int(asked.size());
    }
};

class FakeTransport : public Transport {
public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : m_state(std::move(state)) {}

    void get(const QUrl &url, const Callback &callback) override { answer(url, {}, callback); }

    // Only GETs with headers come this way in these tests.
    void send(const HttpRequest &request, const Callback &callback) override
    {
        QByteArray headers;
        for (const auto &header : request.headers)
            headers += header.first + ": " + header.second + "\n";
        answer(request.url, headers, callback);
    }

private:
    void answer(const QUrl &url, const QByteArray &headers, const Callback &callback)
    {
        TransportReply reply;
        reply.status = 404;
        {
            QMutexLocker lock(&m_state->mutex);
            const QString text = url.toString(QUrl::FullyEncoded);
            m_state->asked << text;
            m_state->headers << headers;
            m_state->times << QDateTime::currentMSecsSinceEpoch();
            if (!m_state->oneOff.isEmpty()) {
                reply = m_state->oneOff.takeFirst();
            } else {
                bool found = false;
                if (headers.contains("Authorization")) {
                    for (const auto &answer : m_state->keyedAnswers) {
                        if (text.contains(answer.first)) {
                            reply = answer.second;
                            found = true;
                            break;
                        }
                    }
                }
                for (qsizetype i = 0; !found && i < m_state->answers.size(); ++i) {
                    if (text.contains(m_state->answers.at(i).first)) {
                        reply = m_state->answers.at(i).second;
                        found = true;
                    }
                }
            }
        }
        QTimer::singleShot(0, [callback, reply] { callback(reply); });
    }

    std::shared_ptr<FakeState> m_state;
};

// A real HTTP server on this computer, for the one test of the real network path.
class LocalServer : public QTcpServer {
public:
    QByteArray response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    std::atomic_int connections = 0;
    QStringList requests;

protected:
    void incomingConnection(qintptr handle) override
    {
        ++connections;
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            requests << QString::fromLatin1(socket->readAll());
            socket->write(response);
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
};

} // namespace

class MetadataTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        m_publications = std::make_unique<PublicationRepository>(*m_db);
        m_lookups = std::make_unique<LookupRepository>(*m_db);
        m_fake = std::make_shared<FakeState>();
        m_transport = std::make_unique<FakeTransport>(m_fake);
        m_engine = std::make_unique<LookupEngine>(*m_db, *m_transport);
        m_engine->setRetryDelays(10, 20);

        m_unexpectedBoxes.clear();
        m_boxWatcher = std::make_unique<QTimer>();
        connect(m_boxWatcher.get(), &QTimer::timeout, this, [this] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                m_unexpectedBoxes << box->text();
                box->close();
            }
        });
        m_boxWatcher->start(100);
    }

    void cleanup()
    {
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
        m_window.reset();
        m_session.reset();
        m_engine.reset();
        m_transport.reset();
        m_lookups.reset();
        m_publications.reset();
        m_db.reset();
        m_dir.reset();
    }

    // ---- What the sources' answers mean ----

    void readsACrossrefWork()
    {
        const auto record = parseCrossrefWork(fixture("crossref_work.json"));
        QVERIFY(record.has_value());
        const Publication &p = record->publication;
        QCOMPARE(p.title, QStringLiteral("Fatigue of Steel Beams & Columns")); // No markup, and no codes.
        QCOMPARE(p.subtitle, QStringLiteral("A Long Study"));
        QCOMPARE(p.doi, QStringLiteral("10.1000/example.2020.7"));
        QCOMPARE(p.authors.size(), 3);
        QCOMPARE(p.authors.at(0).family, QStringLiteral("Example"));
        QCOMPARE(p.authors.at(0).given, QStringLiteral("Ada"));
        QCOMPARE(p.authors.at(2).family, QStringLiteral("The Example Consortium"));
        QCOMPARE(p.containerTitle, QStringLiteral("Journal of Made-up Results"));
        QCOMPARE(p.volume, QStringLiteral("42"));
        QCOMPARE(p.issue, QStringLiteral("3"));
        QCOMPARE(p.pages, QStringLiteral("101-120"));
        QCOMPARE(p.publisher, QStringLiteral("Example Press"));
        QCOMPARE(p.issn, QStringLiteral("1234-5679"));
        QCOMPARE(p.eissn, QStringLiteral("2345-6789"));
        QCOMPARE(p.year.value_or(0), 2020);
        QCOMPARE(p.month.value_or(0), 9);
        QCOMPARE(p.day.value_or(0), 16);
        QCOMPARE(p.abstract, QStringLiteral("Abstract A made-up abstract about steel."));
        QCOMPARE(record->type.value(), PublicationType::JournalArticle);
        QVERIFY(!record->preprintHint);
    }

    void readsCrossrefNotices()
    {
        // As Crossref answers, with made-up works: a notice can be listed by the publisher and by Retraction Watch.
        const QByteArray body = R"({"status":"ok","message":{"items":[
            {"DOI":"10.1000/notice.1","update-to":[
                {"DOI":"10.1000/Paper.ONE","type":"retraction","label":"Retraction","source":"publisher",
                 "updated":{"date-parts":[[2021,3,9]]}},
                {"DOI":"10.1000/paper.one","type":"retraction","label":"Retraction","source":"retraction-watch",
                 "updated":{"date-parts":[[2021,3,9]]}}]},
            {"DOI":"10.1000/notice.2","update-to":[
                {"DOI":"10.1000/paper.two","type":"expression-of-concern","updated":{"date-parts":[[2019]]}},
                {"DOI":"10.1000/paper.two","type":"<b>odd</b>","label":"Odd"},
                {"type":"correction"}]},
            {"title":["No DOI of its own"],"update-to":[{"DOI":"10.1000/paper.three","type":"correction"}]}]}})";
        bool readable = false;
        const QHash<QString, QList<Notice>> found = parseCrossrefNotices(body, &readable);
        QVERIFY(readable);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.value(QStringLiteral("10.1000/paper.one")).size(), 1);
        const Notice retraction = found.value(QStringLiteral("10.1000/paper.one")).first();
        QCOMPARE(retraction.kind, QStringLiteral("retraction"));
        QCOMPARE(retraction.noticeDoi, QStringLiteral("10.1000/notice.1"));
        QCOMPARE(retraction.date, QStringLiteral("2021-03-09"));
        QVERIFY(retraction.isWarning());
        const QList<Notice> concern = found.value(QStringLiteral("10.1000/paper.two"));
        QCOMPARE(concern.size(), 1);
        QCOMPARE(concern.first().kind, QStringLiteral("expression_of_concern"));
        QCOMPARE(concern.first().label, QStringLiteral("Expression of concern"));
        QCOMPARE(concern.first().date, QStringLiteral("2019"));

        QVERIFY(parseCrossrefNotices("not json", &readable).isEmpty());
        QVERIFY(!readable);
    }

    void noticesAreCheckedInBatchesSendingOnlyDois()
    {
        const qint64 one = addPublication(QStringLiteral("Made Up Study One"), QStringLiteral("10.1000/One.1"), false);
        const qint64 two = addPublication(QStringLiteral("Made Up Study Two"), QStringLiteral("10.1000/two.2"), false);
        const qint64 three = addPublication(QStringLiteral("Made Up Study Three"), QStringLiteral("10.1000/three.3"), false);
        const qint64 comma = addPublication(QStringLiteral("Made Up Study Four"), QStringLiteral("10.1000/four,4"), false);
        addPublication(QStringLiteral("Made Up Study Five"), QString(), false);
        m_fake->answer(QStringLiteral("updates"), R"({"message":{"items":[
            {"DOI":"10.1000/n1","update-to":[{"DOI":"10.1000/one.1","type":"retraction","label":"Retraction","updated":{"date-parts":[[2022,1,2]]}}]},
            {"DOI":"10.1000/n3","update-to":[{"DOI":"10.1000/three.3","type":"correction","label":"Correction"}]}]}})");

        NoticeChecker checker(*m_db, *m_transport);
        checker.setBatchSize(2);
        checker.setIntervalMs(0);
        QSignalSpy finished(&checker, &NoticeChecker::finished);
        QSignalSpy changed(&checker, &NoticeChecker::noticesChanged);
        QSignalSpy problem(&checker, &NoticeChecker::problem);
        checker.start();
        QVERIFY(finished.wait(kWaitMs));
        QVERIFY2(problem.isEmpty(), qPrintable(problem.isEmpty() ? QString() : problem.first().first().toString() + u' ' + m_fake->asked.join(u' ')));
        QCOMPARE(finished.first().at(0).toInt(), 4);
        QCOMPARE(finished.first().at(1).toInt(), 1); // Only the retraction is a warning.

        // Two requests: the first two, then the third alone, since a comma can't be asked about. Only DOIs went.
        QCOMPARE(m_fake->count(), 2);
        const QString asked = m_fake->asked.join(u'\n');
        QVERIFY2(asked.contains(QStringLiteral("updates:10.1000/one.1,updates:10.1000/two.2")), qPrintable(asked));
        QVERIFY(!asked.contains(QStringLiteral("four")));
        QVERIFY(!asked.contains(QStringLiteral("Made")));
        QVERIFY(!asked.contains(QStringLiteral("mailto")));

        NoticeRepository notices(*m_db);
        QCOMPARE(notices.forPublication(one).size(), 1);
        QCOMPARE(notices.forPublication(one).first().date, QStringLiteral("2022-01-02"));
        QCOMPARE(notices.forPublication(two).size(), 0);
        QCOMPARE(notices.forPublication(three).first().kind, QStringLiteral("correction"));
        QCOMPARE(notices.forPublication(comma).size(), 0);
        QCOMPARE(notices.warningCount(), qint64(1));
        QVERIFY(changed.first().first().value<QList<qint64>>().contains(one));
        PublicationFilter retracted;
        retracted.retracted = true;
        QCOMPARE(LibraryQuery(*m_db).ids(retracted, {}), QList<qint64>{one});
        QCOMPARE(LibraryQuery(*m_db).flagCounts({}).retracted, qint64(1));

        // Checked, so not asked about again for a while.
        finished.clear();
        checker.start();
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(finished.first().at(0).toInt(), 0);
        QCOMPARE(m_fake->count(), 2);

        // A notice that's gone when checked again is gone here too.
        m_fake->answers.clear();
        m_fake->answer(QStringLiteral("updates"), R"({"message":{"items":[]}})");
        checker.setRecheckAfterDays(0);
        checker.setContactEmail(QStringLiteral("someone@example.org"));
        // A run only checks again what was checked before it started, and Windows' clock moves in steps of about
        // 15 ms, so on a fast machine the last check can share the new run's start. Let the clock move on first.
        auto lastCheck = m_db->prepare("SELECT max(checked_at) FROM notice_check");
        QVERIFY(lastCheck->step());
        const qint64 checkedAt = lastCheck->int64At(0);
        QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentMSecsSinceEpoch() > checkedAt, kWaitMs);
        finished.clear();
        checker.start();
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(notices.warningCount(), qint64(0));
        QVERIFY(m_fake->asked.last().contains(QStringLiteral("mailto")));
    }

    void relatedPapersAreFoundByDoiAloneAndKept()
    {
        const qint64 paper = addPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"), false);
        const qint64 earlier = addPublication(QStringLiteral("A Made-Up Earlier Study"), QStringLiteral("10.1000/earlier"), false);
        // Most specific first, since the first answer whose text is in the address is given.
        m_fake->answer(QStringLiteral("filter=openalex"),
                       "{\"meta\":{\"count\":2},\"results\":["
                       "{\"id\":\"https://openalex.org/W2\",\"doi\":\"https://doi.org/10.1000/earlier\",\"title\":\"A Made-Up Earlier Study\","
                       "\"publication_year\":2001,\"cited_by_count\":5,\"type\":\"article\"},"
                       "{\"id\":\"https://openalex.org/W1\",\"doi\":\"https://doi.org/10.1000/other\",\"title\":\"Another Made-Up Paper\","
                       "\"publication_year\":2003,\"cited_by_count\":9,\"type\":\"preprint\",\"authorships\":[{\"author\":{\"display_name\":\"Ada Example\"}}]}]}");
        m_fake->answer(QStringLiteral("filter=cites"),
                       "{\"meta\":{\"count\":120},\"results\":["
                       "{\"id\":\"https://openalex.org/W9\",\"title\":\"A Later Made-Up Paper\",\"cited_by_count\":40,\"type\":\"article\"},"
                       "{\"id\":\"not an id\",\"title\":\"Something Odd\",\"cited_by_count\":1}]}");
        m_fake->answer(QStringLiteral("/works/doi"),
                       "{\"id\":\"https://openalex.org/W100\",\"cited_by_count\":120,\"referenced_works\":"
                       "[\"https://openalex.org/W1\",\"https://openalex.org/W2\",\"javascript:alert(1)\",\"https://openalex.org/W1\"]}");

        RelatedFinder finder(*m_db, *m_transport);
        QSignalSpy found(&finder, &RelatedFinder::found);
        QSignalSpy failed(&finder, &RelatedFinder::failed);
        finder.find(paper, false);
        QTRY_VERIFY_WITH_TIMEOUT(!found.isEmpty() || !failed.isEmpty(), kWaitMs);
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.first().at(1).toString() + u' '
                                                  + failed.first().at(2).toString() + u' ' + m_fake->asked.join(u' ')));

        // The DOI, then what it cites by OpenAlex's ids, then what cites it. Nothing else about it goes.
        QCOMPARE(m_fake->count(), 3);
        QVERIFY2(m_fake->asked.at(0).contains(QStringLiteral("doi:10.1000/made.up")), qPrintable(m_fake->asked.at(0)));
        QVERIFY2(m_fake->asked.at(1).contains(QStringLiteral("W1")) && m_fake->asked.at(1).contains(QStringLiteral("W2")),
                 qPrintable(m_fake->asked.at(1)));
        QVERIFY(m_fake->asked.at(2).contains(QStringLiteral("W100")));
        const QString asked = m_fake->asked.join(u'\n');
        QVERIFY(!asked.contains(QStringLiteral("alert")));
        QVERIFY(!asked.contains(QStringLiteral("Made")));
        QVERIFY(!asked.contains(QStringLiteral("mailto")));

        // In the order the paper cites them, marking the one already here.
        RelatedPapers papers = relatedPapersFor(*m_db, paper);
        QVERIFY(papers.fetched);
        QCOMPARE(papers.references.size(), 2);
        QCOMPARE(papers.references.at(0).openAlexId, QStringLiteral("W1"));
        QVERIFY(!papers.references.at(0).inLibrary);
        QCOMPARE(papers.references.at(1).inLibrary.value_or(0), earlier);
        QCOMPARE(papers.citedBy.size(), 1);
        QCOMPARE(papers.citedByTotal, 120);
        QCOMPARE(papers.references.at(0).record.publication.authors.first().family, QStringLiteral("Example"));

        // Kept, so asking again doesn't go online.
        found.clear();
        finder.find(paper, false);
        QTRY_COMPARE_WITH_TIMEOUT(found.size(), 1, kWaitMs); // At once, from what's kept.
        QCOMPARE(m_fake->count(), 3);

        // Added without a file, it's then in the library. A preprint is only ever a hint.
        const qint64 added = addRelatedToLibrary(*m_db, papers.references.at(0).record);
        const Publication made = *m_publications->get(added);
        QCOMPARE(made.title, QStringLiteral("Another Made-Up Paper"));
        QCOMPARE(made.doi, QStringLiteral("10.1000/other"));
        QVERIFY(made.type != PublicationType::Preprint);
        QVERIFY(made.extraJson.contains(QStringLiteral("preprintHint")));
        QCOMPARE(relatedPapersFor(*m_db, paper).references.at(0).inLibrary.value_or(0), added);

        // With no DOI there's nothing to ask about.
        const qint64 none = addPublication(QStringLiteral("Made Up With No DOI"), QString(), false);
        finder.find(none, false);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, kWaitMs);
        QCOMPARE(failed.first().at(1).toInt(), int(RelatedProblem::NoDoi));
        QCOMPARE(m_fake->count(), 3);

        // Out of allowance, it says so and keeps what it had.
        TransportReply limited;
        limited.status = 429;
        m_fake->oneOff << limited;
        failed.clear();
        finder.find(paper, true);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, kWaitMs);
        QCOMPARE(failed.first().at(1).toInt(), int(RelatedProblem::RateLimited));
        QCOMPARE(relatedPapersFor(*m_db, paper).references.size(), 2);
    }

    void aFailedNoticeCheckIsTriedAgainLater()
    {
        const qint64 one = addPublication(QStringLiteral("Made Up Study One"), QStringLiteral("10.1000/one.1"), false);
        m_fake->answer(QStringLiteral("updates"), QByteArray(), 503);
        NoticeChecker checker(*m_db, *m_transport);
        checker.setIntervalMs(0);
        QSignalSpy finished(&checker, &NoticeChecker::finished);
        QSignalSpy problem(&checker, &NoticeChecker::problem);
        checker.start();
        QVERIFY(finished.wait(kWaitMs));
        QCOMPARE(problem.size(), 1);
        QCOMPARE(finished.first().at(0).toInt(), 0);
        QCOMPARE(NoticeRepository(*m_db).dueForCheck(10, 0).size(), 1);
        QCOMPARE(NoticeRepository(*m_db).dueForCheck(10, 0).first().first, one);
    }

    void aPreprintIsOnlyEverAHint()
    {
        const auto record = parseCrossrefWork(fixture("crossref_preprint.json"));
        QVERIFY(record.has_value());
        QVERIFY(record->preprintHint);
        QVERIFY(!record->type.has_value());
    }

    void readsBooksAndIsbns()
    {
        const auto crossref = parseCrossrefWork(fixture("crossref_book.json"));
        QVERIFY(crossref.has_value());
        QCOMPARE(crossref->type.value(), PublicationType::Book);
        QCOMPARE(crossref->publication.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(crossref->publication.isbn10, QStringLiteral("0306406152"));

        const auto library = parseOpenLibrary(fixture("openlibrary.json"));
        QVERIFY(library.has_value());
        QCOMPARE(library->publication.title, QStringLiteral("Principles of Sample Engineering"));
        QCOMPARE(library->publication.subtitle, QStringLiteral("A Primer"));
        QCOMPARE(library->publication.authors.at(0).family, QStringLiteral("Example"));
        QCOMPARE(library->publication.publisher, QStringLiteral("Example Press"));
        QCOMPARE(library->publication.place, QStringLiteral("Exampleville"));
        QCOMPARE(library->publication.year.value_or(0), 2018);
    }

    void readsOpenAlexAndItsListedAbstract()
    {
        const auto record = parseOpenAlexWork(fixture("openalex_work.json"));
        QVERIFY(record.has_value());
        const Publication &p = record->publication;
        QCOMPARE(p.doi, QStringLiteral("10.1000/example.2020.7"));
        QCOMPARE(p.abstract, QStringLiteral("A made-up abstract about steel."));
        QCOMPARE(p.pages, QStringLiteral("101-120"));
        QCOMPARE(p.pmid, QStringLiteral("32939066"));
        QCOMPARE(p.containerTitle, QStringLiteral("Journal of Made-up Results"));
        QCOMPARE(p.authors.at(1).family, QStringLiteral("Sample"));
        QCOMPARE(record->type.value(), PublicationType::JournalArticle);
    }

    void readsArxiv()
    {
        const auto record = parseArxiv(fixture("arxiv.xml"));
        QVERIFY(record.has_value());
        QCOMPARE(record->publication.title, QStringLiteral("Sampling Made Simple"));
        QCOMPARE(record->publication.arxivId, QStringLiteral("2101.00001"));
        QCOMPARE(record->publication.authors.size(), 2);
        QCOMPARE(record->publication.year.value_or(0), 2021);
        QVERIFY(record->preprintHint);
        QVERIFY(!parseArxiv(fixture("arxiv_empty.xml")).has_value());
    }

    void readsSearchResults()
    {
        QCOMPARE(parseCrossrefSearch(fixture("crossref_search.json")).size(), 3);
        QCOMPARE(parseOpenAlexSearch(fixture("openalex_search.json")).size(), 1);
    }

    void aBadAnswerIsJustNoAnswer_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not JSON or XML") << QByteArray("<html>Service unavailable");
        QTest::newRow("cut off") << fixture("crossref_work.json").left(100);
        QTest::newRow("the wrong shape") << QByteArray("[1, 2, 3]");
        QTest::newRow("no title") << QByteArray("{\"message\": {\"DOI\": \"10.1/x\"}}");
        QTest::newRow("wrong types") << QByteArray("{\"message\": {\"title\": 7, \"author\": \"x\", \"issued\": []}}");
    }

    void aBadAnswerIsJustNoAnswer()
    {
        QFETCH(QByteArray, body);
        QVERIFY(!parseCrossrefWork(body).has_value());
        QVERIFY(parseCrossrefSearch(body).isEmpty());
        QVERIFY(!parseOpenAlexWork(body).has_value() || parseOpenAlexWork(body)->publication.title.isEmpty());
        QVERIFY(parseOpenAlexSearch(body).isEmpty());
        QVERIFY(!parseArxiv(body).has_value());
        QVERIFY(!parseOpenLibrary(body).has_value());
    }

    void aRecordSurvivesBeingStored()
    {
        const FoundRecord record = *parseCrossrefWork(fixture("crossref_work.json"));
        const FoundRecord back = FoundRecord::fromJson(record.toJson());
        QCOMPARE(back.publication.title, record.publication.title);
        QCOMPARE(back.publication.authors, record.publication.authors);
        QCOMPARE(back.publication.year, record.publication.year);
        QCOMPARE(back.type, record.type);
        QCOMPARE(back.provider, record.provider);
    }

    void identifiersCantChangeWhereARequestGoes()
    {
        const ProviderUrls urls;
        const QUrl url = crossrefWorkUrl(urls, QStringLiteral("10.1000/a?b=c#d/../../evil"));
        QCOMPARE(url.host(), QStringLiteral("api.crossref.org"));
        QVERIFY(!url.hasFragment());
        QVERIFY(!url.hasQuery());
        QCOMPARE(openAlexWorkUrl(urls, QStringLiteral("10.1000/a?b#c")).host(), QStringLiteral("api.openalex.org"));
        QVERIFY(!openAlexWorkUrl(urls, QStringLiteral("10.1000/a?b#c")).hasFragment());
        QVERIFY(crossrefSearchUrl(urls, QStringLiteral("a&filter=x#y")).toString(QUrl::FullyEncoded).contains(QStringLiteral("a%26filter%3Dx%23y")));
    }

    // ---- Deciding what's a match ----

    void comparesTitles()
    {
        QCOMPARE(titleSimilarity(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("fatigue of steel beams")), 1.0);
        QCOMPARE(titleSimilarity(QStringLiteral("The Fatigue of Steel Beams"), QStringLiteral("Fatigue of Steel Beams")), 1.0);
        QCOMPARE(titleSimilarity(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("Fatigue of Steel Beams: A Study")), 1.0);
        QVERIFY(titleSimilarity(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("Cooking with Herbs")) < 0.2);
        QVERIFY(titleSimilarity(QStringLiteral("Fatigue_of-Steel Beams"), QStringLiteral("Fatigue of Steel Beams")) > 0.99);
        QCOMPARE(titleSimilarity(QString(), QStringLiteral("Anything")), 0.0);
    }

    void authorsAndYearsCountTowardsAMatch()
    {
        Publication mine;
        mine.title = QStringLiteral("Fatigue of Steel Beams");
        mine.year = 2020;
        mine.authors = {{QStringLiteral("Example"), QStringLiteral("Ada")}};

        FoundRecord same;
        same.publication = mine;
        FoundRecord otherAuthor = same;
        otherAuthor.publication.authors = {{QStringLiteral("Other"), QString()}};
        FoundRecord otherYear = same;
        otherYear.publication.year = 1990;
        FoundRecord otherTitle = same;
        otherTitle.publication.title = QStringLiteral("Cooking with Herbs");

        QCOMPARE(matchScore(mine, same), 1.0);
        QVERIFY(matchScore(mine, otherAuthor) < matchScore(mine, same));
        QVERIFY(matchScore(mine, otherYear) < matchScore(mine, same));
        QVERIFY(matchScore(mine, otherTitle) < 0.5);
    }

    void anIdentifierNeedsATitleThatFits()
    {
        Publication mine;
        mine.title = QStringLiteral("Cooking with Herbs");
        FoundRecord found;
        found.publication.title = QStringLiteral("Fatigue of Steel Beams");
        QVERIFY(!titleAgrees(mine, {}, found));
        // A poor title from a file's metadata isn't held against it, if the file name agrees.
        QVERIFY(titleAgrees(mine, {QStringLiteral("Fatigue of Steel Beams.pdf")}, found));
    }

    void whatAResultChanges()
    {
        Publication mine;
        mine.title = QStringLiteral("fatigue of beams");
        mine.abstract = QStringLiteral("My own abstract.");
        mine.doi = QStringLiteral("10.1000/mine");
        mine.publisher = QStringLiteral("Wrong Press");
        mine.containerTitle = QStringLiteral("Set By Hand");
        mine.type = PublicationType::Other;

        const FoundRecord found = *parseCrossrefWork(fixture("crossref_work.json"));
        const QList<FieldChange> changes = plannedChanges(mine, found, {QStringLiteral("container_title")});
        const auto changed = [&](const char *name) {
            return std::any_of(changes.cbegin(), changes.cend(), [&](const FieldChange &c) { return c.field == QLatin1StringView(name); });
        };
        QVERIFY(changed("title"));
        QVERIFY(changed("authors"));
        QVERIFY(changed("year"));
        QVERIFY(changed("publisher"));
        QVERIFY(changed("type"));
        QVERIFY(!changed("container_title")); // Set by hand.
        QVERIFY(!changed("abstract"));        // Already has one.
        QVERIFY(!changed("doi"));             // Identifiers aren't replaced.
    }

    void aTypeOnlyMovesBetweenGroups()
    {
        const auto typeChange = [](PublicationType mine, PublicationType found) {
            Publication p;
            p.type = mine;
            FoundRecord record;
            record.type = found;
            const QList<FieldChange> changes = plannedChanges(p, record, {});
            return std::any_of(changes.cbegin(), changes.cend(), [](const FieldChange &c) { return c.field == QLatin1StringView("type"); });
        };
        QVERIFY(typeChange(PublicationType::Other, PublicationType::JournalArticle));
        QVERIFY(typeChange(PublicationType::Book, PublicationType::JournalArticle));
        QVERIFY(!typeChange(PublicationType::Textbook, PublicationType::Book)); // A textbook is more exact.
        QVERIFY(!typeChange(PublicationType::Thesis, PublicationType::Dissertation));
        QVERIFY(!typeChange(PublicationType::Preprint, PublicationType::JournalArticle)); // Only the user decides.
        QVERIFY(!typeChange(PublicationType::JournalArticle, PublicationType::Preprint));
    }

    // ---- The modes ----

    void reviewModeAppliesAnExactMatchThatFits()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), true);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        const LookupSummary summary = run();

        QCOMPARE(summary.applied, 1);
        const Publication p = *m_publications->get(id);
        QCOMPARE(p.title, QStringLiteral("Fatigue of Steel Beams & Columns"));
        QCOMPARE(p.containerTitle, QStringLiteral("Journal of Made-up Results"));
        QCOMPARE(p.type, PublicationType::JournalArticle);
        QVERIFY(!p.needsReview); // The match settles any doubt.
        QCOMPARE(m_publications->provenance(id, QStringLiteral("title")).first().source, QStringLiteral("crossref"));
        QCOMPARE(m_publications->provenance(id, QStringLiteral("title")).first().previousValue, QStringLiteral("Fatigue of Steel Beams"));
        QCOMPARE(m_lookups->waitingForReviewCount(), 0);
    }

    void reviewModeHoldsAnIdentifierWhoseTitleDoesntFit()
    {
        // A DOI read from a file could be one it cites.
        const qint64 id = addPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/example.2020.7"), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        const LookupSummary summary = run();

        QCOMPARE(summary.forReview, 1);
        QCOMPARE(summary.applied, 0);
        QCOMPARE(m_publications->get(id)->title, QStringLiteral("Cooking with Herbs"));
        QCOMPARE(m_lookups->waitingForReviewCount(), 1);
        QCOMPARE(m_lookups->candidatesFor(id).first().matchedBy, QStringLiteral("doi"));
    }

    void automaticModeAppliesItAnywayButMarksIt()
    {
        const qint64 id = addPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/example.2020.7"), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        QCOMPARE(run().applied, 1);
        const Publication p = *m_publications->get(id);
        QCOMPARE(p.title, QStringLiteral("Fatigue of Steel Beams & Columns"));
        QVERIFY(p.needsReview);
        QCOMPARE(m_lookups->waitingForReviewCount(), 0);
    }

    void reviewModeHoldsSearchResultsBestFirst()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams in Practice"), QString(), true);
        addAuthor(id, QStringLiteral("Example"));
        m_fake->answer(QStringLiteral("query.bibliographic"), fixture("crossref_search.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().forReview, 1);

        QCOMPARE(m_publications->get(id)->doi, QString());
        const QList<LookupCandidate> candidates = m_lookups->candidatesFor(id);
        QVERIFY(candidates.size() >= 2);
        const FoundRecord best = FoundRecord::fromJson(QJsonDocument::fromJson(candidates.first().recordJson.toUtf8()).object());
        QCOMPARE(best.publication.doi, QStringLiteral("10.1000/found.it"));
        QVERIFY(candidates.first().score > candidates.at(1).score);
    }

    void automaticModeAppliesTheBestSearchResultAndMarksIt()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams in Practice"), QString(), false);
        addAuthor(id, QStringLiteral("Example"));
        m_fake->answer(QStringLiteral("query.bibliographic"), fixture("crossref_search.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        QCOMPARE(run().applied, 1);
        const Publication p = *m_publications->get(id);
        QCOMPARE(p.doi, QStringLiteral("10.1000/found.it"));
        QVERIFY(p.needsReview);
    }

    void aPoorSearchResultIsntOfferedAtAll()
    {
        const qint64 id = addPublication(QStringLiteral("Something Else Entirely About Herbs"), QString(), false);
        m_fake->answer(QStringLiteral("query.bibliographic"), fixture("crossref_search.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        const LookupSummary summary = run();
        QCOMPARE(summary.applied, 0);
        QCOMPARE(summary.notFound, 1);
        QCOMPARE(m_publications->get(id)->doi, QString());
    }

    // ---- The sources in turn ----

    void openAlexIsAskedWhenCrossrefDoesntKnow()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), QByteArray(), 404);
        m_fake->answer(QStringLiteral("api.openalex.org/works/doi:"), fixture("openalex_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().applied, 1);
        QCOMPARE(m_publications->get(id)->pmid, QStringLiteral("32939066"));
        QCOMPARE(m_publications->provenance(id, QStringLiteral("pmid")).first().source, QStringLiteral("openalex"));
    }

    void anArxivNumberAsksArxiv()
    {
        Publication p;
        p.title = QStringLiteral("Sampling Made Simple");
        p.arxivId = QStringLiteral("2101.00001v2");
        const qint64 id = m_publications->create(p);
        m_fake->answer(QStringLiteral("export.arxiv.org"), fixture("arxiv.xml"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().applied, 1);
        QVERIFY(m_fake->asked.first().contains(QStringLiteral("id_list=2101.00001")));
        QVERIFY(!m_fake->asked.first().contains(QStringLiteral("v2")));

        // It says it's a preprint, which is noted, and the type stays as it was.
        const Publication after = *m_publications->get(id);
        QVERIFY(after.extraJson.contains(QStringLiteral("preprintHint")));
        QCOMPARE(after.type, PublicationType::Other);
        QCOMPARE(after.authors.size(), 2);
    }

    void anIsbnAsksOpenLibrary()
    {
        Publication p;
        p.title = QStringLiteral("Principles of Sample Engineering");
        p.isbn13 = QStringLiteral("9780306406157");
        const qint64 id = m_publications->create(p);
        m_fake->answer(QStringLiteral("openlibrary.org"), fixture("openlibrary.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().applied, 1);
        QCOMPARE(m_publications->get(id)->publisher, QStringLiteral("Example Press"));
        QVERIFY(!m_fake->asked.first().contains(QStringLiteral("api.crossref.org")));
    }

    void openAlexSearchIsOnlyAskedIfCrossrefDoesntFindIt()
    {
        // Its searches use up a daily allowance, so they're kept for when they're needed.
        const qint64 found = addPublication(QStringLiteral("Fatigue of Steel Beams in Practice"), QString(), false);
        addAuthor(found, QStringLiteral("Example"));
        m_fake->answer(QStringLiteral("query.bibliographic"), fixture("crossref_search.json"));
        m_fake->answer(QStringLiteral("api.openalex.org/works?search"), fixture("openalex_search.json"));
        m_lookups->enqueue({found}, QStringLiteral("online_auto"));
        run();
        QVERIFY(!m_fake->asked.join(u' ').contains(QStringLiteral("api.openalex.org")));

        const qint64 elsewhere = addPublication(QStringLiteral("An Unusual Report on Example Bridges"), QString(), false);
        m_lookups->enqueue({elsewhere}, QStringLiteral("online_auto"));
        run();
        QCOMPARE(m_publications->get(elsewhere)->doi, QStringLiteral("10.1000/from.openalex"));
    }

    void aTitleTooShortToTellApartIsntSearchedFor()
    {
        const qint64 id = addPublication(QStringLiteral("Notes"), QString(), false);
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        QCOMPARE(run().notFound, 1);
        QCOMPARE(m_fake->count(), 0);
    }

    // ---- Being a good guest ----

    void onlyIdentifiersAndTitlesAreSent()
    {
        Publication p;
        p.title = QStringLiteral("Fatigue of Steel Beams in Practice");
        p.authors = {{QStringLiteral("Secretauthorname"), QString()}};
        p.notes = QStringLiteral("Secretnotes");
        const qint64 id = m_publications->create(p);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();

        const QString all = m_fake->asked.join(u' ');
        QVERIFY(!all.isEmpty());
        QVERIFY(!all.contains(QStringLiteral("Secret"), Qt::CaseInsensitive));
    }

    void aContactEmailOnlyGoesIfGivenAndOnlyWhereAskedFor()
    {
        Publication p;
        p.title = QStringLiteral("Sampling Made Simple");
        p.doi = QStringLiteral("10.1000/a");
        p.arxivId = QStringLiteral("2101.00001");
        p.isbn13 = QStringLiteral("9780306406157");
        const qint64 id = m_publications->create(p);

        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();
        QVERIFY(!m_fake->asked.join(u' ').contains(QStringLiteral("mailto")));
        QVERIFY(m_fake->asked.size() >= 3);

        m_fake->asked.clear();
        m_db->exec("DELETE FROM lookup_cache");
        m_engine->setContactEmail(QStringLiteral("someone@example.org"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();
        for (const QString &url : std::as_const(m_fake->asked)) {
            const bool asksForIt = url.contains(QStringLiteral("crossref")) || url.contains(QStringLiteral("openalex"));
            QCOMPARE(url.contains(QStringLiteral("mailto=someone%40example.org")), asksForIt);
        }
    }

    void anOpenAlexKeyGoesOnlyToOpenAlexInAHeader()
    {
        Publication p;
        p.title = QStringLiteral("Sampling Made Simple");
        p.doi = QStringLiteral("10.1000/a");
        p.arxivId = QStringLiteral("2101.00001");
        const qint64 id = m_publications->create(p);
        m_fake->answer(QStringLiteral("api.openalex.org"), fixture("openalex_work.json"));

        const QString key = QStringLiteral("madeUpKey123");
        m_engine->setKey(kOpenAlex, key);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();

        bool openAlexAsked = false;
        for (qsizetype i = 0; i < m_fake->asked.size(); ++i) {
            const QString &url = m_fake->asked.at(i);
            QVERIFY2(!url.contains(key), qPrintable(url));
            const bool openAlex = url.contains(QStringLiteral("openalex"));
            openAlexAsked = openAlexAsked || openAlex;
            QCOMPARE(m_fake->headers.at(i).contains("Authorization: Bearer " + key.toLatin1()), openAlex);
        }
        QVERIFY(openAlexAsked);

        // Nothing keeps it: not the answers kept, nor the database at all.
        auto cached = m_db->prepare("SELECT count(*) FROM lookup_cache WHERE key LIKE ?");
        cached->bind(1, QStringLiteral("%%1%").arg(key));
        QVERIFY(cached->step());
        QCOMPARE(cached->int64At(0), 0);
        cached.reset();
        auto checkpoint = m_db->prepare("PRAGMA wal_checkpoint(TRUNCATE)");
        checkpoint->step();
        checkpoint.reset();
        QFile file(m_dir->filePath(QStringLiteral("library.db")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(key.toLatin1()));
    }

    void aKeyOpenAlexRefusesIsDroppedAndItsAskedWithout()
    {
        Publication p;
        p.title = QStringLiteral("A Made-Up Study of Bridges");
        p.doi = QStringLiteral("10.1000/made.up");
        const qint64 id = m_publications->create(p);
        TransportReply refusal;
        refusal.status = 403;
        refusal.body = "{\"error\":\"key_not_activated\"}";
        m_fake->keyedAnswers << qMakePair(QStringLiteral("api.openalex.org"), refusal);
        m_fake->answer(QStringLiteral("api.openalex.org"), fixture("openalex_work.json"));

        QSignalSpy refused(m_engine.get(), &LookupEngine::keyRefused);
        m_engine->setKey(kOpenAlex, QStringLiteral("notActivated"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();

        QCOMPARE(refused.size(), 1);
        QCOMPARE(refused.first().first().toString(), kOpenAlex);
        // Asked once with it, then again without, and never with it after.
        int keyed = 0;
        int plain = 0;
        for (qsizetype i = 0; i < m_fake->asked.size(); ++i) {
            if (!m_fake->asked.at(i).contains(QStringLiteral("openalex")))
                continue;
            if (m_fake->headers.at(i).isEmpty())
                ++plain;
            else
                ++keyed;
        }
        QCOMPARE(keyed, 1);
        QVERIFY(plain >= 1);
    }

    void relatedPapersSendTheKeyInAHeaderAndCarryOnWithoutOneRefused()
    {
        const qint64 paper = addPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"), false);
        m_fake->answer(QStringLiteral("filter="), "{\"meta\":{\"count\":0},\"results\":[]}");
        m_fake->answer(QStringLiteral("/works/doi"), "{\"id\":\"https://openalex.org/W100\",\"cited_by_count\":0,\"referenced_works\":[]}");

        RelatedFinder finder(*m_db, *m_transport);
        finder.setOpenAlexKey(QStringLiteral("madeUpKey123"));
        QSignalSpy found(&finder, &RelatedFinder::found);
        QSignalSpy refused(&finder, &RelatedFinder::keyRefused);
        finder.find(paper, false);
        QTRY_VERIFY_WITH_TIMEOUT(!found.isEmpty(), kWaitMs);
        QVERIFY(refused.isEmpty());
        QVERIFY(!m_fake->asked.isEmpty());
        for (qsizetype i = 0; i < m_fake->asked.size(); ++i) {
            QVERIFY(!m_fake->asked.at(i).contains(QStringLiteral("madeUpKey123")));
            QVERIFY(m_fake->headers.at(i).contains("Authorization: Bearer madeUpKey123"));
        }

        m_fake->asked.clear();
        m_fake->headers.clear();
        TransportReply refusal;
        refusal.status = 401;
        m_fake->keyedAnswers << qMakePair(QStringLiteral("api.openalex.org"), refusal);
        finder.find(paper, true);
        QTRY_VERIFY_WITH_TIMEOUT(found.size() == 2, kWaitMs);
        QCOMPARE(refused.size(), 1);
        QVERIFY(m_fake->headers.first().contains("Authorization"));
        for (qsizetype i = 1; i < m_fake->headers.size(); ++i)
            QVERIFY(m_fake->headers.at(i).isEmpty());
    }

    void aKeyThatCouldBreakAHeaderIsntSent()
    {
        QCOMPARE(keyHeaders(kOpenAlex, QStringLiteral("abc123")).size(), 1);
        QVERIFY(keyHeaders(kOpenAlex, QStringLiteral("abc\r\nX-Other: 1")).isEmpty());
        QVERIFY(keyHeaders(kOpenAlex, QStringLiteral("  ")).isEmpty());
        QVERIFY(keyHeaders(kCrossref, QStringLiteral("abc123")).isEmpty());
    }

    // ---- Court cases

    void caseSourcesAnswersAreRead()
    {
        const QList<FoundRecord> uk = parseFindCaseLaw(fixture("findcaselaw.atom"));
        QCOMPARE(uk.size(), 2);
        QCOMPARE(uk.at(0).publication.url, QString()); // Not a page of its own site.
        const Publication &supreme = uk.at(1).publication;
        QCOMPARE(supreme.title, QStringLiteral("Made-Up Holdings Ltd v Example Council"));
        QCOMPARE(supreme.neutralCitation, QStringLiteral("[2021] UKSC 99"));
        QCOMPARE(supreme.court, QStringLiteral("United Kingdom Supreme Court"));
        QCOMPARE(supreme.jurisdiction, QStringLiteral("United Kingdom"));
        QCOMPARE(supreme.year.value_or(0), 2021);
        QCOMPARE(supreme.month.value_or(0), 3);
        QCOMPARE(supreme.day.value_or(0), 4);
        QCOMPARE(supreme.url, QStringLiteral("https://caselaw.nationalarchives.gov.uk/uksc/2021/99"));
        QCOMPARE(supreme.caseCitationForm, QStringLiteral("uk"));
        QCOMPARE(uk.at(0).publication.jurisdiction, QStringLiteral("England and Wales"));
        QVERIFY(uk.at(1).type && *uk.at(1).type == PublicationType::CaseLaw);

        const QList<FoundRecord> us = parseCourtListenerSearch(fixture("courtlistener_search.json"));
        QCOMPARE(us.size(), 2);
        const Publication &sample = us.at(0).publication;
        QCOMPARE(sample.title, QStringLiteral("Sample v. Example County"));
        // The official report, wherever it comes in the list.
        QCOMPARE(sample.volume, QStringLiteral("999"));
        QCOMPARE(sample.containerTitle, QStringLiteral("U.S."));
        QCOMPARE(sample.pages, QStringLiteral("111"));
        QCOMPARE(sample.court, QStringLiteral("U.S."));
        QCOMPARE(sample.caseNumber, QStringLiteral("No. 19-9999"));
        QCOMPARE(sample.url, QStringLiteral("https://www.courtlistener.com/opinion/1234/sample-v-example-county/"));
        QCOMPARE(sample.caseCitationForm, QStringLiteral("us"));
        QCOMPARE(us.at(1).publication.court, QStringLiteral("9th Cir."));
        QCOMPARE(us.at(1).publication.url, QString()); // Another site's address isn't kept.

        const QList<FoundRecord> eu = parseEurLex(fixture("eurlex_sparql.json"));
        QCOMPARE(eu.size(), 1);
        QCOMPARE(eu.first().publication.title, QStringLiteral("Made-Up Association v Example Authority"));
        QCOMPARE(eu.first().publication.neutralCitation, QStringLiteral("ECLI:EU:C:2019:999"));
        QCOMPARE(eu.first().publication.caseNumber, QStringLiteral("C-999/18"));
        QCOMPARE(eu.first().publication.court, QStringLiteral("Court of Justice"));
        QCOMPARE(eu.first().publication.day.value_or(0), 5);

        const std::optional<FoundRecord> ca = parseCanLii(fixture("canlii_case.json"));
        QVERIFY(ca);
        QCOMPARE(ca->publication.neutralCitation, QStringLiteral("2022 SCC 99"));
        QCOMPARE(ca->publication.court, QStringLiteral("Supreme Court of Canada"));
        QCOMPARE(ca->publication.url, QStringLiteral("https://canlii.ca/t/zz999"));

        // Nonsense is no answer.
        QVERIFY(parseFindCaseLaw("<not xml").isEmpty());
        QVERIFY(parseCourtListenerSearch("{\"results\": 5}").isEmpty());
        QVERIFY(parseEurLex("[]").isEmpty());
        QVERIFY(!parseCanLii("{\"title\": \"x\", \"citation\": \"not one\"}"));
    }

    void casesAreToldApartByJurisdiction()
    {
        Publication p;
        p.type = PublicationType::CaseLaw;
        p.neutralCitation = QStringLiteral("[2021] UKSC 99");
        QCOMPARE(caseJurisdictionOf(p), CaseJurisdiction::UnitedKingdom);
        p.neutralCitation = QStringLiteral("[2020] EWHC 4321 (Ch)");
        QCOMPARE(caseJurisdictionOf(p), CaseJurisdiction::UnitedKingdom);
        p.neutralCitation = QStringLiteral("[2019] CSIH 12");
        p.caseCitationForm = QStringLiteral("uk");
        QCOMPARE(caseJurisdictionOf(p), CaseJurisdiction::Unknown); // Scotland's courts aren't in Find Case Law.
        p.neutralCitation = QStringLiteral("ECLI:EU:C:2019:999");
        QCOMPARE(caseJurisdictionOf(p), CaseJurisdiction::EuropeanUnion);
        p.neutralCitation = QStringLiteral("2022 SCC 99");
        QCOMPARE(caseJurisdictionOf(p), CaseJurisdiction::Canada);

        Publication us;
        us.type = PublicationType::CaseLaw;
        us.containerTitle = QStringLiteral("F.3d");
        QCOMPARE(caseJurisdictionOf(us), CaseJurisdiction::UnitedStates);
        Publication circuit;
        circuit.court = QStringLiteral("9th Cir.");
        QCOMPARE(caseJurisdictionOf(circuit), CaseJurisdiction::UnitedStates);
        Publication eu;
        eu.caseNumber = QStringLiteral("Case C-999/18");
        QCOMPARE(caseJurisdictionOf(eu), CaseJurisdiction::EuropeanUnion);
        QCOMPARE(celexForCaseNumber(eu.caseNumber), QStringLiteral("62018CJ0999"));
        QCOMPARE(celexForCaseNumber(QStringLiteral("T-12/05")), QStringLiteral("62005TJ0012"));
        Publication nowhere;
        nowhere.jurisdiction = QStringLiteral("Australia");
        nowhere.caseCitationForm = QStringLiteral("uk");
        QCOMPARE(caseJurisdictionOf(nowhere), CaseJurisdiction::Unknown);
        QCOMPARE(caseJurisdictionOf(Publication{}), CaseJurisdiction::Unknown);

        QVERIFY(canLiiCaseFor(QStringLiteral("2022 SCC 99")));
        QCOMPARE(canLiiCaseFor(QStringLiteral("2022 SCC 99"))->databaseId, QStringLiteral("csc-scc"));
        QCOMPARE(canLiiCaseFor(QStringLiteral("2022 SCC 99"))->caseId, QStringLiteral("2022scc99"));
        QVERIFY(!canLiiCaseFor(QStringLiteral("2022 XYZ 99")));
    }

    void caseNamesAreComparedAsCasesAre()
    {
        QVERIFY(caseNameSimilarity(QStringLiteral("Made-Up BV and others v Sample and others"), QStringLiteral("Made-Up v Sample")) >= 0.8);
        QCOMPARE(caseNameSimilarity(QStringLiteral("R v Example"), QStringLiteral("Regina v. Example")), 1.0);
        QCOMPARE(caseNameSimilarity(QStringLiteral("Sample v. Example County"), QStringLiteral("Sample vs Example County")), 1.0);
        QVERIFY(caseNameSimilarity(QStringLiteral("Sample v Example County"), QStringLiteral("Other Party v Someone Else")) < 0.5);
        // Sharing one party, or the Crown, isn't being the same case.
        QVERIFY(caseNameSimilarity(QStringLiteral("Sample v Example"), QStringLiteral("Sample v Another Made Up Ltd")) < 0.5);
        QVERIFY(caseNameSimilarity(QStringLiteral("R v Sample"), QStringLiteral("R v Example")) < 0.5);
    }

    void aHostileCaseNumberCantChangeWhatsAsked()
    {
        const ProviderUrls urls;
        // Anything after the ECLI itself means it isn't trusted as one, and a case number only ever gives digits.
        QVERIFY(eurLexQueryUrl(urls, QStringLiteral("ECLI:EU:C:2019:1\") } DELETE { ?s ?p ?o"), QString()).isEmpty());
        QCOMPARE(celexForCaseNumber(QStringLiteral("C-1/19\") } DELETE")), QStringLiteral("62019CJ0001"));
        const QString query = QUrlQuery(eurLexQueryUrl(urls, QString(), QStringLiteral("62019CJ0001"))).queryItemValue(QStringLiteral("query"), QUrl::FullyDecoded);
        QVERIFY(query.contains(QStringLiteral("\"62019CJ0001\"")));
        QVERIFY(!query.contains(QStringLiteral("DELETE")));
        // CourtListener's search can't be broken out of either.
        const QString q = QUrlQuery(courtListenerNameUrl(urls, QStringLiteral("Sample\") OR caseName:(\"x"), std::nullopt))
                              .queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
        QCOMPARE(q.count(u'"'), 2);
        QCOMPARE(q.count(u'('), 1);
    }

    void aCaseIsNeverSentToSourcesOfPapers()
    {
        m_fake->answer(QStringLiteral("api.crossref.org"), fixture("crossref_search.json"));
        m_fake->answer(QStringLiteral("api.openalex.org"), fixture("openalex_search.json"));
        Publication p;
        p.type = PublicationType::CaseLaw;
        p.title = QStringLiteral("Made Up Holdings Ltd v Example Council of Somewhere");
        p.doi = QStringLiteral("10.1000/something.it.cites");
        p.jurisdiction = QStringLiteral("Australia");
        const qint64 id = m_publications->create(p);
        m_lookups->enqueue({id}, QStringLiteral("online_auto"), true, true);
        run();
        QCOMPARE(m_fake->count(), 0);
        QCOMPARE(m_publications->get(id)->title, p.title);

        // And a paper's details can never change a case, whatever's asked.
        FoundRecord paper;
        paper.provider = kCrossref;
        paper.type = PublicationType::JournalArticle;
        paper.publication.title = p.title;
        paper.publication.containerTitle = QStringLiteral("A Made-Up Journal");
        QVERIFY(plannedChanges(*m_publications->get(id), paper, {}).isEmpty());
    }

    void aUkCaseIsOnlyAskedOfFindCaseLawWhenChosen()
    {
        m_fake->answer(QStringLiteral("caselaw.nationalarchives.gov.uk"), fixture("findcaselaw.atom"));
        Publication p;
        p.type = PublicationType::CaseLaw;
        p.title = QStringLiteral("Made-Up Holdings Ltd v Example Council");
        p.neutralCitation = QStringLiteral("[2021] UKSC 99");
        p.year = 2021;
        const qint64 id = m_publications->create(p);

        // Not on import, nor for the whole library.
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"), true);
        run();
        QCOMPARE(m_fake->count(), 0);

        m_lookups->enqueue({id}, QStringLiteral("online_review"), true, true);
        run();
        QCOMPARE(m_fake->count(), 1);
        QVERIFY2(m_fake->asked.first().contains(QStringLiteral("UKSC%2099")), qPrintable(m_fake->asked.first()));
        QVERIFY(m_fake->asked.first().contains(QStringLiteral("order=relevance")));
        const Publication after = *m_publications->get(id);
        QCOMPARE(after.type, PublicationType::CaseLaw);
        QCOMPARE(after.court, QStringLiteral("United Kingdom Supreme Court"));
        QCOMPARE(after.jurisdiction, QStringLiteral("United Kingdom"));
        QCOMPARE(after.month.value_or(0), 3);
        QCOMPARE(after.url, QStringLiteral("https://caselaw.nationalarchives.gov.uk/uksc/2021/99"));
        QCOMPARE(undoableSource(*m_db, id), kFindCaseLaw);
        QVERIFY(undoLastLookup(*m_db, id) > 0);
        QCOMPARE(m_publications->get(id)->court, QString());
    }

    void aUsCaseNeedsATokenAndIsFoundByItsReport()
    {
        m_fake->answer(QStringLiteral("www.courtlistener.com"), fixture("courtlistener_search.json"));
        Publication p;
        p.type = PublicationType::CaseLaw;
        p.title = QStringLiteral("Sample v. Example County");
        p.containerTitle = QStringLiteral("U.S.");
        p.volume = QStringLiteral("999");
        p.pages = QStringLiteral("111");
        p.court = QStringLiteral("Set by hand");
        const qint64 id = m_publications->create(p);
        m_db->exec("INSERT INTO metadata_provenance (publication_id, field, source, new_value, recorded_at) "
                   "SELECT id, 'court', 'manual', court, 1 FROM publication");

        m_lookups->enqueue({id}, QStringLiteral("online_auto"), true, true);
        run();
        QCOMPARE(m_fake->count(), 0); // No token, no CourtListener.

        m_engine->setKey(kCourtListener, QStringLiteral("madeUpToken"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"), true, true);
        run();
        QCOMPARE(m_fake->count(), 1);
        QVERIFY2(m_fake->asked.first().contains(QStringLiteral("citation")), qPrintable(m_fake->asked.first()));
        QVERIFY(!m_fake->asked.first().contains(QStringLiteral("madeUpToken")));
        QVERIFY(m_fake->headers.first().contains("Authorization: Token madeUpToken"));
        const Publication after = *m_publications->get(id);
        QCOMPARE(after.caseNumber, QStringLiteral("No. 19-9999"));
        QCOMPARE(after.court, QStringLiteral("Set by hand"));
        QCOMPARE(after.day.value_or(0), 14);
        QVERIFY(!after.needsReview); // Its report matched, and its name agrees.
    }

    void anEuCaseIsFoundByItsCaseNumber()
    {
        m_fake->answer(QStringLiteral("publications.europa.eu"), fixture("eurlex_sparql.json"));
        Publication p;
        p.type = PublicationType::CaseLaw;
        p.title = QStringLiteral("Made-Up Association v Example Authority");
        p.caseNumber = QStringLiteral("Case C-999/18");
        const qint64 id = m_publications->create(p);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();
        QCOMPARE(m_fake->count(), 1);
        QVERIFY(m_fake->asked.first().contains(QStringLiteral("62018CJ0999")));
        const Publication after = *m_publications->get(id);
        QCOMPARE(after.neutralCitation, QStringLiteral("ECLI:EU:C:2019:999"));
        QCOMPARE(after.court, QStringLiteral("Court of Justice"));
        QCOMPARE(after.jurisdiction, QStringLiteral("European Union"));
        QCOMPARE(after.caseNumber, QStringLiteral("Case C-999/18")); // Already there, so left as it was.
    }

    void aCanadianCasesKeyStaysOutOfEverythingButItsRequest()
    {
        Publication p;
        p.type = PublicationType::CaseLaw;
        p.title = QStringLiteral("Sample v Example Corp");
        p.neutralCitation = QStringLiteral("2022 SCC 99");
        const qint64 id = m_publications->create(p);
        m_engine->setKey(kCanLii, QStringLiteral("canKey1"));
        // First the network fails, saying where it was going, as Qt's messages do.
        TransportReply failure;
        failure.error = QStringLiteral("Error transferring https://api.canlii.org/v1/caseBrowse/en/csc-scc/2022scc99/?api_key=canKey1");
        m_fake->oneOff << failure;
        m_fake->answer(QStringLiteral("api.canlii.org"), fixture("canlii_case.json"));
        QSignalSpy problems(m_engine.get(), &LookupEngine::problem);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();

        QVERIFY(!problems.isEmpty());
        for (const QList<QVariant> &problem : std::as_const(problems))
            QVERIFY2(!problem.first().toString().contains(QStringLiteral("canKey1")), qPrintable(problem.first().toString()));
        QVERIFY(m_fake->count() >= 2);
        QVERIFY2(m_fake->asked.last().contains(QStringLiteral("/v1/caseBrowse/en/csc-scc/2022scc99/")), qPrintable(m_fake->asked.last()));
        QVERIFY(m_fake->asked.last().contains(QStringLiteral("api_key=canKey1")));
        QVERIFY(!m_fake->headers.last().isEmpty()); // Sent the way that never follows a redirect.
        auto cached = m_db->prepare("SELECT count(*) FROM lookup_cache WHERE key LIKE '%canKey1%'");
        QVERIFY(cached->step());
        QCOMPARE(cached->int64At(0), 0);
        cached.reset();
        QCOMPARE(m_publications->get(id)->court, QStringLiteral("Supreme Court of Canada"));
    }

    void requestsAreSpacedOut()
    {
        for (int i = 0; i < 4; ++i)
            m_lookups->enqueue({addPublication(QStringLiteral("Title Number %1 Here").arg(i), QStringLiteral("10.1000/%1").arg(i), false)},
                               QStringLiteral("online_review"));
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        run();
        // Crossref's limit is 5 a second, and this keeps below it.
        const QList<qint64> times = crossrefTimes();
        QVERIFY(times.size() >= 4);
        for (qsizetype i = 1; i < times.size(); ++i)
            QVERIFY2(times.at(i) - times.at(i - 1) >= 190, qPrintable(QString::number(times.at(i) - times.at(i - 1))));
    }

    void aLowerLimitFromASourceIsFollowed()
    {
        for (int i = 0; i < 3; ++i)
            m_lookups->enqueue({addPublication(QStringLiteral("Title Number %1 Here").arg(i), QStringLiteral("10.1000/%1").arg(i), false)},
                               QStringLiteral("online_review"));
        TransportReply reply;
        reply.status = 200;
        reply.body = fixture("crossref_work.json");
        reply.rateLimitPerSecond = 1;
        m_fake->answers << qMakePair(QStringLiteral("api.crossref.org/works/"), reply);
        run();
        const QList<qint64> times = crossrefTimes();
        QVERIFY(times.size() >= 3);
        QVERIFY2(times.at(2) - times.at(1) >= 1000, qPrintable(QString::number(times.at(2) - times.at(1))));
    }

    void searchesGoAtTheSlowerPaceCrossrefAllowsThem()
    {
        for (int i = 0; i < 3; ++i)
            m_lookups->enqueue({addPublication(QStringLiteral("Search Title Number %1").arg(i), QString(), false)}, QStringLiteral("online_review"));
        m_fake->answer(QStringLiteral("query.bibliographic"), "{\"message\": {\"items\": []}}");
        run();
        const QList<qint64> times = crossrefTimes();
        QCOMPARE(times.size(), 3);
        for (qsizetype i = 1; i < times.size(); ++i)
            QVERIFY2(times.at(i) - times.at(i - 1) >= 1050, qPrintable(QString::number(times.at(i) - times.at(i - 1))));
    }

    void aSecondLookupDoesntAskAgain()
    {
        const qint64 first = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        const qint64 second = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({first, second}, QStringLiteral("online_review"));
        QCOMPARE(run().applied, 2);
        QCOMPARE(m_fake->count(), 1);
    }

    void notFoundIsRememberedUnlessAskedAgainByHand()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue"), QStringLiteral("10.1000/nothing"), false);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().notFound, 1);
        const int asked = m_fake->count();
        QCOMPARE(asked, 2);

        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        run();
        QCOMPARE(m_fake->count(), asked);

        m_lookups->enqueue({id}, QStringLiteral("online_review"), true);
        run();
        QCOMPARE(m_fake->count(), asked * 2);
    }

    // ---- When things go wrong ----

    void aNetworkFailureKeepsTheJobAndTriesAgain()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        TransportReply failed;
        failed.error = QStringLiteral("no route to host");
        m_fake->oneOff << failed << failed;
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));

        QSignalSpy problems(m_engine.get(), &LookupEngine::problem);
        QCOMPARE(run().applied, 1);
        QCOMPARE(problems.size(), 2);
    }

    void aServerProblemKeepsTheJobToo()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        TransportReply busy;
        busy.status = 503;
        m_fake->oneOff << busy;
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().applied, 1);
    }

    void aSourceOutOfAllowanceDoesntHoldUpTheOthers()
    {
        // OpenAlex's search allowance has run out, and Crossref finds nothing for this one.
        const qint64 first = addPublication(QStringLiteral("Something Nobody Has Heard Of"), QString(), false);
        const qint64 second = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        TransportReply noResults;
        noResults.status = 200;
        noResults.body = "{\"message\": {\"items\": []}}";
        TransportReply outOfAllowance;
        outOfAllowance.status = 429;
        outOfAllowance.retryAfterSeconds = 7200;
        m_fake->answers << qMakePair(QStringLiteral("query.bibliographic"), noResults);
        m_fake->answers << qMakePair(QStringLiteral("api.openalex.org/works?search"), outOfAllowance);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({first, second}, QStringLiteral("online_review"));
        const LookupSummary summary = run();
        QCOMPARE(summary.notFound, 1);
        QCOMPARE(summary.applied, 1);
        QCOMPARE(m_lookups->pendingCount(), 0);
    }

    void whenEveryoneIsOutOfAllowanceItWaits()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        TransportReply limited;
        limited.status = 429;
        limited.retryAfterSeconds = 7200;
        m_fake->answers << qMakePair(QStringLiteral("doi"), limited);
        m_fake->answers << qMakePair(QStringLiteral("works"), limited);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));

        QSignalSpy problems(m_engine.get(), &LookupEngine::problem);
        m_engine->start();
        QVERIFY(problems.wait(kWaitMs));
        // It isn't "not found": it's still waiting to be asked.
        QCOMPARE(m_lookups->pendingCount(), 1);
        m_engine->stop();
    }

    void stoppingKeepsWhatsLeft()
    {
        QList<qint64> ids;
        for (int i = 0; i < 5; ++i)
            ids << addPublication(QStringLiteral("Title Number %1 Here").arg(i), QStringLiteral("10.1000/%1").arg(i), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue(ids, QStringLiteral("online_review"));

        QSignalSpy finished(m_engine.get(), &LookupEngine::finished);
        QSignalSpy progress(m_engine.get(), &LookupEngine::progress);
        m_engine->start();
        QTRY_VERIFY_WITH_TIMEOUT(progress.size() >= 3, kWaitMs);
        m_engine->stop();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, kWaitMs);
        QVERIFY(m_lookups->pendingCount() > 0);
        QVERIFY(m_lookups->pendingCount() < 5);
        QVERIFY(finished.first().at(0).value<LookupSummary>().stopped);

        // And it picks up from there.
        const qint64 left = m_lookups->pendingCount();
        QCOMPARE(run().forReview, int(left));
        QCOMPARE(m_lookups->pendingCount(), 0);
    }

    void closingTheAppMidLookupCarriesOnNextTime()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QVERIFY(m_lookups->nextJob().has_value()); // Claimed, and then the app went away.
        QCOMPARE(run().applied, 1);
        QCOMPARE(m_lookups->pendingCount(), 0);
    }

    void startingAnImportDoesntDisturbARunningLookup()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QString(), false);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QVERIFY(m_lookups->nextJob().has_value());

        import::ImportOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        import::ImportService service(*m_db, options);
        QVERIFY(!m_lookups->nextJob().has_value());
    }

    void aPublicationThatsGoneIsSkipped()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/x"), false);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        m_publications->remove(id);
        run();
        QCOMPARE(m_fake->count(), 0);
        QCOMPARE(m_lookups->pendingCount(), 0);
    }

    // ---- Provenance and undo ----

    void undoPutsBackEverythingALookupChanged()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), true);
        addAuthor(id, QStringLiteral("Hooke"));
        const Publication before = *m_publications->get(id);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        run();
        QVERIFY(m_publications->get(id)->title != before.title);
        QCOMPARE(undoableSource(*m_db, id), kCrossref);

        QVERIFY(undoLastLookup(*m_db, id) > 5);
        const Publication after = *m_publications->get(id);
        QCOMPARE(after.title, before.title);
        QCOMPARE(after.authors, before.authors);
        QCOMPARE(after.containerTitle, QString());
        QCOMPARE(after.year, before.year);
        QCOMPARE(after.month, before.month);
        QCOMPARE(after.type, before.type);
        QCOMPARE(after.doi, before.doi);

        // There's nothing further to undo.
        QCOMPARE(undoableSource(*m_db, id), QString());
        QCOMPARE(undoLastLookup(*m_db, id), 0);
    }

    void undoLeavesWhatsBeenChangedByHandSince()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        run();
        Publication p = *m_publications->get(id);
        p.publisher = QStringLiteral("My Own Press");
        m_publications->updateWithProvenance(p, QStringLiteral("manual"));

        undoLastLookup(*m_db, id);
        QCOMPARE(m_publications->get(id)->publisher, QStringLiteral("My Own Press"));
        QCOMPARE(m_publications->get(id)->containerTitle, QString());
    }

    void aLookupLeavesWhatsSetByHandAlone()
    {
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        Publication p = *m_publications->get(id);
        p.title = QStringLiteral("Fatigue of Steel Beams, My Way");
        p.type = PublicationType::Preprint;
        m_publications->updateWithProvenance(p, QStringLiteral("manual"));

        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_auto"));
        run();
        const Publication after = *m_publications->get(id);
        QCOMPARE(after.title, QStringLiteral("Fatigue of Steel Beams, My Way"));
        QCOMPARE(after.type, PublicationType::Preprint);
        QCOMPARE(after.containerTitle, QStringLiteral("Journal of Made-up Results"));
    }

    // ---- Removed folders ----

    void whatsWaitingForReviewFollowsItsFolder()
    {
        FileRepository files(*m_db);
        const qint64 root = files.addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        const qint64 id = addPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/example.2020.7"), false);
        FileRecord file;
        file.rootId = root;
        file.relativePath = QStringLiteral("a.pdf");
        file.format = QStringLiteral("pdf");
        files.link(id, files.upsert(file), true);

        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_lookups->enqueue({id}, QStringLiteral("online_review"));
        QCOMPARE(run().forReview, 1);
        QCOMPARE(m_lookups->waitingForReviewCount(), 1);

        files.hideRoot(root);
        QCOMPARE(m_lookups->waitingForReviewCount(), 0);
        QVERIFY(m_lookups->publicationsWaitingForReview().isEmpty());

        files.restoreRoot(root);
        QCOMPARE(m_lookups->waitingForReviewCount(), 1);
    }

    void nothingFromARemovedFolderIsLookedUp()
    {
        FileRepository files(*m_db);
        const qint64 root = files.addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        const qint64 id = addPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"), false);
        FileRecord file;
        file.rootId = root;
        file.relativePath = QStringLiteral("a.pdf");
        file.format = QStringLiteral("pdf");
        files.link(id, files.upsert(file), true);
        m_lookups->enqueue({id}, QStringLiteral("online_review"));

        files.hideRoot(root);
        QCOMPARE(m_lookups->pendingCount(), 0);
        run();
        QCOMPARE(m_fake->count(), 0);
    }

    // ---- The real network path, on this computer only ----

    void aFileIsOnlyKeptIfItsExactlyWhatWasExpected()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkTransport transport;
        const QByteArray body = "A made-up model, standing in for a language's.";
        const QByteArray hash = QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();
        const QString path = m_dir->filePath(QStringLiteral("tessdata/made.traineddata"));
        QDir().mkpath(QFileInfo(path).path());

        const auto fetch = [&](qint64 maxBytes, const QByteArray &sha256) {
            DownloadRequest request;
            request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/made.traineddata").arg(server.serverPort()));
            request.path = path;
            request.maxBytes = maxBytes;
            request.sha256 = sha256;
            QString error = QStringLiteral("never answered");
            QEventLoop loop;
            transport.download(request, [&](const QString &result) {
                error = result;
                loop.quit();
            });
            loop.exec();
            return error;
        };
        const QByteArray ok = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size())
            + "\r\nConnection: close\r\n\r\n" + body;

        // The wrong file, one too large, or one somewhere else, is never kept.
        server.response = ok;
        QVERIFY(fetch(body.size(), QByteArray(64, 'a')).contains(QStringLiteral("expected")));
        QVERIFY(!QFile::exists(path));
        QVERIFY(fetch(body.size() - 1, hash).contains(QStringLiteral("larger")));
        QVERIFY(!QFile::exists(path));
        server.response = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        QVERIFY(fetch(body.size(), hash).contains(QStringLiteral("somewhere else")));
        QVERIFY(!QFile::exists(path));

        // The right one is.
        server.response = ok;
        QCOMPARE(fetch(body.size(), hash), QString());
        QFile kept(path);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), body);

        // Only securely, from anywhere but here.
        DownloadRequest plain;
        plain.url = QUrl(QStringLiteral("http://example.org/made.traineddata"));
        plain.path = path;
        QString refused;
        transport.download(plain, [&](const QString &error) { refused = error; });
        QVERIFY(refused.contains(QStringLiteral("securely")));
    }

    void languagesComeFromOnePlaceAndAreChecked()
    {
        QVERIFY(ocrLanguages().size() > 20);
        for (const OcrLanguage &language : ocrLanguages()) {
            const QUrl url = ocrLanguageUrl(language);
            QCOMPARE(url.scheme(), QStringLiteral("https"));
            QCOMPARE(url.host(), QStringLiteral("raw.githubusercontent.com"));
            QVERIFY(url.path().startsWith(QStringLiteral("/tesseract-ocr/tessdata_fast/4.1.0/")));
            QCOMPARE(QByteArray(language.sha256).size(), 64);
            QVERIFY(language.bytes > 100000 && language.bytes < 20000000);
        }
        // The English that comes with Papertrail is the one listed.
        const OcrLanguage *english = ocrLanguage(QStringLiteral("eng"));
        QVERIFY(english);
        QFile bundled(QStringLiteral(PAPERTRAIL_SOURCE_TESSDATA "/eng.traineddata"));
        QVERIFY(bundled.open(QIODevice::ReadOnly));
        const QByteArray data = bundled.readAll();
        QCOMPARE(data.size(), english->bytes);
        QCOMPARE(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(), QByteArray(english->sha256));
        QVERIFY(!ocrLanguage(QStringLiteral("../eng")));
    }

    void aLanguageIsntFetchedWhileOffline()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        openWindow(&server);
        // No choice made yet, which is offline: it says so, and nothing is asked for.
        bool ready = true;
        m_window->getOcrLanguage(QStringLiteral("fra"), [&](bool result) { ready = result; });
        QVERIFY(!ready);
        QTRY_VERIFY_WITH_TIMEOUT(!m_unexpectedBoxes.isEmpty(), kWaitMs);
        QVERIFY(m_unexpectedBoxes.first().contains(QStringLiteral("offline")));
        m_unexpectedBoxes.clear();
        QCOMPARE(server.connections.load(), 0);
    }

    void theRealTransportHandlesAnswers()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/works/x").arg(server.serverPort()));
        NetworkTransport transport;

        const auto fetch = [&] {
            TransportReply result;
            QEventLoop loop;
            transport.get(url, [&](const TransportReply &reply) {
                result = reply;
                loop.quit();
            });
            loop.exec();
            return result;
        };

        server.response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Rate-Limit-Limit: 3\r\nConnection: close\r\n\r\nhi";
        TransportReply reply = fetch();
        QCOMPARE(reply.status, 200);
        QCOMPARE(reply.body, QByteArray("hi"));
        QCOMPARE(reply.rateLimitPerSecond, 3);
        QVERIFY(reply.error.isEmpty());
        QVERIFY(server.requests.last().contains(QStringLiteral("Papertrail")));

        server.response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        reply = fetch();
        QCOMPARE(reply.status, 404);
        QVERIFY(reply.error.isEmpty());

        server.response = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 30\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        reply = fetch();
        QCOMPARE(reply.status, 429);
        QCOMPARE(reply.retryAfterSeconds, 30);

        // Nothing is listening here any more, which is a failure of the network and not an answer.
        server.close();
        reply = fetch();
        QCOMPARE(reply.status, 0);
        QVERIFY(!reply.error.isEmpty());
    }

    void anAnswerTooLargeIsRefused()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.response = "HTTP/1.1 200 OK\r\nContent-Length: 20000000\r\nConnection: close\r\n\r\n" + QByteArray(20000000, 'x');
        NetworkTransport transport;
        TransportReply result;
        QEventLoop loop;
        transport.get(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())), [&](const TransportReply &reply) {
            result = reply;
            loop.quit();
        });
        QTimer::singleShot(kWaitMs, &loop, &QEventLoop::quit);
        loop.exec();
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.body.isEmpty());
    }

    // ---- Importing ----

    void importingOnlyQueuesLookupsIfAskedTo()
    {
        const QString folder = docsFolder({QStringLiteral("article.pdf")});
        FileRepository files(*m_db);
        const qint64 root = files.addRoot(QStringLiteral("Docs"), folder);

        QString mode;
        import::ImportOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        options.lookupMode = [&mode] { return mode; };
        import::ImportService service(*m_db, options);
        service.scan(root);
        service.processQueue();
        QCOMPARE(m_lookups->pendingCount(), 0);

        mode = QStringLiteral("online_review");
        QVERIFY(QFile::copy(sample(QStringLiteral("thesis.pdf")), QDir(folder).filePath(QStringLiteral("thesis.pdf"))));
        service.scan(root);
        service.processQueue();
        QCOMPARE(m_lookups->pendingCount(), 1);

        // A waiting lookup isn't the import's to deal with.
        QCOMPARE(service.pendingJobs(), 0);
        service.processQueue();
        QCOMPARE(m_lookups->pendingCount(), 1);
    }

    // ---- The window, and staying offline ----

    void nothingIsSentBeforeAChoiceHasBeenMade()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        openWindow(&server);
        QCOMPARE(m_window->lookupMode(), QStringLiteral("offline"));

        m_window->addFolder(docsFolder({QStringLiteral("article.pdf"), QStringLiteral("book.epub"), QStringLiteral("xmp_article.pdf")}));
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), 3, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kWaitMs);
        QTest::qWait(500);

        QCOMPARE(server.connections.load(), 0);
        QCOMPARE(LookupRepository(m_session->database()).pendingCount(), 0);

        // The same, if it's asked for by hand: it can't start without a choice.
        m_window->lookUp(m_session->query().ids({}, {}));
        QTest::qWait(300);
        QCOMPARE(server.connections.load(), 0);
    }

    void offlineModeMakesNoRequestsAndOnlineDoes()
    {
        // The same steps, real network code and all. The second half is what shows the first means something.
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        openWindow(&server);
        m_window->setLookupMode(QStringLiteral("offline"), QString());

        m_window->addFolder(docsFolder({QStringLiteral("article.pdf")}));
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), 1, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kWaitMs);
        m_window->lookUp(m_session->query().ids({}, {}));
        QTest::qWait(500);
        QCOMPARE(server.connections.load(), 0);

        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_window->lookUp(m_session->query().ids({}, {}));
        QTRY_VERIFY_WITH_TIMEOUT(server.connections.load() > 0, kWaitMs);
    }

    void noticesAreOnlyCheckedOnlineAndWhenWanted()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        openWindow(&server);
        addSessionPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"));

        // No choice made yet, which is offline.
        m_window->lookupThread()->checkNotices();
        QTest::qWait(400);
        QCOMPARE(server.connections.load(), 0);

        // Online, but not wanted.
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_session->settings().setValue(QStringLiteral("metadata.check_notices"), false);
        m_window->lookupThread()->checkNotices();
        QTest::qWait(400);
        QCOMPARE(server.connections.load(), 0);

        // Both: it asks, with the real network code, about the DOI and nothing else.
        m_session->settings().setValue(QStringLiteral("metadata.check_notices"), true);
        m_window->lookupThread()->checkNotices();
        QTRY_VERIFY_WITH_TIMEOUT(server.connections.load() > 0, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(), kWaitMs);
        QVERIFY2(server.requests.first().contains("updates:10.1000/made.up"), qPrintable(server.requests.first()));
        QVERIFY(!server.requests.first().contains("Made"));
    }

    void relatedPapersAreOnlyAskedForOnline()
    {
        LocalServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        openWindow(&server);
        const qint64 id = addSessionPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"));

        // No choice made yet, which is offline: the window says so and nothing is sent.
        RelatedPapersDialog *dialog = m_window->showRelatedPapers(id);
        QVERIFY(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->status()->text().contains(QStringLiteral("offline")), kWaitMs);
        QTest::qWait(300);
        QCOMPARE(server.connections.load(), 0);

        // Online, it asks with the real network code, about the DOI and nothing else.
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        dialog->refreshButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(), kWaitMs);
        QVERIFY2(server.requests.first().contains("doi:10.1000/made.up"), qPrintable(server.requests.first()));
        QVERIFY(!server.requests.first().contains("Made"));
        // The server knows nothing, which it says.
        QTRY_VERIFY_WITH_TIMEOUT(dialog->status()->text().contains(QStringLiteral("doesn't know")), kWaitMs);
        dialog->close();
    }

    void aRetractionIsShownWithItsPublication()
    {
        openFakeWindow();
        const qint64 id = addSessionPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"));
        const qint64 other = addSessionPublication(QStringLiteral("Another Made Up Study"), QStringLiteral("10.1000/other"));
        Notice retraction{QStringLiteral("retraction"), QStringLiteral("Retraction"), QStringLiteral("10.1000/notice"),
                          QStringLiteral("2022-01-02"), QStringLiteral("publisher")};
        Notice correction{QStringLiteral("correction"), QStringLiteral("Correction"), QStringLiteral("10.1000/fix"),
                          QString(), QString()};
        NoticeRepository(m_session->database()).record({id, other}, {{id, {retraction}}, {other, {correction}}}, 1000);

        m_window->details()->showPublication(id);
        QVERIFY(m_window->details()->noticeWarning()->isVisibleTo(m_window->details()));
        QCOMPARE(m_window->details()->noticeWarning()->text(), QStringLiteral("This was retracted in 2022."));
        QCOMPARE(m_window->details()->noticeWarning()->textFormat(), Qt::PlainText);

        // In the library, its cover has a red bar along the bottom saying so, and the corrected one's doesn't.
        for (const PublicationSummary &row : m_session->query().page({}, {}, 0, 10))
            QCOMPARE(row.coverNotice, row.id == id ? QStringLiteral("retraction") : QString());
        m_window->refreshNow();
        PublicationListModel *model = m_window->model();
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 2, kWaitMs);
        for (int row = 0; row < 2; ++row)
            QTRY_VERIFY_WITH_TIMEOUT(model->idAt(row).has_value(), kWaitMs);
        const int retractedRow = *model->idAt(0) == id ? 0 : 1;
        QCOMPARE(model->index(retractedRow, 0).data(PublicationListModel::CoverNoticeRole).toString(), QStringLiteral("retraction"));
        QVERIFY(model->index(retractedRow, 0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("Retracted")));
        const auto redRowsInCard = [&](int row) {
            QStyleOptionViewItem option;
            option.initFrom(m_window->gridView());
            option.widget = m_window->gridView();
            option.rect = QRect(QPoint(0, 0), GridDelegate::cardSize());
            QImage image(option.rect.size(), QImage::Format_ARGB32);
            image.fill(Qt::white);
            {
                QPainter painter(&image);
                m_window->gridView()->itemDelegate()->paint(&painter, option, model->index(row, 0));
            }
            // Rows with red across the middle, and the lowest of them.
            int count = 0;
            int lowest = -1;
            for (int y = 0; y < image.height(); ++y) {
                const QColor colour = image.pixelColor(image.width() / 2 - 20, y);
                if (colour.red() > 150 && colour.green() < 80 && colour.blue() < 80) {
                    ++count;
                    lowest = y;
                }
            }
            return qMakePair(count, lowest);
        };
        const auto retracted = redRowsInCard(retractedRow);
        QVERIFY2(retracted.first >= 10, qPrintable(QString::number(retracted.first)));
        // At the bottom of the front page, not the top: the title and authors are below it.
        QVERIFY(retracted.second > GridDelegate::cardSize().height() / 2);
        QCOMPARE(redRowsInCard(1 - retractedRow).first, 0);
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty()) {
            m_window->resize(1200, 700);
            m_window->show();
            m_window->details()->showPublication(id);
            QTest::qWait(500);
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("retracted.png")));
        }

        // A small cover gets the bar without the words, which wouldn't fit.
        QImage small(30, 40, QImage::Format_ARGB32);
        small.fill(Qt::white);
        int height = 0;
        {
            QPainter painter(&small);
            height = drawNoticeBar(&painter, small.rect(), QStringLiteral("Retracted"));
        }
        QVERIFY(height >= 4 && height < 20);
        QCOMPARE(small.pixelColor(15, 39).red(), 0xc6);
        QCOMPARE(small.pixelColor(15, 0), QColor(Qt::white));

        // A correction is listed with the details, but isn't warned about.
        m_window->details()->showPublication(other);
        QVERIFY(!m_window->details()->noticeWarning()->isVisibleTo(m_window->details()));
        bool listed = false;
        for (const QLabel *label : m_window->details()->findChildren<QLabel *>())
            listed = listed || label->text().contains(QStringLiteral("10.1000/fix"));
        QVERIFY(listed);
    }

    void noticesKeepWhenTheyWereFirstFound()
    {
        const qint64 one = addPublication(QStringLiteral("Made Up Study One"), QStringLiteral("10.1000/one"), false);
        const qint64 two = addPublication(QStringLiteral("Made Up Study Two"), QStringLiteral("10.1000/two"), false);
        const Notice concern{QStringLiteral("expression_of_concern"), QStringLiteral("Expression of concern"),
                             QStringLiteral("10.1000/n1"), QStringLiteral("2021"), QStringLiteral("publisher")};
        const Notice correction{QStringLiteral("correction"), QStringLiteral("Correction"), QStringLiteral("10.1000/n2"),
                                QString(), QString()};
        NoticeRepository notices(*m_db);
        QCOMPARE(notices.newestFoundAt(), qint64(0));
        notices.record({one, two}, {{one, {concern}}, {two, {correction}}}, 1000);

        // Only the concern is worth warning about.
        QList<NewWarning> unseen = notices.unseenWarnings(500);
        QCOMPARE(unseen.size(), 1);
        QCOMPARE(unseen.first().publicationId, one);
        QCOMPARE(unseen.first().title, QStringLiteral("Made Up Study One"));
        QCOMPARE(unseen.first().notice.kind, QStringLiteral("expression_of_concern"));
        QCOMPARE(unseen.first().foundAt, qint64(1000));

        // Found again, it's still from when it was first found, so nothing's new.
        notices.record({one, two}, {{one, {concern}}, {two, {correction}}}, 2000);
        QVERIFY(notices.unseenWarnings(1000).isEmpty());
        QCOMPARE(notices.forPublication(one).size(), 1);

        // A concern that becomes a retraction is new.
        const Notice retraction{QStringLiteral("retraction"), QStringLiteral("Retraction"), QStringLiteral("10.1000/n3"),
                                QStringLiteral("2022-05"), QStringLiteral("publisher")};
        notices.record({one}, {{one, {concern, retraction}}}, 3000);
        unseen = notices.unseenWarnings(1000);
        QCOMPARE(unseen.size(), 1); // Listed once, with its newest.
        QCOMPARE(unseen.first().notice.kind, QStringLiteral("retraction"));
        QCOMPARE(unseen.first().notice.date, QStringLiteral("2022-05"));
        QCOMPARE(unseen.first().foundAt, qint64(3000));
        QCOMPARE(notices.newestFoundAt(), qint64(3000));

        // One that's gone when checked again is gone here too.
        notices.record({one}, {{one, {retraction}}}, 4000);
        QCOMPARE(notices.forPublication(one).size(), 1);
        QCOMPARE(notices.forPublication(one).first().kind, QStringLiteral("retraction"));

        // Nothing in a removed folder is warned about.
        m_db->exec(QStringLiteral("INSERT INTO hidden_publication (publication_id) VALUES (%1)").arg(one).toUtf8().constData());
        QVERIFY(notices.unseenWarnings(0).isEmpty());
    }

    void newRetractionsAreCountedAndOffered()
    {
        openFakeWindow();
        const qint64 id = addSessionPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"));
        SettingsStore &settings = m_session->settings();
        const QString key = QStringLiteral("notices.seen_until");
        // An empty library has nothing it's already been told about.
        QTRY_VERIFY_WITH_TIMEOUT(settings.value(key).toDouble() > 0, kWaitMs);
        QPushButton *status = m_window->retractionStatusButton();
        QVERIFY(!status->isVisibleTo(m_window.get()));

        const qint64 found = qint64(settings.value(key).toDouble()) + 1000;
        const Notice retraction{QStringLiteral("retraction"), QStringLiteral("Retraction"), QStringLiteral("10.1000/notice"),
                                QStringLiteral("2022-01-02"), QStringLiteral("publisher")};
        NoticeRepository(m_session->database()).record({id}, {{id, {retraction}}}, found);
        m_window->updateRetractionWarning();
        QVERIFY(status->isVisibleTo(m_window.get()));
        QCOMPARE(status->text(), QStringLiteral("1 new retraction or concern"));

        // It's offered without taking the keyboard, naming the publication as plain text.
        QTRY_VERIFY_WITH_TIMEOUT(m_window->retractionOffer() != nullptr, kWaitMs);
        QMessageBox *offer = m_window->retractionOffer();
        QCOMPARE(offer->textFormat(), Qt::PlainText);
        QVERIFY(offer->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY2(offer->informativeText().contains(QStringLiteral("Retracted in 2022: Made Up Study")),
                 qPrintable(offer->informativeText()));
        // Only once: asking again doesn't bring up another.
        offer->findChild<QPushButton *>(QStringLiteral("retractionsLater"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->retractionOffer() == nullptr, kWaitMs);
        m_window->updateRetractionWarning();
        QVERIFY(m_window->retractionOffer() == nullptr);
        QVERIFY(status->isVisibleTo(m_window.get())); // Still counted until it's looked at.

        // Counted across restarts, but not offered again.
        m_window.reset();
        m_session.reset();
        openFakeWindow();
        status = m_window->retractionStatusButton();
        QTRY_VERIFY_WITH_TIMEOUT(status->isVisibleTo(m_window.get()), kWaitMs);
        QTest::qWait(200);
        QVERIFY(m_window->retractionOffer() == nullptr);

        // Showing them chooses the filter, and counts them as seen.
        status->click();
        QCOMPARE(qint64(m_session->settings().value(key).toDouble()), found);
        QVERIFY(!status->isVisibleTo(m_window.get()));
        PublicationFilter filter;
        m_window->sidebar()->applyTo(filter);
        QVERIFY(filter.retracted);
        QCOMPARE(m_window->documentTabs()->currentIndex(), 0);
    }

    void goingOfflineDropsWhatsWaiting()
    {
        LocalServer server; // It never answers usefully, so jobs are still queued or in progress.
        QVERIFY(server.listen(QHostAddress::LocalHost));
        openWindow(&server);
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        QList<qint64> ids;
        for (int i = 0; i < 30; ++i)
            ids << addSessionPublication(QStringLiteral("Title Number %1 Here").arg(i), QStringLiteral("10.1000/%1").arg(i));
        m_window->lookUp(ids);
        QTRY_VERIFY_WITH_TIMEOUT(server.connections.load() > 0, kWaitMs);

        m_window->setLookupMode(QStringLiteral("offline"), QString());
        QCOMPARE(LookupRepository(m_session->database()).pendingCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->lookupThread()->isBusy(), kWaitMs);
        const int connections = server.connections.load();
        QTest::qWait(600);
        QCOMPARE(server.connections.load(), connections);
    }

    void aChoiceForOneFolderOnlyLastsForThatFolder()
    {
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());

        // Online by default, but this one folder stays offline.
        const QString folder = docsFolder({QStringLiteral("article.pdf")});
        m_window->addFolder(folder, QStringLiteral("offline"));
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), 1, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kWaitMs);
        QTest::qWait(300);
        QCOMPARE(m_fake->count(), 0);

        // Back to the default after it.
        QVERIFY(QFile::copy(sample(QStringLiteral("xmp_article.pdf")), QDir(folder).filePath(QStringLiteral("xmp_article.pdf"))));
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), 2, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->count() > 0, kWaitMs);
    }

    void aChoiceToLookUpOneFolderWorksWhileOffline()
    {
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("offline"), QString());
        m_window->addFolder(docsFolder({QStringLiteral("xmp_article.pdf")}), QStringLiteral("online_review"));
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->count() > 0, kWaitMs);
    }

    void theLookupChoiceGivesBackWhatWasChosen()
    {
        OnlineLookupDialog dialog(QStringLiteral("Heading"), QStringLiteral("offline"), QString(), true);
        QCOMPARE(dialog.mode(), QStringLiteral("offline"));
        QCOMPARE(dialog.email(), QString());
        for (const QString &mode : {QStringLiteral("online_review"), QStringLiteral("online_auto"), QStringLiteral("offline")}) {
            dialog.setMode(mode);
            QCOMPARE(dialog.mode(), mode);
        }
    }

    void reviewingAMatchFromTheWindow()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        const qint64 keep = addSessionPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/example.2020.7"));
        const qint64 drop = addSessionPublication(QStringLiteral("Growing Some Herbs"), QStringLiteral("10.1000/example.2020.8"));
        m_window->lookUp({keep, drop});

        QTRY_COMPARE_WITH_TIMEOUT(LookupRepository(m_session->database()).waitingForReviewCount(), 2, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewButton()->isVisibleTo(m_window.get()), kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(m_window->reviewButton()->text(), QStringLiteral("To review: 2"), kWaitMs);

        m_window->showReviewTab();
        ReviewTab *review = m_window->reviewTab();
        QVERIFY(review != nullptr);
        QCOMPARE(review->list()->count(), 2);
        QCOMPARE(review->currentPublication(), keep);

        // Both versions are there, side by side.
        QTableWidget *table = review->comparison();
        QVERIFY(table->rowCount() > 3);
        bool sawTitle = false;
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 0)->text() == QStringLiteral("Title")) {
                sawTitle = true;
                QCOMPARE(table->item(row, 1)->text(), QStringLiteral("Cooking with Herbs"));
                QCOMPARE(table->item(row, 2)->text(), QStringLiteral("Fatigue of Steel Beams & Columns"));
            }
        }
        QVERIFY(sawTitle);
        QCOMPARE(table->item(1, 0)->text(), QStringLiteral("Title"));
        QVERIFY2(table->item(3, 0)->text().startsWith(QStringLiteral("Author")), qPrintable(table->item(3, 0)->text()));
        // A short title fits on one line, once the table has its real width.
        QTRY_VERIFY_WITH_TIMEOUT(table->rowHeight(1) < table->fontMetrics().height() * 2 + 12, kWaitMs);

        // For looking at by eye, if asked for.
        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty())
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("review.png")));

        review->acceptCurrent();
        QCOMPARE(m_session->publications().get(keep)->title, QStringLiteral("Fatigue of Steel Beams & Columns"));
        QCOMPARE(review->list()->count(), 1);
        QCOMPARE(m_window->reviewButton()->text(), QStringLiteral("To review: 1"));

        review->rejectCurrent();
        QCOMPARE(m_session->publications().get(drop)->title, QStringLiteral("Growing Some Herbs"));
        QCOMPARE(review->list()->count(), 0);
        QVERIFY(!m_window->reviewButton()->isVisibleTo(m_window.get()));
    }

    void aPopUpOffersTheReviewWhenLookupsFinish()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_window->lookUp({addSessionPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/a")),
                          addSessionPublication(QStringLiteral("Growing Some Herbs"), QStringLiteral("10.1000/b"))});

        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() != nullptr, kWaitMs);
        QMessageBox *offer = m_window->reviewOffer();
        QVERIFY(offer->text().contains(QStringLiteral("2 publications")));
        QCOMPARE(offer->textFormat(), Qt::PlainText);
        QVERIFY(!offer->isModal()); // The library can still be used with it up.
        QCOMPARE(m_window->documentTabs()->currentIndex(), 0);

        offer->findChild<QPushButton *>(QStringLiteral("reviewNow"))->click();
        QVERIFY(m_window->documentTabs()->currentWidget() == m_window->reviewTab());
        QCOMPARE(m_window->reviewTab()->list()->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() == nullptr, kWaitMs);
    }

    void theWindowsOpenAlexKeyReachesItsLookups()
    {
        m_fake->answer(QStringLiteral("api.openalex.org"), fixture("openalex_work.json"));
        prepareSession(ProviderUrls{}, m_fake, QStringLiteral("windowKey42"));
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_window->lookUp({addSessionPublication(QStringLiteral("Made Up Study"), QStringLiteral("10.1000/made.up"))});
        QTRY_VERIFY_WITH_TIMEOUT(m_fake->count() >= 2 && !m_window->lookupThread()->isBusy(), kWaitMs);
        QMutexLocker lock(&m_fake->mutex);
        bool keyed = false;
        for (qsizetype i = 0; i < m_fake->asked.size(); ++i) {
            QVERIFY(!m_fake->asked.at(i).contains(QStringLiteral("windowKey42")));
            const bool openAlex = m_fake->asked.at(i).contains(QStringLiteral("openalex"));
            QCOMPARE(m_fake->headers.at(i).contains("Bearer windowKey42"), openAlex);
            keyed = keyed || openAlex;
        }
        QVERIFY(keyed);
    }

    void choosingLaterLeavesItAloneUntilThereIsMore()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_window->lookUp({addSessionPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/a"))});
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() != nullptr, kWaitMs);
        QVERIFY(m_window->reviewOffer()->text().contains(QStringLiteral("1 publication")));

        m_window->reviewOffer()->findChild<QPushButton *>(QStringLiteral("reviewLater"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() == nullptr, kWaitMs);
        QCOMPARE(m_window->documentTabs()->currentIndex(), 0);
        QVERIFY(m_window->reviewTab() == nullptr);
        QCOMPARE(m_window->reviewButton()->text(), QStringLiteral("To review: 1"));

        // A lookup that adds nothing new doesn't bring it up again.
        m_window->lookUp({addSessionPublication(QStringLiteral("Nothing Known About This"), QString())});
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->lookupThread()->isBusy() && m_fake->count() >= 3, kWaitMs);
        QTest::qWait(300);
        QVERIFY(m_window->reviewOffer() == nullptr);

        // One that does, does.
        m_window->lookUp({addSessionPublication(QStringLiteral("Growing Some Herbs"), QStringLiteral("10.1000/b"))});
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() != nullptr, kWaitMs);
        QVERIFY(m_window->reviewOffer()->text().contains(QStringLiteral("2 publications")));
    }

    void thePopUpWaitsForAnImportToFinish()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        // Its DOI will bring back a record with another title, so that waits for review.
        m_window->addFolder(docsFolder({QStringLiteral("xmp_article.pdf"), QStringLiteral("article.pdf")}));

        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() != nullptr, kWaitMs);
        QVERIFY(!m_window->importThread()->isBusy());
        QVERIFY(!m_window->lookupThread()->isBusy());
        QCOMPARE(LookupRepository(m_session->database()).pendingCount(), 0);
    }

    void thePopUpDoesntInterruptReading()
    {
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("offline"), QString());
        m_window->addFolder(docsFolder({QStringLiteral("article.pdf")}));
        QTRY_COMPARE_WITH_TIMEOUT(m_session->query().count({}), 1, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kWaitMs);
        QVERIFY(m_window->openPublication(m_session->query().ids({}, {}).first()) != nullptr);
        QVERIFY(m_window->documentTabs()->currentIndex() != 0);

        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_window->lookUp({addSessionPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/a"))});
        QTRY_COMPARE_WITH_TIMEOUT(LookupRepository(m_session->database()).waitingForReviewCount(), qint64(1), kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->lookupThread()->isBusy(), kWaitMs);
        QTest::qWait(300);
        QVERIFY(m_window->reviewOffer() == nullptr);

        m_window->documentTabs()->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() != nullptr, kWaitMs);
    }

    void thereIsNoPopUpWhenNothingNeedsChecking()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        const qint64 id = addSessionPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1000/example.2020.7"));
        m_window->lookUp({id});
        QTRY_VERIFY_WITH_TIMEOUT(!m_session->publications().get(id)->authors.isEmpty(), kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->lookupThread()->isBusy(), kWaitMs);
        QTest::qWait(300);
        QVERIFY(m_window->reviewOffer() == nullptr);
    }

    void thereIsNoPopUpWhenLookupsAreStopped()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        QList<qint64> ids;
        for (int i = 0; i < 30; ++i)
            ids << addSessionPublication(QStringLiteral("Cooking with Herbs %1").arg(i), QStringLiteral("10.1000/%1").arg(i));
        m_window->lookUp(ids);
        QTRY_VERIFY_WITH_TIMEOUT(LookupRepository(m_session->database()).waitingForReviewCount() > 0, kWaitMs);
        m_window->lookupThread()->stopLookups();
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->lookupThread()->isBusy(), kWaitMs);
        QVERIFY(LookupRepository(m_session->database()).pendingCount() > 0);
        QTest::qWait(300);
        QVERIFY(m_window->reviewOffer() == nullptr);
    }

    void whatWasLeftForLaterIsntBroughtUpOnOpening()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_work.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        m_window->lookUp({addSessionPublication(QStringLiteral("Cooking with Herbs"), QStringLiteral("10.1000/a"))});
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewOffer() != nullptr, kWaitMs);

        m_window.reset();
        m_session.reset();
        openFakeWindow();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->reviewButton()->isVisibleTo(m_window.get()), kWaitMs);
        QTest::qWait(300);
        QVERIFY(m_window->reviewOffer() == nullptr);
    }

    void theDetailsOfferUndoAndAPreprintHint()
    {
        m_fake->answer(QStringLiteral("api.crossref.org/works/"), fixture("crossref_preprint.json"));
        openFakeWindow();
        m_window->setLookupMode(QStringLiteral("online_review"), QString());
        const qint64 id = addSessionPublication(QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("10.1101/2020.01.01.123456"));
        m_window->details()->showPublication(id);
        QVERIFY(!m_window->details()->undoRow()->isVisibleTo(m_window->details()));
        m_window->lookUp({id});
        QTRY_VERIFY_WITH_TIMEOUT(!m_session->publications().get(id)->authors.isEmpty(), kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->details()->undoRow()->isVisibleTo(m_window->details()), kWaitMs);

        // Mentioned, and not acted on. Marking something a preprint is the user's to do.
        QVERIFY(m_window->details()->preprintHint()->isVisibleTo(m_window->details()));
        QCOMPARE(m_window->details()->preprintHint()->textFormat(), Qt::PlainText);
        QVERIFY(!m_window->details()->preprintBox()->isChecked());
        QVERIFY(m_session->publications().get(id)->type != PublicationType::Preprint);

        m_window->details()->undoLookup();
        QVERIFY(m_session->publications().get(id)->authors.isEmpty());
        QVERIFY(!m_window->details()->undoRow()->isVisibleTo(m_window->details()));
        QVERIFY(!m_window->details()->preprintHint()->isVisibleTo(m_window->details()));
    }

private:
    qint64 addPublication(const QString &title, const QString &doi, bool needsReview)
    {
        Publication p;
        p.title = title;
        p.doi = doi;
        p.needsReview = needsReview;
        return m_publications->create(p);
    }

    void addAuthor(qint64 id, const QString &family)
    {
        Publication p = *m_publications->get(id);
        p.authors = {{family, QString()}};
        m_publications->update(p);
    }

    qint64 addSessionPublication(const QString &title, const QString &doi)
    {
        Publication p;
        p.title = title;
        p.doi = doi;
        return m_session->publications().create(p);
    }

    LookupSummary run()
    {
        QSignalSpy finished(m_engine.get(), &LookupEngine::finished);
        m_engine->start();
        if (!finished.wait(kWaitMs))
            qWarning("the lookups didn't finish");
        return finished.isEmpty() ? LookupSummary{} : finished.first().at(0).value<LookupSummary>();
    }

    QList<qint64> crossrefTimes() const
    {
        QList<qint64> times;
        for (qsizetype i = 0; i < m_fake->asked.size(); ++i) {
            if (m_fake->asked.at(i).contains(QStringLiteral("api.crossref.org")))
                times << m_fake->times.at(i);
        }
        return times;
    }

    QString docsFolder(const QStringList &names)
    {
        const QString folder = QDir::cleanPath(m_dir->filePath(QStringLiteral("docs")));
        QDir().mkpath(folder);
        for (const QString &name : names)
            QFile::copy(sample(name), QDir(folder).filePath(name));
        return folder;
    }

    void prepareSession(const ProviderUrls &urls, const std::shared_ptr<FakeState> &fake, const QString &openAlexKey = QString())
    {
        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("window.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        paths.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        paths.settleMs = 0;
        paths.providerUrls = urls;
        if (fake)
            paths.transportFactory = [fake] { return std::make_unique<FakeTransport>(fake); };
        // Keys only ever in memory here, never the computer's own credential store.
        paths.keyStoreFactory = [openAlexKey](const QString &entry) {
            auto keys = std::make_unique<MemoryKeyStore>();
            if (entry == kOpenAlexKeyEntry && !openAlexKey.isEmpty())
                keys->write(openAlexKey, [](bool) {});
            return std::unique_ptr<ApiKeyStore>(std::move(keys));
        };
        m_session = std::make_unique<LibrarySession>(paths);
        m_session->settings().setValue(QStringLiteral("library.rescan_on_startup"), false);
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->resize(1200, 800);
        m_window->show();
        // Lets the window finish starting, which notes what was already waiting, before a test adds to it.
        QCoreApplication::processEvents();
    }

    // With the real network code, and every source pointed at a server in this test.
    void openWindow(LocalServer *server)
    {
        ProviderUrls urls;
        const QString here = QStringLiteral("http://127.0.0.1:%1").arg(server->serverPort());
        urls.crossref = urls.openAlex = urls.arxiv = urls.openLibrary = here;
        prepareSession(urls, nullptr);
    }

    void openFakeWindow()
    {
        prepareSession(ProviderUrls{}, m_fake);
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<PublicationRepository> m_publications;
    std::unique_ptr<LookupRepository> m_lookups;
    std::shared_ptr<FakeState> m_fake;
    std::unique_ptr<FakeTransport> m_transport;
    std::unique_ptr<LookupEngine> m_engine;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
};

QTEST_MAIN(MetadataTest)
#include "test_metadata.moc"
