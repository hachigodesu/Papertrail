// Tests of article summaries. Nothing here reaches Anthropic or spends anything: replies are made up here, in the
// API's format, and the one real network path talks to a server inside the test.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/SummaryRepository.h"
#include "core/repo/VolumeArticles.h"
#include "core/settings/SettingsStore.h"
#include "metadata/Transport.h"
#include "summary/SummaryBatches.h"
#include "summary/SummaryEngine.h"
#include "summary/SummaryRequest.h"
#include "summary/SummarySpending.h"
#include "ui/ApiKeyStore.h"
#include "ui/BatchSummaryDialog.h"
#include "ui/DetailsPanel.h"
#include "ui/ImportThread.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/SummariesDialog.h"
#include "ui/SummaryDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::metadata;
using namespace papertrail::summary;
using namespace papertrail::ui;

namespace {

constexpr int kWaitMs = 30000;
const QString kKey = QStringLiteral("sk-test-not-a-real-key-1234567890");

const QString kGoodText = QStringLiteral(
    "BASED ON: full text\nIDENTIFIED: yes\n\n"
    "## Aim and background\nIt looked at fatigue.\n\n"
    "## Methodology\nThey bent some beams.\n\n"
    "## Results\nThe beams broke.\n\n"
    "## Limitations\nOnly a few beams.\n\n"
    "## Conclusion and discussion\nBeams are finite.");

QByteArray replyWith(const QString &text, const QString &stopReason = QStringLiteral("end_turn"), qint64 input = 1000, qint64 output = 500,
                     qint64 searches = 1, const QJsonArray &extraBlocks = {})
{
    QJsonArray content = extraBlocks;
    if (!text.isEmpty())
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}});
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("type"), QStringLiteral("message")},
                             {QStringLiteral("role"), QStringLiteral("assistant")},
                             {QStringLiteral("stop_reason"), stopReason},
                             {QStringLiteral("content"), content},
                             {QStringLiteral("usage"), QJsonObject{{QStringLiteral("input_tokens"), input},
                                                                   {QStringLiteral("output_tokens"), output},
                                                                   {QStringLiteral("server_tool_use"), QJsonObject{{QStringLiteral("web_search_requests"), searches}}}}},
                         })
        .toJson(QJsonDocument::Compact);
}

QByteArray errorReply(const QString &type, const QString &message)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                                     {QStringLiteral("error"), QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("message"), message}}}})
        .toJson(QJsonDocument::Compact);
}

struct FakeState {
    QList<HttpRequest> requests;
    QList<TransportReply> replies; // In order, and the last is repeated.
    QByteArray modelList = "{\"data\":[{\"id\":\"claude-sonnet-5-5\"},{\"id\":\"claude-new-model\"},{\"id\":\"not-claude\"}]}";

    QList<HttpRequest> messages() const
    {
        QList<HttpRequest> found;
        for (const HttpRequest &request : requests) {
            if (request.url.path().endsWith(QLatin1StringView("/v1/messages")))
                found << request;
        }
        return found;
    }

    void reply(const QByteArray &body, int status = 200)
    {
        TransportReply one;
        one.status = status;
        one.body = body;
        replies << one;
    }
};

class FakeTransport : public Transport {
public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : m_state(std::move(state)) {}

    void get(const QUrl &, const Callback &callback) override { callback(TransportReply{}); }

    void send(const HttpRequest &request, const Callback &callback) override
    {
        m_state->requests << request;
        TransportReply reply;
        if (request.url.path().endsWith(QLatin1StringView("/v1/models"))) {
            reply.status = 200;
            reply.body = m_state->modelList;
        } else if (!m_state->replies.isEmpty()) {
            reply = m_state->replies.size() > 1 ? m_state->replies.takeFirst() : m_state->replies.first();
        } else {
            reply.status = 500;
        }
        QTimer::singleShot(0, [callback, reply] { callback(reply); });
    }

private:
    std::shared_ptr<FakeState> m_state;
};

} // namespace

class SummaryTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_docs = QDir::cleanPath(m_dir->filePath(QStringLiteral("docs")));
        QDir().mkpath(m_docs);
        m_fake = std::make_shared<FakeState>();
        m_transport = std::make_unique<FakeTransport>(m_fake);
        m_keys = std::make_unique<MemoryKeyStore>();

        m_unexpectedBoxes.clear();
        m_boxWatcher = std::make_unique<QTimer>();
        connect(m_boxWatcher.get(), &QTimer::timeout, this, [this] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                m_unexpectedBoxes << box->text();
                box->close();
            }
        });
        m_boxWatcher->start(100);

        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("library.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        paths.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        paths.settleMs = 0;
        m_session = std::make_unique<LibrarySession>(paths);
        m_session->settings().setValue(QStringLiteral("library.rescan_on_startup"), false);
        m_session->settings().setValue(QStringLiteral("library.index_contents"), false);
    }

    void cleanup()
    {
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
        m_window.reset();
        m_session.reset();
        m_keys.reset();
        m_transport.reset();
        m_fake.reset();
        m_dir.reset();
    }

    // ---- What's sent ----

    void onlyIdentifyingDetailsAreSent()
    {
        const Publication p = examplePublication();
        const QByteArray body = buildRequest(defaultModel(), defaultEffort(), p, std::nullopt);
        const QJsonObject request = QJsonDocument::fromJson(body).object();

        QCOMPARE(request.value(QStringLiteral("model")).toString(), QStringLiteral("claude-sonnet-5-5"));
        QCOMPARE(request.value(QStringLiteral("output_config")).toObject().value(QStringLiteral("effort")).toString(), QStringLiteral("high"));

        const QString text = QString::fromUtf8(body);
        QVERIFY(text.contains(QStringLiteral("Fatigue of Steel Beams")));
        QVERIFY(text.contains(QStringLiteral("Example, Ada")));
        QVERIFY(text.contains(QStringLiteral("https://doi.org/10.1000/example")));
        QVERIFY(text.contains(QStringLiteral("2020")));
        // Not the user's own notes or abstract, and not a document.
        QVERIFY(!text.contains(QStringLiteral("My private note")));
        QVERIFY(!text.contains(QStringLiteral("\"document\"")));
        QVERIFY(!text.contains(kKey));

        // The web tools are limited, to limit what it can cost.
        const QJsonArray tools = request.value(QStringLiteral("tools")).toArray();
        QCOMPARE(tools.size(), 2);
        QCOMPARE(tools.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("web_search"));
        QVERIFY(tools.at(0).toObject().value(QStringLiteral("max_uses")).toInt() > 0);
        QCOMPARE(tools.at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("web_fetch"));
        QVERIFY(tools.at(1).toObject().value(QStringLiteral("max_content_tokens")).toInt() > 0);
    }

    void identifiersBecomeAddressesThatCanBeFetched()
    {
        Publication p;
        p.title = QStringLiteral("One");
        p.arxivId = QStringLiteral("2101.00001");
        p.url = QStringLiteral("https://example.org/paper");
        bool sawArxiv = false;
        bool sawUrl = false;
        for (const IdentifyingDetail &detail : identifyingDetails(p)) {
            sawArxiv = sawArxiv || detail.value == QStringLiteral("https://arxiv.org/abs/2101.00001");
            sawUrl = sawUrl || detail.value == QStringLiteral("https://example.org/paper");
        }
        QVERIFY(sawArxiv);
        QVERIFY(sawUrl);
    }

    void aPaperGoesInOnlyWhenGivenAndWithoutWebTools()
    {
        PaperContent pdf;
        pdf.pdfBase64 = QByteArray("JVBERi0=");
        const QJsonObject request = QJsonDocument::fromJson(buildRequest(defaultModel(), defaultEffort(), examplePublication(), pdf)).object();
        QVERIFY(!request.contains(QStringLiteral("tools")));
        const QJsonArray content = request.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("content")).toArray();
        const QJsonObject document = content.at(0).toObject();
        QCOMPARE(document.value(QStringLiteral("type")).toString(), QStringLiteral("document"));
        QCOMPARE(document.value(QStringLiteral("source")).toObject().value(QStringLiteral("media_type")).toString(), QStringLiteral("application/pdf"));
        QCOMPARE(document.value(QStringLiteral("source")).toObject().value(QStringLiteral("data")).toString(), QStringLiteral("JVBERi0="));

        PaperContent text;
        text.text = QStringLiteral("the words of the paper");
        const QByteArray withText = buildRequest(defaultModel(), defaultEffort(), examplePublication(), text);
        QVERIFY(QString::fromUtf8(withText).contains(QStringLiteral("the words of the paper")));
        QVERIFY(!QString::fromUtf8(withText).contains(QStringLiteral("\"document\"")));
    }

    void carryingOnKeepsEverythingSoFar()
    {
        const QByteArray request = buildRequest(defaultModel(), defaultEffort(), examplePublication(), std::nullopt);
        QJsonArray soFar;
        soFar.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("server_tool_use")}, {QStringLiteral("id"), QStringLiteral("x")}});
        const QJsonObject next = QJsonDocument::fromJson(buildContinuation(request, soFar)).object();
        const QJsonArray messages = next.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(1).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));
        QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toArray(), soFar);
        QVERIFY(next.contains(QStringLiteral("tools")));
    }

    // ---- What comes back ----

    void readsAReplyAndItsUsage()
    {
        QJsonArray blocks;
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("web_search_tool_result")},
                                  {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("web_search_result")},
                                                                                     {QStringLiteral("url"), QStringLiteral("https://example.org/only-searched")},
                                                                                     {QStringLiteral("title"), QStringLiteral("Just A Search Result")}}}}});
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("web_fetch_tool_result")},
                                  {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("web_fetch_result")},
                                                                          {QStringLiteral("url"), QStringLiteral("https://example.org/read")},
                                                                          {QStringLiteral("content"), QJsonObject{{QStringLiteral("title"), QStringLiteral("  A   Read Page ")}}}}}});
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("web_fetch_tool_result")},
                                  {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("web_fetch_tool_result_error")},
                                                                          {QStringLiteral("error_code"), QStringLiteral("url_not_accessible")}}}});
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), QStringLiteral("Part one. ")},
                                  {QStringLiteral("citations"), QJsonArray{QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.org/cited")}, {QStringLiteral("title"), QStringLiteral("Cited")}},
                                                                           QJsonObject{{QStringLiteral("url"), QStringLiteral("javascript:alert(1)")}, {QStringLiteral("title"), QStringLiteral("Bad")}}}}});

        const ParsedResponse parsed = parseResponse(replyWith(QStringLiteral("Part two."), QStringLiteral("end_turn"), 1234, 56, 2, blocks));
        QVERIFY(parsed.valid);
        QCOMPARE(parsed.text, QStringLiteral("Part one. Part two."));
        QCOMPARE(parsed.usage.inputTokens, qint64(1234));
        QCOMPARE(parsed.usage.outputTokens, qint64(56));
        QCOMPARE(parsed.usage.webSearches, qint64(2));

        // Only what was read or cited counts as a source, and only web addresses.
        QCOMPARE(parsed.sources.size(), 2);
        QCOMPARE(parsed.sources.at(0).url, QStringLiteral("https://example.org/read"));
        QCOMPARE(parsed.sources.at(0).title, QStringLiteral("A Read Page"));
        QCOMPARE(parsed.sources.at(1).url, QStringLiteral("https://example.org/cited"));
    }

    void badRepliesAreJustNoReply_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not JSON") << QByteArray("<html>");
        QTest::newRow("wrong shape") << QByteArray("[1,2]");
        QTest::newRow("no content") << QByteArray("{\"type\":\"message\"}");
        QTest::newRow("an error") << errorReply(QStringLiteral("api_error"), QStringLiteral("Nope"));
    }

    void badRepliesAreJustNoReply()
    {
        QFETCH(QByteArray, body);
        QVERIFY(!parseResponse(body).valid);
    }

    void readsTheSections()
    {
        const SummaryText summary = parseSummaryText(kGoodText);
        QVERIFY(summary.structured);
        QCOMPARE(summary.basis, SummaryBasis::FullText);
        QVERIFY(!summary.identificationUnsure);
        QCOMPARE(summary.aim, QStringLiteral("It looked at fatigue."));
        QCOMPARE(summary.methods, QStringLiteral("They bent some beams."));
        QCOMPARE(summary.results, QStringLiteral("The beams broke."));
        QCOMPARE(summary.limitations, QStringLiteral("Only a few beams."));
        QCOMPARE(summary.conclusion, QStringLiteral("Beams are finite."));
        const QString shown = summaryAsText(summary);
        for (const QString &heading : {QStringLiteral("Aim and background"), QStringLiteral("Methodology"), QStringLiteral("Results"),
                                       QStringLiteral("Limitations"), QStringLiteral("Conclusion and discussion")})
            QVERIFY(shown.contains(heading));
    }

    void sectionsCanBeWrittenInOtherWays()
    {
        const SummaryText summary = parseSummaryText(QStringLiteral("**Based on:** abstract only\n**Identified:** unsure\n\n"
                                                                    "**Aim and background**\nOne.\n\n**Methodology:**\nTwo.\n\n# Results\nThree.\n"));
        QVERIFY(summary.structured);
        QCOMPARE(summary.basis, SummaryBasis::AbstractOnly);
        QVERIFY(summary.identificationUnsure);
        QCOMPARE(summary.aim, QStringLiteral("One."));
        QCOMPARE(summary.results, QStringLiteral("Three."));
        // What it left out says so, rather than being blank.
        QVERIFY(summaryAsText(summary).contains(QStringLiteral("Not available")));
    }

    void aReplyInAnyOtherFormIsKeptWhole()
    {
        const QString odd = QStringLiteral("I couldn't find this paper, sorry.");
        const SummaryText summary = parseSummaryText(odd);
        QVERIFY(!summary.structured);
        QCOMPARE(summary.basis, SummaryBasis::Unknown);
        QCOMPARE(summaryAsText(summary), odd);
    }

    // ---- What it costs ----

    void workingOutWhatItCost()
    {
        Usage usage;
        usage.inputTokens = 1000000;
        usage.outputTokens = 1000000;
        usage.webSearches = 3;
        QCOMPARE(costOf(QStringLiteral("claude-sonnet-5-5"), usage).value(), 2.0 + 10.0 + 0.03);
        QCOMPARE(costOf(QStringLiteral("claude-opus-5-5"), usage).value(), 4.0 + 20.0 + 0.03);
        QVERIFY(!costOf(QStringLiteral("claude-unknown"), usage).has_value());

        // Haiku costs more for a long prompt.
        Usage shortPrompt;
        shortPrompt.inputTokens = 100000;
        QCOMPARE(costOf(QStringLiteral("claude-haiku-5-5"), shortPrompt).value(), 0.01);
        Usage longPrompt;
        longPrompt.inputTokens = 200000;
        QCOMPARE(costOf(QStringLiteral("claude-haiku-5-5"), longPrompt).value(), 0.10);
    }

    void estimatesMoveTheWayTheyShould()
    {
        const Publication p = examplePublication();
        const CostRange sonnet = *estimateFromDetails(QStringLiteral("claude-sonnet-5-5"), QStringLiteral("high"), p);
        QVERIFY(sonnet.low > 0);
        QVERIFY(sonnet.high > sonnet.low);

        const CostRange opus = *estimateFromDetails(QStringLiteral("claude-opus-5-5"), QStringLiteral("high"), p);
        const CostRange haiku = *estimateFromDetails(QStringLiteral("claude-haiku-5-5"), QStringLiteral("high"), p);
        QVERIFY(opus.high > sonnet.high);
        QVERIFY(haiku.high < sonnet.high);

        const CostRange lowEffort = *estimateFromDetails(QStringLiteral("claude-sonnet-5-5"), QStringLiteral("low"), p);
        const CostRange maxEffort = *estimateFromDetails(QStringLiteral("claude-sonnet-5-5"), QStringLiteral("max"), p);
        QVERIFY(lowEffort.high < sonnet.high);
        QVERIFY(maxEffort.high > sonnet.high);
        QVERIFY(!estimateFromDetails(QStringLiteral("claude-unknown"), QStringLiteral("high"), p).has_value());

        const CostRange pdf = *estimateFromPaper(QStringLiteral("claude-sonnet-5-5"), QStringLiteral("high"), 20, true);
        const CostRange text = *estimateFromPaper(QStringLiteral("claude-sonnet-5-5"), QStringLiteral("high"), 20, false);
        const CostRange longer = *estimateFromPaper(QStringLiteral("claude-sonnet-5-5"), QStringLiteral("high"), 200, true);
        QVERIFY(pdf.high > text.high);
        QVERIFY(longer.high > pdf.high);
    }

    void pricesShowSensibly()
    {
        QCOMPARE(formatDollars(1.5), QStringLiteral("$1.50"));
        QCOMPARE(formatDollars(0.004), QStringLiteral("under $0.01"));
        QCOMPARE(formatDollars(0), QStringLiteral("$0.00"));
        QCOMPARE(formatRange({0.02, 0.15}), QStringLiteral("$0.02 to $0.15"));
    }

    // ---- The engine ----

    void aRequestCarriesTheKeyOnlyInItsHeader()
    {
        m_fake->reply(replyWith(kGoodText));
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        engine.setApiKey(kKey);
        QSignalSpy finished(&engine, &SummaryEngine::finished);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        QVERIFY(finished.wait(kWaitMs));

        const HttpRequest request = m_fake->messages().first();
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.url, QUrl(QStringLiteral("https://api.example/v1/messages")));
        QByteArray sent;
        bool sawVersion = false;
        for (const auto &header : request.headers) {
            if (header.first == "x-api-key")
                sent = header.second;
            sawVersion = sawVersion || header.first == "anthropic-version";
        }
        QCOMPARE(QString::fromUtf8(sent), kKey);
        QVERIFY(sawVersion);
        QVERIFY(!request.body.contains(kKey.toUtf8()));
        QVERIFY(!request.url.toString().contains(kKey));

        const SummaryOutcome outcome = finished.first().at(0).value<SummaryOutcome>();
        QVERIFY(outcome.summary.structured);
        QCOMPARE(outcome.usage.inputTokens, qint64(1000));
        QVERIFY(outcome.cost.has_value());
        QVERIFY(!outcome.sentPaper);
        QVERIFY(!outcome.cutOff);
    }

    // ---- Batches ----

    void aBatchIsSentCollectedAndCarriedOn()
    {
        Publication first = examplePublication();
        first.id = m_session->publications().create(first);
        Publication second = examplePublication();
        second.title = QStringLiteral("Corrosion of Made-Up Bridges");
        second.doi = QStringLiteral("10.1000/second");
        second.id = m_session->publications().create(second);

        QJsonArray paused;
        paused.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("server_tool_use")}, {QStringLiteral("id"), QStringLiteral("a")},
                                  {QStringLiteral("name"), QStringLiteral("web_search")}});
        const auto line = [](const QString &customId, const QByteArray &message) {
            return QJsonDocument(QJsonObject{{QStringLiteral("custom_id"), customId},
                                             {QStringLiteral("result"), QJsonObject{{QStringLiteral("type"), QStringLiteral("succeeded")},
                                                                                    {QStringLiteral("message"), QJsonDocument::fromJson(message).object()}}}})
                .toJson(QJsonDocument::Compact);
        };
        const QByteArray batchOne = R"({"id":"msgbatch_01","type":"message_batch","processing_status":"in_progress"})";
        m_fake->reply(batchOne);
        m_fake->reply(batchOne); // Not finished yet when first asked.
        m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"ended"})");
        m_fake->reply(line(QStringLiteral("p%1").arg(first.id), replyWith(kGoodText, QStringLiteral("end_turn"), 1000, 500, 1)) + '\n'
                      + line(QStringLiteral("p%1").arg(second.id), replyWith(QString(), QStringLiteral("pause_turn"), 100, 10, 1, paused)) + '\n');
        m_fake->reply(R"({"id":"msgbatch_02","type":"message_batch","processing_status":"in_progress"})"); // What paused, sent on.
        m_fake->reply(R"({"id":"msgbatch_02","type":"message_batch","processing_status":"ended"})");
        m_fake->reply(R"({"custom_id":"p)" + QByteArray::number(second.id)
                      + R"(-c1","result":{"type":"errored","error":{"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}}}})");

        SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
        batches.setApiKey(kKey);
        QSignalSpy submitted(&batches, &SummaryBatches::submitted);
        QSignalSpy saved(&batches, &SummaryBatches::summariesArrived);
        QSignalSpy failed(&batches, &SummaryBatches::summariesFailed);
        QSignalSpy checked(&batches, &SummaryBatches::checked);
        batches.submit(defaultModel(), defaultEffort(), {first, second});
        QVERIFY(submitted.wait(kWaitMs));
        QCOMPARE(submitted.first().first().toInt(), 2);
        QCOMPARE(batches.waitingCount(), qint64(2));
        QCOMPARE(SummaryBatches::entries(m_session->database()).size(), 2);
        QCOMPARE(SummaryBatches::entryFor(m_session->database(), first.id)->state, BatchEntry::State::Waiting);

        // What was sent: each publication's identifying details under its own id, the key only in a header.
        const HttpRequest sent = m_fake->requests.first();
        QCOMPARE(sent.url, QUrl(QStringLiteral("https://api.example/v1/messages/batches")));
        QCOMPARE(sent.method, QByteArray("POST"));
        const QJsonArray requests = QJsonDocument::fromJson(sent.body).object().value(QStringLiteral("requests")).toArray();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(requests.at(0).toObject().value(QStringLiteral("custom_id")).toString(), QStringLiteral("p%1").arg(first.id));
        QCOMPARE(requests.at(0).toObject().value(QStringLiteral("params")).toObject().value(QStringLiteral("model")).toString(), defaultModel());
        QVERIFY(sent.body.contains("Fatigue of Steel Beams"));
        QVERIFY(!sent.body.contains("My private note"));
        QVERIFY(!sent.body.contains(kKey.toUtf8()));

        // Not finished: nothing changes.
        batches.check();
        QVERIFY(checked.wait(kWaitMs));
        QVERIFY(saved.isEmpty());
        QCOMPARE(batches.waitingCount(), qint64(2));

        // Finished: one arrives to be reviewed, at half the price, and the one that paused goes on in another batch.
        checked.clear();
        batches.check();
        QVERIFY(checked.wait(kWaitMs));
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().first().value<QList<qint64>>(), QList<qint64>{first.id});
        QVERIFY(!SummaryRepository(m_session->database()).forPublication(first.id)); // Not until it's kept.
        const BatchEntry arrived = *SummaryBatches::entryFor(m_session->database(), first.id);
        QCOMPARE(arrived.state, BatchEntry::State::Arrived);
        QVERIFY(arrived.arrivedAt > 0);
        QVERIFY(arrived.text.contains(QStringLiteral("The beams broke.")));
        QCOMPARE(SummaryBatches::arrivedCount(m_session->database()), qint64(1));
        Usage usage;
        usage.inputTokens = 1000;
        usage.outputTokens = 500;
        usage.webSearches = 1;
        QVERIFY(arrived.cost.has_value());
        QVERIFY(qAbs(*arrived.cost - *costOf(defaultModel(), usage) * kBatchPriceFactor) < 1e-9);
        batches.keep(arrived.batchId, arrived.customId);
        const std::optional<StoredSummary> stored = SummaryRepository(m_session->database()).forPublication(first.id);
        QVERIFY(stored);
        QVERIFY(stored->text.contains(QStringLiteral("The beams broke.")));
        QVERIFY(qAbs(stored->costDollars.value_or(0) - *arrived.cost) < 1e-9);
        QCOMPARE(SummaryBatches::arrivedCount(m_session->database()), qint64(0));
        QVERIFY(!SummaryBatches::entryFor(m_session->database(), first.id));
        QVERIFY(m_fake->requests.at(3).url.path().endsWith(QLatin1StringView("/v1/messages/batches/msgbatch_01/results")));
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->requests.size(), 5, kWaitMs);
        const QJsonArray carried = QJsonDocument::fromJson(m_fake->requests.at(4).body).object().value(QStringLiteral("requests")).toArray();
        QCOMPARE(carried.size(), 1);
        QCOMPARE(carried.at(0).toObject().value(QStringLiteral("custom_id")).toString(), QStringLiteral("p%1-c1").arg(second.id));
        const QJsonArray messages = carried.at(0).toObject().value(QStringLiteral("params")).toObject().value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toArray(), paused);
        QTRY_COMPARE_WITH_TIMEOUT(batches.waitingCount(), qint64(1), kWaitMs);
        QCOMPARE(submitted.size(), 1); // Carrying on isn't announced as something new.

        // The carried-on one fails, and says why.
        checked.clear();
        batches.check();
        QVERIFY(checked.wait(kWaitMs));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(0).toInt(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("Overloaded"));
        QCOMPARE(batches.waitingCount(), qint64(0));
        QVERIFY(!SummaryRepository(m_session->database()).forPublication(second.id));

        // The failure stays listed, saying why, until it's taken off the list.
        const BatchEntry failure = *SummaryBatches::entryFor(m_session->database(), second.id);
        QCOMPARE(failure.state, BatchEntry::State::Failed);
        QCOMPARE(failure.problem, QStringLiteral("Overloaded"));
        batches.dismiss(failure.batchId, failure.customId);
        QVERIFY(SummaryBatches::entries(m_session->database()).isEmpty());

        // Each reply that came back is counted at half price, whether or not its summary was kept.
        const QList<SpendEntry> spent = spendEntries(m_session->database());
        QCOMPARE(spent.size(), 2);
        double total = 0;
        for (const SpendEntry &entry : spent) {
            QCOMPARE(entry.kind, SpendKind::Batch);
            total += entry.cost.value_or(0);
        }
        Usage paused2;
        paused2.inputTokens = 100;
        paused2.outputTokens = 10;
        paused2.webSearches = 1;
        const double expected = (*costOf(defaultModel(), usage) + *costOf(defaultModel(), paused2)) * kBatchPriceFactor;
        QVERIFY(qAbs(total - expected) < 1e-9);
        QVERIFY(qAbs(spendTotals(m_session->database(), QDateTime::currentMSecsSinceEpoch()).batch - expected) < 1e-9);
    }

    void nothingIsLostWhenPapertrailClosesPartway()
    {
        Publication one = examplePublication();
        one.id = m_session->publications().create(one);
        Publication two = examplePublication();
        two.title = QStringLiteral("Corrosion of Made-Up Bridges");
        two.id = m_session->publications().create(two);
        QJsonArray paused;
        paused.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("server_tool_use")}, {QStringLiteral("id"), QStringLiteral("a")},
                                  {QStringLiteral("name"), QStringLiteral("web_search")}});
        const auto line = [](const QString &customId, const QByteArray &message) {
            return QJsonDocument(QJsonObject{{QStringLiteral("custom_id"), customId},
                                             {QStringLiteral("result"), QJsonObject{{QStringLiteral("type"), QStringLiteral("succeeded")},
                                                                                    {QStringLiteral("message"), QJsonDocument::fromJson(message).object()}}}})
                .toJson(QJsonDocument::Compact);
        };

        {
            SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
            batches.setApiKey(kKey);
            QSignalSpy submitted(&batches, &SummaryBatches::submitted);
            m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"in_progress"})");
            batches.submit(defaultModel(), defaultEffort(), {one, two});
            QVERIFY(submitted.wait(kWaitMs));
        }
        // Papertrail closed with the batch on its way. Next time, it has finished: one arrives, and the other paused, but
        // Anthropic can't be reached to send it on.
        {
            SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
            batches.setApiKey(kKey);
            QCOMPARE(batches.waitingCount(), qint64(2));
            m_fake->replies.clear();
            m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"ended"})");
            m_fake->reply(line(QStringLiteral("p%1").arg(one.id), replyWith(kGoodText)) + '\n'
                          + line(QStringLiteral("p%1").arg(two.id), replyWith(QString(), QStringLiteral("pause_turn"), 100, 10, 1, paused)) + '\n');
            m_fake->reply(QByteArray(), 503);
            QSignalSpy checked(&batches, &SummaryBatches::checked);
            QSignalSpy problems(&batches, &SummaryBatches::checkProblem);
            batches.check();
            QVERIFY(checked.wait(kWaitMs));
            QCOMPARE(problems.size(), 1);
            QCOMPARE(SummaryBatches::entryFor(m_session->database(), one.id)->state, BatchEntry::State::Arrived);
            QCOMPARE(SummaryBatches::entryFor(m_session->database(), two.id)->state, BatchEntry::State::Paused);
        }
        // And closed again. What arrived still waits to be reviewed, and what paused is sent on from where it was.
        SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
        batches.setApiKey(kKey);
        QCOMPARE(SummaryBatches::arrivedCount(m_session->database()), qint64(1));
        QCOMPARE(batches.waitingCount(), qint64(1));
        m_fake->replies.clear();
        m_fake->reply(R"({"id":"msgbatch_02","type":"message_batch","processing_status":"in_progress"})");
        const qsizetype before = m_fake->requests.size();
        QSignalSpy checked(&batches, &SummaryBatches::checked);
        batches.check();
        QVERIFY(checked.wait(kWaitMs));
        QCOMPARE(m_fake->requests.size(), before + 1);
        const QJsonArray carried = QJsonDocument::fromJson(m_fake->requests.last().body).object().value(QStringLiteral("requests")).toArray();
        QCOMPARE(carried.size(), 1);
        QCOMPARE(carried.at(0).toObject().value(QStringLiteral("custom_id")).toString(), QStringLiteral("p%1-c1").arg(two.id));
        const BatchEntry onItsWay = *SummaryBatches::entryFor(m_session->database(), two.id);
        QCOMPARE(onItsWay.state, BatchEntry::State::Waiting);
        QCOMPARE(onItsWay.batchId, QStringLiteral("msgbatch_02"));
        QCOMPARE(onItsWay.continuations, 1);
        QCOMPARE(SummaryBatches::entryFor(m_session->database(), one.id)->state, BatchEntry::State::Arrived);

        // Discarding one that arrived keeps nothing, but what it cost is still counted.
        const BatchEntry arrived = *SummaryBatches::entryFor(m_session->database(), one.id);
        batches.discard(arrived.batchId, arrived.customId);
        QVERIFY(!SummaryBatches::entryFor(m_session->database(), one.id));
        QVERIFY(!SummaryRepository(m_session->database()).forPublication(one.id));
        QCOMPARE(spendEntries(m_session->database()).size(), 2);
    }

    void aRefusedContinuationFailsRatherThanRepeating()
    {
        Publication one = examplePublication();
        one.id = m_session->publications().create(one);
        m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"ended"})");
        SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
        batches.setApiKey(kKey);
        QSignalSpy submitted(&batches, &SummaryBatches::submitted);
        batches.submit(defaultModel(), defaultEffort(), {one});
        QVERIFY(submitted.wait(kWaitMs));
        m_fake->replies.clear();
        m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"ended"})");
        m_fake->reply(QJsonDocument(QJsonObject{{QStringLiteral("custom_id"), QStringLiteral("p%1").arg(one.id)},
                                                {QStringLiteral("result"), QJsonObject{{QStringLiteral("type"), QStringLiteral("succeeded")},
                                                                                       {QStringLiteral("message"), QJsonDocument::fromJson(replyWith(QString(), QStringLiteral("pause_turn"))).object()}}}})
                          .toJson(QJsonDocument::Compact));
        m_fake->reply(errorReply(QStringLiteral("invalid_request_error"), QStringLiteral("Too long")), 400);
        QSignalSpy checked(&batches, &SummaryBatches::checked);
        QSignalSpy failed(&batches, &SummaryBatches::summariesFailed);
        batches.check();
        QVERIFY(checked.wait(kWaitMs));
        QCOMPARE(failed.size(), 1);
        const BatchEntry entry = *SummaryBatches::entryFor(m_session->database(), one.id);
        QCOMPARE(entry.state, BatchEntry::State::Failed);
        QVERIFY(entry.problem.contains(QStringLiteral("Too long")));
        QCOMPARE(batches.waitingCount(), qint64(0));
    }

    void keepAllKeepsEveryOneAndSeveralFailuresGoAtOnce()
    {
        QList<Publication> sent;
        for (const QString &title : {QStringLiteral("Fatigue of Steel Beams"), QStringLiteral("Corrosion of Made-Up Bridges"),
                                     QStringLiteral("Creep in Imaginary Columns"), QStringLiteral("Wear of Pretend Bearings"),
                                     QStringLiteral("Cracks in Invented Welds")}) {
            Publication p = examplePublication();
            p.title = title;
            p.id = m_session->publications().create(p);
            sent << p;
        }
        SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
        batches.setApiKey(kKey);
        QSignalSpy submitted(&batches, &SummaryBatches::submitted);
        m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"in_progress"})");
        batches.submit(defaultModel(), defaultEffort(), sent);
        QVERIFY(submitted.wait(kWaitMs));

        // Three arrive, and two fail.
        QByteArray results;
        for (int i = 0; i < sent.size(); ++i) {
            const QJsonObject outcome = i < 3
                ? QJsonObject{{QStringLiteral("type"), QStringLiteral("succeeded")},
                              {QStringLiteral("message"), QJsonDocument::fromJson(replyWith(kGoodText)).object()}}
                : QJsonObject{{QStringLiteral("type"), QStringLiteral("expired")}};
            results += QJsonDocument(QJsonObject{{QStringLiteral("custom_id"), QStringLiteral("p%1").arg(sent.at(i).id)},
                                                 {QStringLiteral("result"), outcome}})
                           .toJson(QJsonDocument::Compact)
                + '\n';
        }
        m_fake->replies.clear();
        m_fake->reply(R"({"id":"msgbatch_01","type":"message_batch","processing_status":"ended"})");
        m_fake->reply(results);
        QSignalSpy checked(&batches, &SummaryBatches::checked);
        batches.check();
        QVERIFY(checked.wait(kWaitMs));
        QCOMPARE(SummaryBatches::arrivedCount(m_session->database()), qint64(3));

        SummariesDialog list(*m_session, batches);
        QCOMPARE(list.reviewList()->count(), 3);
        QCOMPARE(list.waitingList()->topLevelItemCount(), 2);
        list.reviewList()->setCurrentRow(1); // Whichever is chosen, all are kept.
        QVERIFY(list.keepAllButton()->isEnabled());
        list.keepAllButton()->click();
        QCOMPARE(list.reviewList()->count(), 0);
        QCOMPARE(SummaryBatches::arrivedCount(m_session->database()), qint64(0));
        for (int i = 0; i < 3; ++i)
            QVERIFY(SummaryRepository(m_session->database()).forPublication(sent.at(i).id).has_value());

        list.waitingList()->selectAll();
        QPushButton *remove = nullptr;
        for (QPushButton *button : list.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Remove from list"))
                remove = button;
        }
        QVERIFY(remove);
        QVERIFY(remove->isEnabled());
        remove->click();
        QCOMPARE(list.waitingList()->topLevelItemCount(), 0);
        QVERIFY(SummaryBatches::entries(m_session->database()).isEmpty());
    }

    void spendingIsAddedUpByWhenAndHow()
    {
        const qint64 id = addExample();
        const QDateTime now = QDateTime::currentDateTime();
        Usage usage;
        usage.inputTokens = 1000000; // A million in.
        const double dollars = *costOf(defaultModel(), usage);
        core::Database &db = m_session->database();
        recordSpend(db, QStringLiteral("instant:a"), id, defaultModel(), SpendKind::Instant, usage, 1.0, now.toMSecsSinceEpoch());
        recordSpend(db, QStringLiteral("instant:a"), id, defaultModel(), SpendKind::Instant, usage, 1.0, now.toMSecsSinceEpoch()); // Again: once.
        recordSpend(db, QStringLiteral("batch:b:p1"), id, defaultModel(), SpendKind::Batch, usage, kBatchPriceFactor, now.toMSecsSinceEpoch());
        recordSpend(db, QStringLiteral("instant:old"), id, defaultModel(), SpendKind::Instant, usage, 1.0, now.addDays(-90).toMSecsSinceEpoch());
        recordSpend(db, QStringLiteral("instant:unknown"), id, QStringLiteral("claude-made-up"), SpendKind::Instant, usage, 1.0,
                    now.toMSecsSinceEpoch());
        recordSpend(db, QStringLiteral("instant:nothing"), id, defaultModel(), SpendKind::Instant, Usage{}, 1.0, now.toMSecsSinceEpoch());

        const SpendTotals totals = spendTotals(db, now.toMSecsSinceEpoch());
        QCOMPARE(totals.requests, 4);
        QCOMPARE(totals.unpriced, 1);
        QVERIFY(qAbs(totals.allTime - dollars * 2.5) < 1e-9);
        QVERIFY(qAbs(totals.last30Days - dollars * 1.5) < 1e-9);
        QVERIFY(qAbs(totals.thisMonth - dollars * 1.5) < 1e-9);
        QVERIFY(qAbs(totals.instant - dollars * 2) < 1e-9);
        QVERIFY(qAbs(totals.batch - dollars * 0.5) < 1e-9);

        // What it was for is still said once the publication has gone.
        m_session->publications().remove(id);
        const QList<SpendEntry> entries = spendEntries(db);
        QCOMPARE(entries.size(), 4);
        QCOMPARE(entries.first().title, QStringLiteral("Fatigue of Steel Beams"));
        QVERIFY(!entries.first().publicationId.has_value());
    }

    void batchesFromBeforeAreKeptAndSpendingStartsFromWhatWasKept()
    {
        // A library from before summaries waited to be reviewed, with one batch on its way and one summary kept.
        const qint64 waiting = addExample();
        Publication other = examplePublication();
        other.title = QStringLiteral("Corrosion of Made-Up Bridges");
        const qint64 kept = m_session->publications().create(other);
        StoredSummary summary;
        summary.publicationId = kept;
        summary.model = defaultModel();
        summary.effort = defaultEffort();
        summary.basis = QStringLiteral("full text");
        summary.text = QStringLiteral("Kept already");
        summary.costDollars = 0.25;
        SummaryRepository(m_session->database()).save(summary);
        core::Database &db = m_session->database();
        db.exec("DELETE FROM summary_spend");
        db.exec("CREATE TABLE summary_batch_item (batch_id TEXT NOT NULL REFERENCES summary_batch(id) ON DELETE CASCADE, "
                "custom_id TEXT NOT NULL, publication_id INTEGER NOT NULL REFERENCES publication(id) ON DELETE CASCADE, "
                "request TEXT NOT NULL, content_so_far TEXT NOT NULL, text_so_far TEXT NOT NULL, sources TEXT NOT NULL, "
                "usage TEXT NOT NULL, continuations INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL CHECK (state IN ('waiting', "
                "'saved', 'failed', 'continued')), problem TEXT, PRIMARY KEY (batch_id, custom_id)) STRICT, WITHOUT ROWID");
        db.exec("INSERT INTO summary_batch (id, model, effort, created_at, status) VALUES ('msgbatch_old', 'claude-sonnet-5-5', 'high', 1000, 'waiting')");
        auto insert = db.prepare("INSERT INTO summary_batch_item (batch_id, custom_id, publication_id, request, content_so_far, text_so_far, "
                                 "sources, usage, state) VALUES ('msgbatch_old', ?, ?, '{}', '[]', '', '[]', '{}', ?)");
        insert->bind(1, QStringLiteral("p%1").arg(waiting));
        insert->bind(2, waiting);
        insert->bind(3, QStringLiteral("waiting"));
        insert->step();
        insert->reset();
        insert->bind(1, QStringLiteral("p%1").arg(kept));
        insert->bind(2, kept);
        insert->bind(3, QStringLiteral("saved"));
        insert->step();
        db.setUserVersion(17);
        core::migrateLibrary(db);

        const BatchEntry entry = *SummaryBatches::entryFor(db, waiting);
        QCOMPARE(entry.state, BatchEntry::State::Waiting);
        QCOMPARE(entry.sentAt, qint64(1000));
        QVERIFY(!SummaryBatches::entryFor(db, kept)); // Kept already, so nothing to do.
        const QList<SpendEntry> spent = spendEntries(db);
        QCOMPARE(spent.size(), 1);
        QCOMPARE(spent.first().kind, SpendKind::Batch);
        QCOMPARE(spent.first().cost.value_or(0), 0.25);

        // Running it again changes nothing. From before the old table was made, as other tests rewind.
        db.setUserVersion(16);
        core::migrateLibrary(db);
        QCOMPARE(SummaryBatches::entries(db).size(), 1);
        QCOMPARE(spendEntries(db).size(), 1);
    }

    void aBatchThatCantBeSentLeavesNothingWaiting()
    {
        Publication one = examplePublication();
        one.id = m_session->publications().create(one);
        m_fake->reply(errorReply(QStringLiteral("authentication_error"), QStringLiteral("invalid x-api-key")), 401);
        SummaryBatches batches(m_session->database(), *m_transport, QStringLiteral("https://api.example"));
        batches.setApiKey(kKey);
        QSignalSpy failed(&batches, &SummaryBatches::submitFailed);
        batches.submit(defaultModel(), defaultEffort(), {one});
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(failed.first().first().value<SummaryFailure>(), SummaryFailure::BadKey);
        QCOMPARE(batches.waitingCount(), qint64(0));

        // An id that isn't one of Anthropic's is never put in an address.
        QVERIFY(!parseBatch(R"({"id":"../other","type":"message_batch","processing_status":"ended"})").valid);
        QVERIFY(parseBatch(R"({"id":"msgbatch_01AbC","type":"message_batch","processing_status":"ended"})").valid);
        QVERIFY(parseBatchResults("not json\n\n").isEmpty());
    }

    void aPausedTurnIsCarriedOnAndAddedUp()
    {
        QJsonArray first;
        first.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("server_tool_use")}, {QStringLiteral("id"), QStringLiteral("a")}, {QStringLiteral("name"), QStringLiteral("web_search")}});
        m_fake->reply(replyWith(QString(), QStringLiteral("pause_turn"), 100, 10, 1, first));
        m_fake->reply(replyWith(kGoodText, QStringLiteral("end_turn"), 200, 20, 2));

        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        engine.setApiKey(kKey);
        QSignalSpy finished(&engine, &SummaryEngine::finished);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        QVERIFY(finished.wait(kWaitMs));

        QCOMPARE(m_fake->messages().size(), 2);
        const QJsonArray messages = QJsonDocument::fromJson(m_fake->messages().at(1).body).object().value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.at(1).toObject().value(QStringLiteral("content")).toArray(), first);

        const SummaryOutcome outcome = finished.first().at(0).value<SummaryOutcome>();
        QCOMPARE(outcome.usage.inputTokens, qint64(300));
        QCOMPARE(outcome.usage.outputTokens, qint64(30));
        QCOMPARE(outcome.usage.webSearches, qint64(3));
    }

    void endlessPausingIsStopped()
    {
        m_fake->reply(replyWith(QString(), QStringLiteral("pause_turn")));
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        engine.setApiKey(kKey);
        QSignalSpy failed(&engine, &SummaryEngine::failed);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        QVERIFY(failed.wait(kWaitMs));
        QVERIFY(m_fake->messages().size() <= 6);
        // What was spent getting there is reported, so it can be shown.
        QVERIFY(failed.first().at(2).value<Usage>().inputTokens > 0);
    }

    void failuresAreToldApart_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("failure");
        QTest::newRow("bad key") << 401 << errorReply(QStringLiteral("authentication_error"), QStringLiteral("invalid x-api-key")) << int(SummaryFailure::BadKey);
        QTest::newRow("forbidden") << 403 << errorReply(QStringLiteral("permission_error"), QStringLiteral("no")) << int(SummaryFailure::BadKey);
        QTest::newRow("no credit") << 400 << errorReply(QStringLiteral("invalid_request_error"), QStringLiteral("Your credit balance is too low")) << int(SummaryFailure::NoCredit);
        QTest::newRow("too many") << 429 << errorReply(QStringLiteral("rate_limit_error"), QStringLiteral("slow down")) << int(SummaryFailure::TooMany);
        QTest::newRow("overloaded") << 529 << errorReply(QStringLiteral("overloaded_error"), QStringLiteral("busy")) << int(SummaryFailure::Unavailable);
        QTest::newRow("server") << 500 << QByteArray() << int(SummaryFailure::Unavailable);
        QTest::newRow("redirect") << 302 << QByteArray() << int(SummaryFailure::Rejected);
        QTest::newRow("bad request") << 400 << errorReply(QStringLiteral("invalid_request_error"), QStringLiteral("nope")) << int(SummaryFailure::Rejected);
        QTest::newRow("unreadable") << 200 << QByteArray("<html>") << int(SummaryFailure::Empty);
        QTest::newRow("no text") << 200 << replyWith(QString()) << int(SummaryFailure::Empty);
        QTest::newRow("refusal") << 200 << replyWith(QString(), QStringLiteral("refusal")) << int(SummaryFailure::Refused);
    }

    void failuresAreToldApart()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, failure);
        m_fake->reply(body, status);
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        engine.setApiKey(kKey);
        QSignalSpy failed(&engine, &SummaryEngine::failed);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(int(failed.first().at(0).value<SummaryFailure>()), failure);
        QVERIFY(!failed.first().at(1).toString().contains(kKey));
        QVERIFY(!engine.isBusy());
    }

    void aNetworkFailureSaysSo()
    {
        TransportReply down;
        down.error = QStringLiteral("no route to host");
        m_fake->replies << down;
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        QSignalSpy failed(&engine, &SummaryEngine::failed);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(failed.first().at(0).value<SummaryFailure>(), SummaryFailure::Network);
    }

    void aLongAnswerSaysItWasCutOff()
    {
        m_fake->reply(replyWith(kGoodText, QStringLiteral("max_tokens")));
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        QSignalSpy finished(&engine, &SummaryEngine::finished);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        QVERIFY(finished.wait(kWaitMs));
        QVERIFY(finished.first().at(0).value<SummaryOutcome>().cutOff);
    }

    void cancellingIgnoresTheAnswer()
    {
        m_fake->reply(replyWith(kGoodText));
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        QSignalSpy finished(&engine, &SummaryEngine::finished);
        engine.generate(defaultModel(), defaultEffort(), examplePublication());
        engine.cancel();
        QVERIFY(!finished.wait(300));
        QVERIFY(!engine.isBusy());
    }

    void aClosedDialogCantBeReachedByALateAnswer()
    {
        m_fake->reply(replyWith(kGoodText));
        {
            SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
            engine.generate(defaultModel(), defaultEffort(), examplePublication());
        }
        QTest::qWait(100); // The answer arrives with nobody there, which mustn't crash.
    }

    void theListOfModelsSendsNothingAboutAnyPublication()
    {
        SummaryEngine engine(*m_transport, QStringLiteral("https://api.example"));
        engine.setApiKey(kKey);
        QSignalSpy ready(&engine, &SummaryEngine::modelsReady);
        engine.fetchModels();
        QVERIFY(ready.wait(kWaitMs));
        QCOMPARE(ready.first().at(0).toStringList(), QStringList({QStringLiteral("claude-sonnet-5-5"), QStringLiteral("claude-new-model")}));
        QVERIFY(m_fake->requests.first().body.isEmpty());
        QCOMPARE(m_fake->requests.first().method, QByteArray("GET"));
    }

    // ---- The real network path, on this computer only ----

    void theRealTransportSendsHeadersAndDoesntFollowRedirects()
    {
        // A redirect would otherwise carry the key to wherever it points.
        QTcpServer other;
        QVERIFY(other.listen(QHostAddress::LocalHost));
        int otherConnections = 0;
        connect(&other, &QTcpServer::newConnection, this, [&] { ++otherConnections; });

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray received;
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi";
        connect(&server, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                received += socket->readAll();
                if (received.contains("\r\n\r\n") && received.endsWith("the body")) {
                    socket->write(response);
                    socket->disconnectFromHost();
                }
            });
        });

        NetworkTransport transport;
        HttpRequest request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/v1/messages").arg(server.serverPort()));
        request.method = "POST";
        request.headers = {{"x-api-key", kKey.toUtf8()}, {"content-type", "application/json"}};
        request.body = "the body";

        const auto send = [&] {
            TransportReply result;
            QEventLoop loop;
            transport.send(request, [&](const TransportReply &reply) {
                result = reply;
                loop.quit();
            });
            QTimer::singleShot(kWaitMs, &loop, &QEventLoop::quit);
            loop.exec();
            return result;
        };

        TransportReply reply = send();
        QCOMPARE(reply.status, 200);
        QCOMPARE(reply.body, QByteArray("hi"));
        QVERIFY(received.startsWith("POST /v1/messages"));
        QVERIFY(received.toLower().contains("x-api-key: " + kKey.toLower().toUtf8())); // Qt may change a header's case.

        received.clear();
        response = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + QByteArray::number(other.serverPort()) + "/steal\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        reply = send();
        QCOMPARE(reply.status, 302);
        QTest::qWait(200);
        QCOMPARE(otherConnections, 0);
    }

    // ---- The dialog ----

    void itStartsOnSonnetAtHighEffortWithAnEstimate()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);

        QCOMPARE(dialog.modelBox()->currentData().toString(), QStringLiteral("claude-sonnet-5-5"));
        QCOMPARE(dialog.effortBox()->currentData().toString(), QStringLiteral("high"));
        QCOMPARE(dialog.effortBox()->count(), 5);
        QVERIFY(dialog.modelBox()->count() >= 3);
        QVERIFY(dialog.estimateLabel()->text().contains(QStringLiteral("$")));
        QCOMPARE(dialog.step(), SummaryDialog::Step::Choosing);

        // It shows just what will be sent, as plain text.
        QVERIFY(dialog.detailsLabel()->text().contains(QStringLiteral("Fatigue of Steel Beams")));
        QVERIFY(dialog.detailsLabel()->text().contains(QStringLiteral("https://doi.org/10.1000/example")));
        QVERIFY(!dialog.detailsLabel()->text().contains(QStringLiteral("My private note")));
        QCOMPARE(dialog.detailsLabel()->textFormat(), Qt::PlainText);

        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty())
            dialog.grab().save(QDir(pictures).filePath(QStringLiteral("summary-choosing.png")));

        // Changing what's chosen changes the estimate.
        const QString first = dialog.estimateLabel()->text();
        dialog.modelBox()->setCurrentIndex(dialog.modelBox()->findData(QStringLiteral("claude-opus-5-5")));
        QVERIFY(dialog.estimateLabel()->text() != first);
    }

    void nothingAboutThePublicationIsSentBeforeConfirming()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        m_fake->reply(replyWith(kGoodText));
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        QTest::qWait(200);
        QCOMPARE(m_fake->messages().size(), 0);
        for (const HttpRequest &request : std::as_const(m_fake->requests))
            QVERIFY(request.body.isEmpty());

        dialog.generate();
        QCOMPARE(m_fake->messages().size(), 1);
    }

    void modelsFromTheAccountAreOfferedWithoutAPrice()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.modelBox()->findData(QStringLiteral("claude-new-model")) >= 0, kWaitMs);
        dialog.modelBox()->setCurrentIndex(dialog.modelBox()->findData(QStringLiteral("claude-new-model")));
        QVERIFY(!dialog.estimateLabel()->text().contains(QStringLiteral("$")));
    }

    void withNoKeyItAsksForOneAndSendsNothing()
    {
        const qint64 id = addExample();
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.show();
        QVERIFY(dialog.keyRow()->isVisibleTo(&dialog));
        QVERIFY(!dialog.generateButton()->isEnabled());
        QCOMPARE(dialog.keyEdit()->echoMode(), QLineEdit::Password);
        QTest::qWait(100);
        QVERIFY(m_fake->requests.isEmpty());

        // Entering one saves it where keys go, and then it's used.
        m_fake->reply(replyWith(kGoodText));
        dialog.keyEdit()->setText(QStringLiteral("  ") + kKey + QStringLiteral(" "));
        QVERIFY(dialog.generateButton()->isEnabled());
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        QString stored;
        m_keys->read([&](const QString &key) { stored = key; });
        QCOMPARE(stored, kKey);
    }

    void theKeyIsNeverInTheDatabaseOrSettings()
    {
        const qint64 id = addExample();
        m_fake->reply(replyWith(kGoodText));
        {
            SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
            dialog.keyEdit()->setText(kKey);
            dialog.generate();
            QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
            dialog.accept();
        }
        m_session->database().exec("PRAGMA wal_checkpoint(FULL)");

        // Every file Papertrail has written for this library.
        QDirIterator files(m_dir->path(), QDir::Files, QDirIterator::Subdirectories);
        int looked = 0;
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY2(!file.readAll().contains(kKey.toUtf8()), qPrintable(file.fileName()));
            ++looked;
        }
        QVERIFY(looked > 0);
    }

    void acceptingKeepsItAndChoicesAreRemembered()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        QJsonArray fetched;
        fetched.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("web_fetch_tool_result")},
                                   {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("web_fetch_result")},
                                                                           {QStringLiteral("url"), QStringLiteral("https://example.org/read")},
                                                                           {QStringLiteral("content"), QJsonObject{{QStringLiteral("title"), QStringLiteral("The Paper")}}}}}});
        m_fake->reply(replyWith(kGoodText, QStringLiteral("end_turn"), 1000, 500, 1, fetched));

        const Publication before = *m_session->publications().get(id);
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.effortBox()->setCurrentIndex(dialog.effortBox()->findData(QStringLiteral("medium")));
        dialog.generate();
        QCOMPARE(dialog.step(), SummaryDialog::Step::Working);
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);

        // For looking at by eye, if asked for.
        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty())
            dialog.grab().save(QDir(pictures).filePath(QStringLiteral("summary-result.png")));

        // Everything's there to judge it by, shown as plain text.
        QVERIFY(dialog.basisLabel()->text().contains(QStringLiteral("full text")));
        QVERIFY(dialog.summaryView()->toPlainText().contains(QStringLiteral("They bent some beams.")));
        for (const QString &heading : {QStringLiteral("Aim and background"), QStringLiteral("Methodology"), QStringLiteral("Results"),
                                       QStringLiteral("Limitations"), QStringLiteral("Conclusion and discussion")})
            QVERIFY(dialog.summaryView()->toPlainText().contains(heading));
        QCOMPARE(dialog.sourceList()->count(), 1);
        QVERIFY(dialog.sourceList()->item(0)->text().contains(QStringLiteral("https://example.org/read")));
        QVERIFY(dialog.costLabel()->text().contains(QStringLiteral("$")));
        QVERIFY(dialog.costLabel()->text().contains(QStringLiteral("1000")));

        // Nothing's stored until it's accepted, but what it cost is counted straight away.
        SummaryRepository summaries(m_session->database());
        QVERIFY(!summaries.forPublication(id).has_value());
        const QList<SpendEntry> spent = spendEntries(m_session->database());
        QCOMPARE(spent.size(), 1);
        QCOMPARE(spent.first().kind, SpendKind::Instant);
        QCOMPARE(spent.first().publicationId.value_or(0), id);
        QVERIFY(spent.first().cost.value_or(0) > 0);
        dialog.accept();
        QVERIFY(dialog.kept());

        const StoredSummary stored = *summaries.forPublication(id);
        QCOMPARE(stored.model, QStringLiteral("claude-sonnet-5-5"));
        QCOMPARE(stored.effort, QStringLiteral("medium"));
        QCOMPARE(stored.basis, QStringLiteral("full text"));
        QVERIFY(stored.text.contains(QStringLiteral("The beams broke.")));
        QCOMPARE(stored.sources.size(), 1);
        QVERIFY(stored.costDollars.value_or(0) > 0);
        QVERIFY(!stored.sentPaper);

        QCOMPARE(m_session->settings().value(QStringLiteral("summary.effort")).toString(), QStringLiteral("medium"));

        // It's kept apart: the publication's own details are just as they were.
        const Publication after = *m_session->publications().get(id);
        QCOMPARE(after.title, before.title);
        QCOMPARE(after.abstract, before.abstract);
        QCOMPARE(after.notes, before.notes);
    }

    void discardingKeepsNothing()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        m_fake->reply(replyWith(kGoodText));
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        dialog.discard();
        QVERIFY(!dialog.kept());
        QVERIFY(!SummaryRepository(m_session->database()).forPublication(id).has_value());
    }

    void aSummaryWithLittleBehindItSaysSo_data()
    {
        QTest::addColumn<QString>("based");
        QTest::addColumn<QString>("identified");
        QTest::addColumn<QString>("expected");
        QTest::newRow("abstract") << QStringLiteral("abstract only") << QStringLiteral("yes") << QStringLiteral("only read the abstract");
        QTest::newRow("other") << QStringLiteral("other sources") << QStringLiteral("yes") << QStringLiteral("couldn't read the paper");
        QTest::newRow("not found") << QStringLiteral("not found") << QStringLiteral("no") << QStringLiteral("couldn't find this paper");
        QTest::newRow("unsure") << QStringLiteral("full text") << QStringLiteral("unsure") << QStringLiteral("wasn't sure");
    }

    void aSummaryWithLittleBehindItSaysSo()
    {
        QFETCH(QString, based);
        QFETCH(QString, identified);
        QFETCH(QString, expected);
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        QString text = kGoodText;
        text.replace(QStringLiteral("BASED ON: full text"), QStringLiteral("BASED ON: ") + based);
        text.replace(QStringLiteral("IDENTIFIED: yes"), QStringLiteral("IDENTIFIED: ") + identified);
        m_fake->reply(replyWith(text));
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        QVERIFY2(dialog.basisLabel()->text().contains(expected), qPrintable(dialog.basisLabel()->text()));
    }

    void aDeclinedRequestOffersAnotherGoWithANewEstimate()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        m_fake->reply(replyWith(QString(), QStringLiteral("refusal"), 500, 0, 0));
        m_fake->reply(replyWith(kGoodText));
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Problem, kWaitMs);
        QVERIFY(dialog.problemLabel()->text().contains(QStringLiteral("declined")));
        QVERIFY(dialog.problemLabel()->text().contains(QStringLiteral("another model")));
        QVERIFY(dialog.spentSoFar() > 0);
        QCOMPARE(m_fake->messages().size(), 1); // It doesn't move to another model by itself.

        dialog.backToChoosing();
        QCOMPARE(dialog.step(), SummaryDialog::Step::Choosing);
        dialog.modelBox()->setCurrentIndex(dialog.modelBox()->findData(QStringLiteral("claude-opus-5-5")));
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        QCOMPARE(QJsonDocument::fromJson(m_fake->messages().at(1).body).object().value(QStringLiteral("model")).toString(), QStringLiteral("claude-opus-5-5"));
    }

    void aRefusedKeyIsForgottenAndAskedForAgain()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        m_fake->reply(errorReply(QStringLiteral("authentication_error"), QStringLiteral("invalid x-api-key")), 401);
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.show();
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Problem, kWaitMs);
        QString stored = QStringLiteral("still there");
        m_keys->read([&](const QString &key) { stored = key; });
        QVERIFY(stored.isEmpty());
        dialog.backToChoosing();
        QVERIFY(dialog.keyRow()->isVisibleTo(&dialog));
    }

    // ---- Sending the paper, as a last resort ----

    void thePaperOnlyGoesAfterItsOwnConfirmation()
    {
        const qint64 id = importSample(QStringLiteral("article.pdf"));
        m_keys->write(kKey, [](bool) {});
        QString abstractOnly = kGoodText;
        abstractOnly.replace(QStringLiteral("full text"), QStringLiteral("abstract only"));
        m_fake->reply(replyWith(abstractOnly));
        m_fake->reply(replyWith(kGoodText, QStringLiteral("end_turn"), 5000, 800, 0));

        const QString file = QDir(m_docs).filePath(QStringLiteral("article.pdf"));
        QFile original(file);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray bytes = original.readAll();
        original.close();
        const QByteArray hashBefore = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);

        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        QVERIFY(!m_fake->messages().first().body.contains(bytes.toBase64()));
        const double spentBefore = dialog.spentSoFar();
        QVERIFY(spentBefore > 0);

        // Asking restates what happens and what it costs, and sends nothing.
        dialog.askAboutPaper();
        QCOMPARE(dialog.step(), SummaryDialog::Step::ConfirmingPaper);
        const QString said = dialog.paperLabel()->text();
        QVERIFY(said.contains(QStringLiteral("article.pdf")));
        QVERIFY(said.contains(QStringLiteral("leaves this computer")));
        QVERIFY(said.contains(QStringLiteral("Estimated cost")));
        QVERIFY(said.contains(QStringLiteral("Spent so far")));
        QVERIFY(said.contains(formatDollars(spentBefore)));
        QTest::qWait(200);
        QCOMPARE(m_fake->messages().size(), 1);

        dialog.sendPaper();
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->messages().size(), 2, kWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);

        const QByteArray sent = m_fake->messages().at(1).body;
        const QJsonObject request = QJsonDocument::fromJson(sent).object();
        QVERIFY(!request.contains(QStringLiteral("tools")));
        const QJsonObject document = request.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("content")).toArray().at(0).toObject();
        QCOMPARE(document.value(QStringLiteral("source")).toObject().value(QStringLiteral("data")).toString().toLatin1(), bytes.toBase64());

        QVERIFY(dialog.spentSoFar() > spentBefore);
        QVERIFY(dialog.costLabel()->text().contains(formatDollars(dialog.spentSoFar())));
        QVERIFY(!dialog.sendPaperButton()->isVisibleTo(&dialog)); // There's nothing further to send.

        dialog.accept();
        QVERIFY(SummaryRepository(m_session->database()).forPublication(id)->sentPaper);

        // The file itself is only read.
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256), hashBefore);
    }

    void anotherFormatGoesAsItsText()
    {
        const qint64 id = importSample(QStringLiteral("reader_nav.epub"));
        m_keys->write(kKey, [](bool) {});
        m_fake->reply(replyWith(kGoodText));
        m_fake->reply(replyWith(kGoodText));
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        dialog.askAboutPaper();
        QVERIFY(dialog.paperLabel()->text().contains(QStringLiteral("The text of")));
        dialog.sendPaper();
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->messages().size(), 2, kWaitMs);
        const QString sent = QString::fromUtf8(m_fake->messages().at(1).body);
        QVERIFY(sent.contains(QStringLiteral("<paper>")));
        QVERIFY(!sent.contains(QStringLiteral("\"document\"")));
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
    }

    void anArticleOnlySendsItsOwnPages()
    {
        const qint64 volume = importSample(QStringLiteral("reader_doc.pdf"));
        Publication record = *m_session->publications().get(volume);
        record.type = PublicationType::JournalIssue;
        m_session->publications().update(record);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        draft.firstPage = 2;
        draft.lastPage = 2;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();

        m_keys->write(kKey, [](bool) {});
        m_fake->reply(replyWith(kGoodText));
        m_fake->reply(replyWith(kGoodText));
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, article);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        dialog.askAboutPaper();
        QVERIFY(dialog.paperLabel()->text().contains(QStringLiteral("pages 2 to 2")));
        dialog.sendPaper();
        QTRY_COMPARE_WITH_TIMEOUT(m_fake->messages().size(), 2, kWaitMs);

        const QString sent = QString::fromUtf8(m_fake->messages().at(1).body);
        QVERIFY(!sent.contains(QStringLiteral("\"document\"")));
        const qsizetype start = sent.indexOf(QStringLiteral("<paper>"));
        const QString paper = sent.mid(start, sent.indexOf(QStringLiteral("</paper>")) - start);
        QVERIFY(paper.contains(QStringLiteral("findme"))); // It's on page 2.
        QVERIFY(!paper.contains(QStringLiteral("opening page"))); // That's page 1.
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
    }

    void whatCantBeSentSaysWhy()
    {
        // A publication whose file isn't there.
        Publication p = examplePublication();
        const qint64 noFile = m_session->publications().create(p);
        m_keys->write(kKey, [](bool) {});
        m_fake->reply(replyWith(kGoodText));
        {
            SummaryDialog dialog(*m_session, *m_keys, *m_transport, noFile);
            dialog.generate();
            QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
            dialog.askAboutPaper();
            QVERIFY(dialog.paperLabel()->text().contains(QStringLiteral("can't be found")));
            QVERIFY(!dialog.confirmPaperButton()->isEnabled());
        }

        // A PDF over Anthropic's limits, which isn't cut down to fit.
        const qint64 id = importSample(QStringLiteral("article.pdf"));
        const FileRecord file = m_session->files().filesFor(id).first();
        m_session->files().setDocumentFacts(file.id, kMostPdfPages + 1, true);
        SummaryDialog dialog(*m_session, *m_keys, *m_transport, id);
        dialog.generate();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.step(), SummaryDialog::Step::Result, kWaitMs);
        dialog.askAboutPaper();
        QVERIFY(dialog.paperLabel()->text().contains(QStringLiteral("larger than Anthropic allows")));
        QVERIFY(!dialog.confirmPaperButton()->isEnabled());
    }

    // ---- In the library ----

    void aKeptSummaryShowsInTheDetailsAndCanBeDeleted()
    {
        const qint64 id = addExample();
        StoredSummary stored;
        stored.publicationId = id;
        stored.model = QStringLiteral("claude-sonnet-5-5");
        stored.effort = QStringLiteral("high");
        stored.basis = QStringLiteral("abstract only");
        stored.identificationUnsure = true;
        stored.text = QStringLiteral("Aim and background\n<b>Not bold</b>");
        stored.sources = {qMakePair(QStringLiteral("https://example.org/read"), QStringLiteral("The Paper"))};
        SummaryRepository(m_session->database()).save(stored);

        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->show();
        DetailsPanel *details = m_window->details();
        details->showPublication(id);
        QVERIFY(details->summaryView()->isVisibleTo(details));
        QVERIFY(details->summaryView()->toPlainText().contains(stored.text));
        QVERIFY(details->summaryView()->toPlainText().contains(QStringLiteral("https://example.org/read")));

        // It's clearly marked as generated, and by what.
        QVERIFY(details->summaryInfo()->text().contains(QStringLiteral("Claude Sonnet 5.5")));
        QVERIFY(details->summaryInfo()->text().contains(QStringLiteral("abstract")));
        QCOMPARE(details->summaryInfo()->textFormat(), Qt::PlainText);

        m_boxWatcher->stop();
        QTimer::singleShot(100, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Yes)->click();
        });
        details->deleteSummaryButton()->click();
        m_boxWatcher->start(100);
        QVERIFY(!SummaryRepository(m_session->database()).forPublication(id).has_value());
        QVERIFY(!details->summaryView()->isVisibleTo(details));
    }

    void savingAgainReplacesItAndItGoesWithThePublication()
    {
        const qint64 id = addExample();
        SummaryRepository summaries(m_session->database());
        StoredSummary first;
        first.publicationId = id;
        first.model = QStringLiteral("claude-sonnet-5-5");
        first.effort = QStringLiteral("high");
        first.basis = QStringLiteral("full text");
        first.text = QStringLiteral("First");
        summaries.save(first);
        StoredSummary second = first;
        second.uuid.clear();
        second.text = QStringLiteral("Second");
        summaries.save(second);
        QCOMPARE(summaries.forPublication(id)->text, QStringLiteral("Second"));
        QVERIFY(!summaries.forPublication(id)->uuid.isEmpty());

        m_session->publications().remove(id);
        QVERIFY(!summaries.forPublication(id).has_value());
    }

    void theWindowConfirmsABatchAndCollectsIt()
    {
        // The window's own transport and key store, which the other tests here don't need.
        LibraryPaths paths = m_session->paths();
        m_session.reset();
        auto fake = m_fake;
        paths.transportFactory = [fake] { return std::make_unique<FakeTransport>(fake); };
        paths.keyStoreFactory = [] {
            auto keys = std::make_unique<MemoryKeyStore>();
            keys->write(kKey, [](bool) {});
            return std::unique_ptr<ApiKeyStore>(std::move(keys));
        };
        m_session = std::make_unique<LibrarySession>(paths);
        const qint64 fresh = addExample();
        Publication other = examplePublication();
        other.title = QStringLiteral("Corrosion of Made-Up Bridges");
        const qint64 summarised = m_session->publications().create(other);
        StoredSummary kept;
        kept.publicationId = summarised;
        kept.model = defaultModel();
        kept.effort = defaultEffort();
        kept.basis = QStringLiteral("abstract only");
        kept.text = QStringLiteral("Kept already");
        SummaryRepository(m_session->database()).save(kept);

        // What's sent is confirmed first: one with a summary is left out unless it's to be replaced.
        BatchSummaryDialog dialog(*m_session, {fresh, summarised});
        QCOMPARE(dialog.included().size(), 1);
        QCOMPARE(dialog.included().first().id, fresh);
        QVERIFY(dialog.replaceBox()->isVisibleTo(&dialog));
        QVERIFY(dialog.estimateLabel()->text().contains(QStringLiteral("1 publication will be sent")));
        QVERIFY(dialog.estimateLabel()->text().contains(QStringLiteral("1 is left out")));
        QVERIFY(dialog.estimateLabel()->text().contains(QStringLiteral("half price")));
        dialog.replaceBox()->setChecked(true);
        QCOMPARE(dialog.included().size(), 2);
        QCOMPARE(dialog.sendButton()->text(), QStringLiteral("Send 2"));
        QVERIFY(m_fake->requests.isEmpty()); // Estimating asks nothing.

        // One on its way, as if sent earlier, is collected when the window checks.
        m_window = std::make_unique<MainWindow>(*m_session);
        m_fake->reply(R"({"id":"msgbatch_09","type":"message_batch","processing_status":"in_progress"})");
        QSignalSpy submitted(m_window->summaryBatches(), &SummaryBatches::submitted);
        m_window->summaryBatches()->setApiKey(kKey);
        Publication sent = *m_session->publications().get(fresh);
        m_window->summaryBatches()->submit(defaultModel(), defaultEffort(), {sent});
        QVERIFY(submitted.wait(kWaitMs));
        m_window->summaryBatches()->setApiKey(QString()); // The window reads its own from the key store.

        // It's said on the publication, and in the status bar.
        m_window->details()->showPublication(fresh);
        QVERIFY(m_window->details()->summaryOnItsWayLabel()->isVisibleTo(m_window->details()));
        QVERIFY(m_window->details()->summaryOnItsWayLabel()->text().contains(QStringLiteral("on its way")));
        QCOMPARE(m_window->summaryStatusButton()->text(), QStringLiteral("1 summary on its way"));
        {
            SummariesDialog list(*m_session, *m_window->summaryBatches());
            QCOMPARE(list.waitingList()->topLevelItemCount(), 1);
            QCOMPARE(list.waitingList()->topLevelItem(0)->text(0), QStringLiteral("Fatigue of Steel Beams"));
            QCOMPARE(list.waitingList()->topLevelItem(0)->text(3), QStringLiteral("On its way"));
            QCOMPARE(list.reviewList()->count(), 0);
        }

        m_fake->replies.clear();
        m_fake->reply(R"({"id":"msgbatch_09","type":"message_batch","processing_status":"ended"})");
        m_fake->reply(QJsonDocument(QJsonObject{{QStringLiteral("custom_id"), QStringLiteral("p%1").arg(fresh)},
                                                {QStringLiteral("result"), QJsonObject{{QStringLiteral("type"), QStringLiteral("succeeded")},
                                                                                       {QStringLiteral("message"), QJsonDocument::fromJson(replyWith(kGoodText)).object()}}}})
                          .toJson(QJsonDocument::Compact));
        QSignalSpy saved(m_window->summaryBatches(), &SummaryBatches::summariesArrived);
        m_window->checkSummaryBatches();
        QVERIFY(saved.wait(kWaitMs));
        QCOMPARE(SummaryBatches::waitingCount(m_session->database()), qint64(0));

        // It waits to be reviewed, which the publication and the status bar say.
        QVERIFY(!SummaryRepository(m_session->database()).forPublication(fresh));
        QTRY_VERIFY_WITH_TIMEOUT(m_window->details()->summaryOnItsWayLabel()->text().contains(QStringLiteral("has arrived")), kWaitMs);
        QCOMPARE(m_window->details()->summariesButton()->text(), QStringLiteral("Review summary..."));
        QCOMPARE(m_window->summaryStatusButton()->text(), QStringLiteral("1 summary to review"));
        {
            SummariesDialog list(*m_session, *m_window->summaryBatches());
            QCOMPARE(list.tabs()->currentIndex(), int(SummariesDialog::Page::ToReview));
            QCOMPARE(list.reviewList()->count(), 1);
            QVERIFY(list.reviewText()->toPlainText().contains(QStringLiteral("The beams broke.")));
            QVERIFY(list.reviewInfo()->text().contains(QStringLiteral("half price")));
            QVERIFY(list.keepButton()->isEnabled());
            QVERIFY(list.totalsLabel()->text().contains(QStringLiteral("$")));
            QCOMPARE(list.spendingList()->topLevelItemCount(), 1);
            QCOMPARE(list.spendingList()->topLevelItem(0)->text(3), QStringLiteral("At half price"));
            // The Console is a link wherever it's named.
            QVERIFY(list.spendingNote()->text().contains(QStringLiteral("<a href=\"https://platform.claude.com/settings/billing\">Claude Console</a>")));
            QVERIFY(list.spendingNote()->openExternalLinks());
            const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
            if (!pictures.isEmpty()) {
                list.resize(820, 600);
                list.grab().save(QDir(pictures).filePath(QStringLiteral("summaries-review.png")));
                list.showPage(SummariesDialog::Page::Spending);
                list.grab().save(QDir(pictures).filePath(QStringLiteral("summaries-spending.png")));
            }
            list.keepButton()->click();
            QCOMPARE(list.reviewList()->count(), 0);
        }
        QVERIFY(SummaryRepository(m_session->database()).forPublication(fresh));
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->summaryStatusButton()->isVisibleTo(m_window.get()), kWaitMs);
        QVERIFY(!m_window->details()->summaryOnItsWayLabel()->isVisibleTo(m_window->details()));
        bool keyed = false;
        for (const auto &header : m_fake->requests.last().headers)
            keyed = keyed || (header.first == "x-api-key" && header.second == kKey.toUtf8());
        QVERIFY(keyed);
        QCOMPARE(SummaryRepository(m_session->database()).forPublication(summarised)->text, QStringLiteral("Kept already"));
    }

    void theWindowOffersItAndAskingNeverSendsByItself()
    {
        const qint64 id = addExample();
        m_keys->write(kKey, [](bool) {});
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->show();
        QTest::qWait(300);
        // Having a key, and opening the library, sends nothing.
        QVERIFY(m_fake->requests.isEmpty());
        m_window->details()->showPublication(id);
        QVERIFY(m_window->details()->summarizeButton()->isVisibleTo(m_window->details()));
    }

