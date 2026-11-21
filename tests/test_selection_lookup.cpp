// Looking up a selection: the words sent, the addresses built, what's read from Wikipedia's and Wiktionary's answers,
// where the library mentions the words, and that nothing goes online unless lookups are online and the user asks.
// Every answer here is made up, and nothing reaches the network.
#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryMentions.h"
#include "core/repo/AnnotationRepository.h"
#include "core/repo/FileRepository.h"
#include "core/repo/FullTextIndex.h"
#include "core/repo/PublicationRepository.h"
#include "core/settings/SettingsStore.h"
#include "metadata/SelectionLookup.h"
#include "metadata/Transport.h"
#include "ui/LibrarySession.h"
#include "ui/Delegates.h"
#include "ui/LookUpPanel.h"

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::metadata;
using papertrail::ui::LibraryPaths;
using papertrail::ui::LibrarySession;
using papertrail::ui::ArticleDelegate;
using papertrail::ui::LookUpPanel;

namespace {

struct Asked {
    QStringList urls;
    QList<QPair<QString, TransportReply>> answers; // By a part of the address.
};

class FakeTransport : public Transport {
public:
    explicit FakeTransport(std::shared_ptr<Asked> asked) : m_asked(std::move(asked)) {}

    void get(const QUrl &url, const Callback &callback) override
    {
        const QString text = url.toString(QUrl::FullyEncoded);
        m_asked->urls << text;
        TransportReply reply;
        reply.status = 404;
        for (const auto &answer : m_asked->answers) {
            if (text.contains(answer.first)) {
                reply = answer.second;
                break;
            }
        }
        QTimer::singleShot(0, [callback, reply] { callback(reply); });
    }

private:
    std::shared_ptr<Asked> m_asked;
};

TransportReply ok(const QByteArray &body)
{
    TransportReply reply;
    reply.status = 200;
    reply.body = body;
    return reply;
}

const QByteArray kSearch = "{\"pages\": [\n"
    "    {\"id\": 1, \"key\": \"Grain_boundary\", \"title\": \"Grain boundary\", \"excerpt\": \"a <span class=\\\"searchmatch\\\">grain</span>\",\n"
    "     \"description\": \"Interface between crystallites\"},\n"
    "    {\"id\": 2, \"key\": \"Grain_boundary_strengthening\", \"title\": \"Grain boundary strengthening\", \"description\": null},\n"
    "    {\"id\": 3, \"key\": \"\", \"title\": \"No key\"}\n"
    "]}";

const QByteArray kSummary = "{\"type\": \"standard\", \"title\": \"Grain boundary\",\n"
    "    \"titles\": {\"canonical\": \"Grain_boundary\", \"normalized\": \"Grain boundary\"},\n"
    "    \"description\": \"Interface between crystallites\",\n"
    "    \"extract\": \"In this made-up summary, a grain boundary is where two grains meet.\",\n"
    "    \"extract_html\": \"<p>In this made-up summary, <b>a grain boundary</b> is where two grains meet.</p>\"}";

const QByteArray kDisambiguation = "{\"type\": \"disambiguation\", \"title\": \"Lattice\",\n"
    "    \"titles\": {\"canonical\": \"Lattice\"}, \"extract\": \"\"}";

const QByteArray kDefinition = "{\n"
    "    \"it\": [{\"partOfSpeech\": \"Noun\", \"language\": \"Italian\", \"definitions\": [{\"definition\": \"a made-up Italian sense\"}]}],\n"
    "    \"en\": [\n"
    "        {\"partOfSpeech\": \"Noun\", \"language\": \"English\", \"definitions\": [\n"
    "            {\"definition\": \"\"},\n"
    "            {\"definition\": \"A <a rel=\\\"mw:WikiLink\\\" href=\\\"/wiki/frame\\\" title=\\\"frame\\\">frame</a> of crossed strips &amp; bars.\"},\n"
    "            {\"definition\": \"A regular <b>arrangement</b> of points&#x2e;\"}]},\n"
    "        {\"partOfSpeech\": \"Verb\", \"language\": \"English\", \"definitions\": [{\"definition\": \"To make into a lattice.\"}]}\n"
    "    ]}";

} // namespace

class SelectionLookupTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_asked = std::make_shared<Asked>();
    }

    void cleanup()
    {
        m_panel.reset();
        m_session.reset();
        m_dir.reset();
    }

    // ---- The words sent ----

    void onlyTheWordsAreSent()
    {
        QCOMPARE(lookupWords(QStringLiteral("  (grain\n boundary).  "), 8, 100), QStringLiteral("grain boundary"));
        QCOMPARE(lookupWords(QStringLiteral("“lattice”,"), 8, 100), QStringLiteral("lattice"));
        // A word broken across lines comes back together.
        QCOMPARE(lookupWords(QStringLiteral("crystal-\nlographic"), 8, 100), QStringLiteral("crystallographic"));
        // Long selections are cut to whole words.
        QCOMPARE(lookupWords(QStringLiteral("one two three four five six seven eight nine ten"), 8, 100),
                 QStringLiteral("one two three four five six seven eight"));
        QCOMPARE(lookupWords(QStringLiteral("abcdef ghijkl mnopqr"), 8, 15), QStringLiteral("abcdef ghijkl"));
        // Nothing that controls text, and nothing at all if only punctuation's left.
        QCOMPARE(lookupWords(QStringLiteral("a") + QChar(0x202E) + QStringLiteral("b\u0007c"), 8, 100), QStringLiteral("a b c"));
        QCOMPARE(lookupWords(QStringLiteral(" ... "), 8, 100), QString());
        QCOMPARE(wordCount(QStringLiteral("grain boundary energy")), 3);
    }

    void languagesAreChecked()
    {
        QVERIFY(isWikiLanguage(QStringLiteral("en")));
        QVERIFY(isWikiLanguage(QStringLiteral("fil")));
        QVERIFY(!isWikiLanguage(QStringLiteral("EN")));
        QVERIFY(!isWikiLanguage(QStringLiteral("en.evil.example")));
        QVERIFY(!isWikiLanguage(QStringLiteral("e")));
        QCOMPARE(wikiLanguageFor(QStringLiteral("de_DE")), QStringLiteral("de"));
        QCOMPARE(wikiLanguageFor(QStringLiteral("pt-BR")), QStringLiteral("pt"));
        QCOMPARE(wikiLanguageFor(QStringLiteral("C")), QStringLiteral("en"));
    }

    // ---- The addresses ----

    void addressesAreBuiltFromEncodedParts()
    {
        QCOMPARE(wikipediaSearchUrl(QStringLiteral("en"), QStringLiteral("grain boundary"), 5).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://en.wikipedia.org/w/rest.php/v1/search/page?q=grain%20boundary&limit=5"));
        // Nothing in the words can add to the query or change the path.
        const QString search = wikipediaSearchUrl(QStringLiteral("en"), QStringLiteral("a+b&limit=99#x"), 5).toString(QUrl::FullyEncoded);
        QCOMPARE(search, QStringLiteral("https://en.wikipedia.org/w/rest.php/v1/search/page?q=a%2Bb%26limit%3D99%23x&limit=5"));
        QCOMPARE(wikipediaSummaryUrl(QStringLiteral("en"), QStringLiteral("AC/DC current")).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://en.wikipedia.org/api/rest_v1/page/summary/AC%2FDC_current"));
        QCOMPARE(wikipediaSummaryUrl(QStringLiteral("en"), QStringLiteral("../../w/api.php?x")).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://en.wikipedia.org/api/rest_v1/page/summary/..%2F..%2Fw%2Fapi.php%3Fx"));
        QCOMPARE(wikipediaPageLink(QStringLiteral("de"), QStringLiteral("Korngrenze")).toString(),
                 QStringLiteral("https://de.wikipedia.org/wiki/Korngrenze"));
        QCOMPARE(wiktionaryDefinitionUrl(QStringLiteral("grain boundary")).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://en.wiktionary.org/api/rest_v1/page/definition/grain_boundary"));
        QCOMPARE(wiktionaryPageLink(QStringLiteral("Gitter")).toString(), QStringLiteral("https://en.wiktionary.org/wiki/Gitter"));
        // A language that isn't one, or nothing to look up, gives no address at all.
        QVERIFY(wikipediaSearchUrl(QStringLiteral("evil.example/x"), QStringLiteral("a"), 5).isEmpty());
        QVERIFY(wikipediaSummaryUrl(QStringLiteral("en"), QString()).isEmpty());
        QVERIFY(wikipediaSummaryUrl(QStringLiteral("en"), QString(400, u'a')).isEmpty());
        QVERIFY(wiktionaryDefinitionUrl(QString()).isEmpty());
    }

    // ---- What's read from answers ----

    void htmlBecomesPlainText()
    {
        QCOMPARE(plainFromHtml(QStringLiteral("A <a href=\"/wiki/x\">frame</a> &amp; <b>bars</b>&#46; &#x41;&lt;&gt;")),
                 QStringLiteral("A frame & bars. A<>"));
        // References to characters that control text, or aren't characters, stay as they were.
        QCOMPARE(plainFromHtml(QStringLiteral("a&#7;b &#xD800; &#x202E;")), QStringLiteral("a&#7;b &#xD800; ") + QChar(0x202E));
        QCOMPARE(plainFromHtml(QStringLiteral("unclosed <tag")), QStringLiteral("unclosed"));
    }

    void wikipediaAnswersAreRead()
    {
        const QList<WikipediaPage> pages = parseWikipediaSearch(kSearch);
        QCOMPARE(pages.size(), 2); // The one with no key is left out.
        QCOMPARE(pages.at(0).key, QStringLiteral("Grain_boundary"));
        QCOMPARE(pages.at(0).description, QStringLiteral("Interface between crystallites"));
        QCOMPARE(pages.at(1).description, QString());

        const std::optional<WikipediaSummary> summary = parseWikipediaSummary(kSummary);
        QVERIFY(summary);
        QCOMPARE(summary->key, QStringLiteral("Grain_boundary"));
        QCOMPARE(summary->title, QStringLiteral("Grain boundary"));
        QVERIFY(summary->extract.startsWith(QStringLiteral("In this made-up summary")));
        QVERIFY(!summary->disambiguation);

        const std::optional<WikipediaSummary> several = parseWikipediaSummary(kDisambiguation);
        QVERIFY(several);
        QVERIFY(several->disambiguation);

        QVERIFY(parseWikipediaSearch("not json").isEmpty());
        QVERIFY(!parseWikipediaSummary("[]"));
        QVERIFY(!parseWikipediaSummary("{\"title\": \"No text\"}"));
    }

    void wiktionaryAnswersAreRead()
    {
        const QList<WiktionaryEntry> entries = parseWiktionary(kDefinition, QStringLiteral("en"));
        QCOMPARE(entries.size(), 3);
        // The reader's language comes first, and empty definitions are left out.
        QCOMPARE(entries.at(0).language, QStringLiteral("English"));
        QCOMPARE(entries.at(0).partOfSpeech, QStringLiteral("Noun"));
        QCOMPARE(entries.at(0).definitions,
                 QStringList({QStringLiteral("A frame of crossed strips & bars."), QStringLiteral("A regular arrangement of points.")}));
        QCOMPARE(entries.at(1).partOfSpeech, QStringLiteral("Verb"));
        QCOMPARE(entries.at(2).language, QStringLiteral("Italian"));
        QCOMPARE(parseWiktionary(kDefinition, QStringLiteral("it")).first().language, QStringLiteral("Italian"));
        QVERIFY(parseWiktionary("{\"status\": 404}", QStringLiteral("en")).isEmpty());
    }

    // ---- Asking ----

    void wikipediaIsSearchedThenTheBestMatchFetched()
    {
        m_asked->answers << qMakePair(QStringLiteral("/search/page"), ok(kSearch))
                         << qMakePair(QStringLiteral("/page/summary/Grain_boundary"), ok(kSummary));
        FakeTransport transport(m_asked);
        SelectionLookup lookup(transport);
        QSignalSpy found(&lookup, &SelectionLookup::wikipediaFound);
        lookup.lookUpOnWikipedia(QStringLiteral("grain boundary"), QStringLiteral("en"));
        QVERIFY(found.wait());
        QCOMPARE(m_asked->urls.size(), 2);
        QVERIFY(m_asked->urls.at(0).startsWith(QStringLiteral("https://en.wikipedia.org/w/rest.php/v1/search/page?q=grain%20boundary")));
        QCOMPARE(m_asked->urls.at(1), QStringLiteral("https://en.wikipedia.org/api/rest_v1/page/summary/Grain_boundary"));
        QCOMPARE(found.first().at(0).value<WikipediaSummary>().title, QStringLiteral("Grain boundary"));
        QCOMPARE(found.first().at(1).value<QList<WikipediaPage>>().size(), 1);
    }

    void aNewLookupDropsTheOldOnesAnswer()
    {
        m_asked->answers << qMakePair(QStringLiteral("q=first"), ok(kSearch))
                         << qMakePair(QStringLiteral("/page/summary/"), ok(kSummary));
        FakeTransport transport(m_asked);
        SelectionLookup lookup(transport);
        QSignalSpy found(&lookup, &SelectionLookup::wikipediaFound);
        QSignalSpy nothing(&lookup, &SelectionLookup::nothingFound);
        lookup.lookUpOnWikipedia(QStringLiteral("first"), QStringLiteral("en"));
        lookup.lookUpOnWikipedia(QStringLiteral("second"), QStringLiteral("en"));
        QVERIFY(nothing.wait());
        QTest::qWait(50);
        QVERIFY(found.isEmpty());
        QCOMPARE(m_asked->urls.size(), 2); // The first's best match was never fetched.
    }

    void problemsAreReported()
    {
        TransportReply tooMany;
        tooMany.status = 429;
        m_asked->answers << qMakePair(QStringLiteral("wiktionary"), tooMany);
        FakeTransport transport(m_asked);
        SelectionLookup lookup(transport);
        QSignalSpy failed(&lookup, &SelectionLookup::failed);
        lookup.lookUpOnWiktionary(QStringLiteral("lattice"), QStringLiteral("en"));
        QVERIFY(failed.wait());
        QCOMPARE(failed.first().at(0).value<SelectionLookup::Source>(), SelectionLookup::Source::Wiktionary);
        QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("too many")));

        QSignalSpy nothing(&lookup, &SelectionLookup::nothingFound);
        lookup.lookUpOnWikipedia(QStringLiteral("nothing here"), QStringLiteral("en")); // 404
        QVERIFY(nothing.wait());
    }

    // ---- The library ----

    void theLibrarysMentionsAreFound()
    {
        const QString path = m_dir->filePath(QStringLiteral("library.db"));
        Database db(path);
        migrateLibrary(db);
        attachFullText(db, fullTextPathFor(path));
        PublicationRepository publications(db);
        FileRepository files(db);
        FullTextIndex index(db);
        const qint64 root = files.addRoot(QStringLiteral("Tests"), m_dir->filePath(QStringLiteral("docs")));
        int number = 0;
        auto add = [&](const QString &title, const QStringList &pages) {
            Publication p;
            p.title = title;
            p.year = 2001;
            const qint64 id = publications.create(p);
            FileRecord file;
            file.rootId = root;
            file.relativePath = QStringLiteral("file%1.pdf").arg(++number);
            file.format = QStringLiteral("pdf");
            file.sha256 = QStringLiteral("hash%1").arg(number);
            files.link(id, files.upsert(file), true);
            const qint64 document = index.beginDocument(file.sha256);
            index.addPages(document, 0, pages);
            index.finishDocument(document, int(pages.size()), QString());
            return id;
        };
        const qint64 reading = add(QStringLiteral("The paper being read"),
                                   {QStringLiteral("nothing"), QStringLiteral("where each grain boundary meets another")});
        const qint64 other = add(QStringLiteral("Another made-up paper"), {QStringLiteral("a grain boundary on its first page")});
        add(QStringLiteral("Grain boundary in its title"), {QStringLiteral("no such words on its pages")});
        add(QStringLiteral("Unrelated"), {QStringLiteral("grain and boundary, but not together")});

        Annotation note;
        note.publicationId = other;
        note.contentHash = QStringLiteral("hash2");
        AnnotationPart part;
        part.page = 0;
        part.text = QStringLiteral("a grain boundary");
        note.parts = {part};
        note.note = QStringLiteral("a note of mine");
        AnnotationRepository(db).create(note);

        const Mentions mentions = mentionsOf(db, QStringLiteral("grain boundary"), reading);
        QCOMPARE(mentions.publications, 3);
        QVERIFY(mentions.found.size() >= 4);
        // What's in the document being read comes first.
        QCOMPARE(mentions.found.first().kind, Mention::Kind::Page);
        QVERIFY(mentions.found.first().thisDocument);
        QCOMPARE(mentions.found.first().page, 1);
        bool otherPage = false, highlight = false, details = false;
        for (const Mention &mention : mentions.found) {
            otherPage = otherPage || (mention.kind == Mention::Kind::Page && mention.publicationId == other && mention.page == 0);
            highlight = highlight || (mention.kind == Mention::Kind::Highlight && mention.snippet.contains(QStringLiteral("a note of mine")));
            details = details || (mention.kind == Mention::Kind::Details && mention.title == QStringLiteral("Grain boundary in its title"));
            QVERIFY(mention.title != QStringLiteral("Unrelated"));
        }
        QVERIFY(otherPage);
        QVERIFY(highlight);
        QVERIFY(details);

        // Quotes in the words can't change the search.
        QVERIFY(mentionsOf(db, QStringLiteral("\"grain\" OR \"x"), reading).found.isEmpty());
        QVERIFY(mentionsOf(db, QString(), reading).found.isEmpty());
    }

    // ---- The panel ----

    void nothingGoesOnlineWhileOffline()
    {
        openPanel(false);
        core::Publication mentioning;
        mentioning.title = QStringLiteral("Pretend Lattice Vibrations");
        mentioning.year = 2021;
        m_session->publications().create(mentioning);
        m_panel->lookUp(QStringLiteral("lattice"));
        QSignalSpy shown(m_panel.get(), &LookUpPanel::libraryShown);
        QVERIFY(shown.wait());
        // Each place is shown as where it is, then what's there, drawn apart so one runs into the next less.
        auto *places = m_panel->findChild<QListWidget *>(QStringLiteral("lookUpLibrary"));
        QCOMPARE(places->count(), 1);
        QVERIFY(qobject_cast<ArticleDelegate *>(places->itemDelegate()));
        QCOMPARE(places->item(0)->data(ArticleDelegate::TitleRole).toString(), QStringLiteral("Pretend Lattice Vibrations (2021)"));
        QCOMPARE(places->item(0)->data(ArticleDelegate::PagesRole).toString(), QStringLiteral("In its details or notes"));
        QVERIFY(!button(QStringLiteral("lookUpWikipedia"))->isVisibleTo(m_panel.get()));
        QVERIFY(!button(QStringLiteral("lookUpWiktionary"))->isVisibleTo(m_panel.get()));
        QVERIFY(!m_panel->findChild<QCheckBox *>(QStringLiteral("lookUpAlways"))->isEnabled());

        // Even with Always set, as an imported library could have it.
        m_session->settings().setValue(QStringLiteral("lookup.online"), true);
        m_panel.reset();
        m_panel = std::make_unique<LookUpPanel>(*m_session, 0);
        m_panel->lookUp(QStringLiteral("lattice"));
        QTest::qWait(100);
        QVERIFY(m_asked->urls.isEmpty());

        // A mode chosen as offline is the same.
        m_session->settings().setValue(QStringLiteral("metadata.mode_chosen"), true);
        m_session->settings().setValue(QStringLiteral("metadata.mode"), QStringLiteral("offline"));
        m_panel->lookUp(QStringLiteral("lattice"));
        QTest::qWait(100);
        QVERIFY(m_asked->urls.isEmpty());
    }

    void onlineSourcesAreOnlyAskedWhenChosen()
    {
        m_asked->answers << qMakePair(QStringLiteral("/search/page"), ok(kSearch))
                         << qMakePair(QStringLiteral("/page/summary/"), ok(kSummary))
                         << qMakePair(QStringLiteral("wiktionary.org/api"), ok(kDefinition));
        openPanel(true);
        m_panel->lookUp(QStringLiteral("lattice"));
        QTest::qWait(100);
        QVERIFY(m_asked->urls.isEmpty()); // Not until asked.

        QPushButton *wikipedia = button(QStringLiteral("lookUpWikipedia"));
        QVERIFY(wikipedia->isVisibleTo(m_panel.get()));
        wikipedia->click();
        QTRY_COMPARE(m_asked->urls.size(), 2);
        QVERIFY(m_asked->urls.at(0).contains(QStringLiteral("q=lattice")));
        auto *extract = m_panel->findChild<QLabel *>(QStringLiteral("lookUpWikipediaExtract"));
        QTRY_VERIFY(extract->text().contains(QStringLiteral("made-up summary")));
        QCOMPARE(extract->textFormat(), Qt::PlainText);
        // The other articles are drawn as the library's results are, a title with what it is beneath, and the list is
        // tall enough to show them whole.
        auto *others = m_panel->findChild<QListWidget *>(QStringLiteral("lookUpWikipediaOthers"));
        QVERIFY(others);
        QTRY_VERIFY(others->isVisibleTo(m_panel.get()));
        QVERIFY(qobject_cast<ArticleDelegate *>(others->itemDelegate()));
        QVERIFY(others->count() > 0);
        for (int row = 0; row < others->count(); ++row)
            QVERIFY(!others->item(row)->data(ArticleDelegate::TitleRole).toString().isEmpty());
        // The article shown isn't listed again.
        QCOMPARE(others->item(0)->data(ArticleDelegate::TitleRole).toString(), QStringLiteral("Grain boundary strengthening"));
        QVERIFY(others->height() >= others->sizeHintForRow(0) * others->count());
        auto *description = m_panel->findChild<QLabel *>(QStringLiteral("lookUpWikipediaDescription"));
        QCOMPARE(description->text(), QStringLiteral("Interface between crystallites"));
        QCOMPARE(description->foregroundRole(), QPalette::PlaceholderText);

        button(QStringLiteral("lookUpWiktionary"))->click();
        auto *definitions = m_panel->findChild<QLabel *>(QStringLiteral("lookUpWiktionaryText"));
        QTRY_VERIFY(definitions->text().contains(QStringLiteral("A frame of crossed strips & bars.")));
        QCOMPARE(m_asked->urls.last(), QStringLiteral("https://en.wiktionary.org/api/rest_v1/page/definition/lattice"));

        // With Always, both are asked as soon as something's looked up.
        m_asked->urls.clear();
        m_panel->findChild<QCheckBox *>(QStringLiteral("lookUpAlways"))->setChecked(true);
        QVERIFY(m_session->settings().value(QStringLiteral("lookup.online")).toBool());
        m_asked->urls.clear();
        m_panel->lookUp(QStringLiteral("grain"));
        QTRY_COMPARE(m_asked->urls.size(), 3);

        // Wiktionary isn't asked about more than a short phrase.
        m_asked->urls.clear();
        m_panel->lookUp(QStringLiteral("where each grain boundary meets"));
        QTRY_COMPARE(m_asked->urls.size(), 2);
        for (const QString &url : m_asked->urls)
            QVERIFY(!url.contains(QStringLiteral("wiktionary")));
    }

    void theLinksOpenedAreBuiltHere()
    {
        m_asked->answers << qMakePair(QStringLiteral("/search/page"), ok(kSearch))
                         << qMakePair(QStringLiteral("/page/summary/"), ok(kSummary));
        openPanel(true);
        m_session->settings().setValue(QStringLiteral("lookup.language"), QStringLiteral("de"));
        QSignalSpy links(m_panel.get(), &LookUpPanel::openLinkWanted);
        m_panel->lookUp(QStringLiteral("Korngrenze"));
        button(QStringLiteral("lookUpWikipedia"))->click();
        QTRY_VERIFY(m_asked->urls.size() == 2);
        QVERIFY(m_asked->urls.first().startsWith(QStringLiteral("https://de.wikipedia.org/")));
        QPushButton *read = nullptr;
        for (QPushButton *b : m_panel->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("Read on Wikipedia"))
                read = b;
        QVERIFY(read);
        QTRY_VERIFY(read->isVisibleTo(m_panel.get()));
        read->click();
        QCOMPARE(links.size(), 1);
        QCOMPARE(links.first().at(0).toUrl(), QUrl(QStringLiteral("https://de.wikipedia.org/wiki/Grain_boundary")));

        // A language setting that isn't a language falls back to the window's.
        m_session->settings().setValue(QStringLiteral("lookup.language"), QStringLiteral("x.evil"));
        m_asked->urls.clear();
        m_panel->lookUp(QStringLiteral("lattice"));
        button(QStringLiteral("lookUpWikipedia"))->click();
        QTRY_VERIFY(!m_asked->urls.isEmpty());
        QVERIFY(QUrl(m_asked->urls.first()).host().endsWith(QStringLiteral(".wikipedia.org")));
        QVERIFY(!m_asked->urls.first().contains(QStringLiteral("evil")));
    }

private:
    void openPanel(bool online)
    {
        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("window/library.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("window/cache"));
        paths.settleMs = 0;
        auto asked = m_asked;
        paths.transportFactory = [asked] { return std::unique_ptr<Transport>(std::make_unique<FakeTransport>(asked)); };
        m_session = std::make_unique<LibrarySession>(paths);
        if (online) {
            m_session->settings().setValue(QStringLiteral("metadata.mode_chosen"), true);
            m_session->settings().setValue(QStringLiteral("metadata.mode"), QStringLiteral("online_review"));
        }
        m_panel = std::make_unique<LookUpPanel>(*m_session, 0);
        m_panel->show();
    }

    QPushButton *button(const QString &name) const
    {
        auto *found = m_panel->findChild<QPushButton *>(name);
        if (!found)
            qFatal("no button %s", qPrintable(name));
        return found;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::shared_ptr<Asked> m_asked;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<LookUpPanel> m_panel;
};

QTEST_MAIN(SelectionLookupTest)
#include "test_selection_lookup.moc"