private:
    Publication examplePublication()
    {
        Publication p;
        p.title = QStringLiteral("Fatigue of Steel Beams");
        p.authors = {{QStringLiteral("Example"), QStringLiteral("Ada")}};
        p.year = 2020;
        p.containerTitle = QStringLiteral("Journal of Made-up Results");
        p.doi = QStringLiteral("10.1000/example");
        p.notes = QStringLiteral("My private note");
        p.abstract = QStringLiteral("An abstract kept here.");
        return p;
    }

    qint64 addExample()
    {
        Publication p = examplePublication();
        return m_session->publications().create(p);
    }

    qint64 importSample(const QString &name)
    {
        QFile::copy(sample(name), QDir(m_docs).filePath(name));
        if (!m_window) {
            m_window = std::make_unique<MainWindow>(*m_session);
            m_window->show();
        }
        const qint64 before = m_session->query().count({});
        m_window->addFolder(m_docs);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 60000 && (m_session->query().count({}) <= before || m_window->importThread()->isBusy()))
            QTest::qWait(50);
        for (const qint64 id : m_session->query().ids({}, {})) {
            const QList<FileRecord> files = m_session->files().filesFor(id);
            if (!files.isEmpty() && files.first().relativePath == name)
                return id;
        }
        return 0;
    }

    static QString sample(const QString &name) { return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name); }

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_docs;
    std::shared_ptr<FakeState> m_fake;
    std::unique_ptr<FakeTransport> m_transport;
    std::unique_ptr<ApiKeyStore> m_keys;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
};

QTEST_MAIN(SummaryTest)
#include "test_summary.moc"
