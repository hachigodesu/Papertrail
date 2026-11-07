#include "citation/CitationItem.h"
#include "citation/Exporters.h"
#include "citation/Formatting.h"
#include "citation/Styles.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail;
using namespace papertrail::citation;
using core::PersonName;
using core::Publication;
using core::PublicationType;

namespace {

const QDate kAccessed(2026, 11, 7);

Publication publication(PublicationType type, const QString &title)
{
    Publication p;
    p.type = type;
    p.title = title;
    p.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return p;
}

// Every kind of publication, all made up, by the names the fixture files use.
QList<std::pair<QString, CitationItem>> fixtureItems()
{
    QList<std::pair<QString, CitationItem>> items;

    Publication article = publication(PublicationType::JournalArticle, QStringLiteral("Lattice Vibrations in Pretend Crystals"));
    article.authors = {{QStringLiteral("Example"), QStringLiteral("Ada Beatrice")}, {QStringLiteral("Sample"), QStringLiteral("Ben")}};
    article.containerTitle = QStringLiteral("Journal of Imaginary Findings");
    article.volume = QStringLiteral("3");
    article.issue = QStringLiteral("2");
    article.pages = QStringLiteral("43-70");
    article.year = 2021;
    article.month = 5;
    article.doi = QStringLiteral("10.5555/jif.2021.0002");
    items << std::make_pair(QStringLiteral("article"), citationItem(article));

    Publication print = publication(PublicationType::JournalArticle, QStringLiteral("Why Do Models Fail?"));
    print.authors = {{QStringLiteral("Dent"), QStringLiteral("Carla")}};
    print.containerTitle = QStringLiteral("Made-Up Review");
    print.volume = QStringLiteral("12");
    print.pages = QStringLiteral("101–108");
    print.year = 2019;
    items << std::make_pair(QStringLiteral("article-print"), citationItem(print));

    Publication seven = publication(PublicationType::JournalArticle, QStringLiteral("Collaborative Pretend Measurements"));
    seven.authors = {{QStringLiteral("Alpha"), QStringLiteral("Anna")},   {QStringLiteral("Beta"), QStringLiteral("Bo")},
                     {QStringLiteral("Gamma"), QStringLiteral("Cy")},     {QStringLiteral("Delta"), QStringLiteral("Di")},
                     {QStringLiteral("Epsilon"), QStringLiteral("Ed")},   {QStringLiteral("Zeta"), QStringLiteral("Flo")},
                     {QStringLiteral("Eta"), QStringLiteral("Gus")}};
    seven.containerTitle = QStringLiteral("Journal of Imaginary Findings");
    seven.volume = QStringLiteral("4");
    seven.issue = QStringLiteral("1");
    seven.pages = QStringLiteral("1496-1500");
    seven.year = 2022;
    seven.url = QStringLiteral("https://example.org/paper");
    items << std::make_pair(QStringLiteral("article-seven-authors"), citationItem(seven));

    Publication editorial = publication(PublicationType::JournalArticle, QStringLiteral("Editorial"));
    editorial.containerTitle = QStringLiteral("Pretend Letters");
    editorial.volume = QStringLiteral("1");
    editorial.issue = QStringLiteral("1");
    editorial.pages = QStringLiteral("1");
    editorial.year = 2020;
    items << std::make_pair(QStringLiteral("article-no-author"), citationItem(editorial));

    Publication book = publication(PublicationType::Book, QStringLiteral("Imaginary Structures"));
    book.subtitle = QStringLiteral("A Practical Guide");
    book.authors = {{QStringLiteral("Fiction"), QStringLiteral("Dana")}};
    book.edition = QStringLiteral("3");
    book.publisher = QStringLiteral("Example Press");
    book.place = QStringLiteral("Bristol");
    book.year = 2018;
    book.isbn13 = QStringLiteral("9780306406157");
    items << std::make_pair(QStringLiteral("book"), citationItem(book));

    Publication edited = publication(PublicationType::Book, QStringLiteral("Collected Pretend Essays"));
    edited.editors = {{QStringLiteral("Editor"), QStringLiteral("Eve")}, {QStringLiteral("Compiler"), QStringLiteral("Frank")}};
    edited.publisher = QStringLiteral("Example Press");
    edited.year = 2015;
    items << std::make_pair(QStringLiteral("edited-book"), citationItem(edited));

    // A chapter takes its book's title, editors, publisher and place from the book it's in.
    Publication holder = publication(PublicationType::Book, QStringLiteral("Collected Pretend Essays"));
    holder.editors = {{QStringLiteral("Editor"), QStringLiteral("Eve")}};
    holder.publisher = QStringLiteral("Example Press");
    holder.place = QStringLiteral("Leeds");
    holder.year = 2015;
    Publication chapter = publication(PublicationType::BookChapter, QStringLiteral("A Chapter on Nothing"));
    chapter.authors = {{QStringLiteral("Writer"), QStringLiteral("Gina")}};
    chapter.pages = QStringLiteral("37-49");
    chapter.parentId = 1;
    items << std::make_pair(QStringLiteral("chapter"), citationItem(chapter, holder));

    Publication paper = publication(PublicationType::ConferencePaper, QStringLiteral("Fast Pretend Sorting"));
    paper.authors = {{QStringLiteral("Tanaka"), QStringLiteral("Hiro")},
                     {QStringLiteral("Garcia"), QStringLiteral("Ines")},
                     {QStringLiteral("Park"), QStringLiteral("Jo")}};
    paper.containerTitle = QStringLiteral("Proceedings of the Made-Up Conference on Examples");
    paper.conferenceLocation = QStringLiteral("Lisbon, Portugal");
    paper.publisher = QStringLiteral("Example Society");
    paper.pages = QStringLiteral("195-203");
    paper.year = 2016;
    paper.doi = QStringLiteral("10.5555/mce.2016.17");
    items << std::make_pair(QStringLiteral("paper"), citationItem(paper));

    Publication proceedings = publication(PublicationType::ConferenceProceedings,
                                          QStringLiteral("Proceedings of the Made-Up Conference on Examples"));
    proceedings.editors = {{QStringLiteral("Lee"), QStringLiteral("Kim")}};
    proceedings.conferenceLocation = QStringLiteral("Lisbon, Portugal");
    proceedings.publisher = QStringLiteral("Example Society");
    proceedings.place = QStringLiteral("Paris");
    proceedings.year = 2016;
    items << std::make_pair(QStringLiteral("proceedings"), citationItem(proceedings));

    Publication issue = publication(PublicationType::JournalIssue, QStringLiteral("Journal of Imaginary Findings"));
    issue.volume = QStringLiteral("3");
    issue.issue = QStringLiteral("2");
    issue.month = 5;
    issue.year = 2021;
    issue.publisher = QStringLiteral("Example Press");
    items << std::make_pair(QStringLiteral("issue"), citationItem(issue));

    Publication thesis = publication(PublicationType::Thesis, QStringLiteral("Pretend Materials Under Stress"));
    thesis.authors = {{QStringLiteral("O'Neill"), QStringLiteral("Liam")}};
    thesis.degree = QStringLiteral("PhD");
    thesis.institution = QStringLiteral("University of Examples");
    thesis.year = 2020;
    thesis.url = QStringLiteral("https://repository.example.org/123");
    items << std::make_pair(QStringLiteral("thesis"), citationItem(thesis));

    Publication dissertation = publication(PublicationType::Dissertation, QStringLiteral("Pretend Materials in Practice"));
    dissertation.authors = {{QStringLiteral("Novak"), QStringLiteral("Mia")}};
    dissertation.degree = QStringLiteral("MSc");
    dissertation.institution = QStringLiteral("Example Institute");
    dissertation.year = 2017;
    items << std::make_pair(QStringLiteral("dissertation"), citationItem(dissertation));

    Publication report = publication(PublicationType::TechnicalReport, QStringLiteral("Annual Report on Pretend Data"));
    report.publisher = QStringLiteral("Example Agency");
    report.place = QStringLiteral("London");
    report.reportNumber = QStringLiteral("EA-2020-04");
    report.year = 2020;
    report.url = QStringLiteral("https://example.org/report.pdf");
    items << std::make_pair(QStringLiteral("report"), citationItem(report));

    Publication standard = publication(PublicationType::Standard, QStringLiteral("Specification for Pretend Bolts"));
    standard.standardBody = QStringLiteral("Example Standards Body");
    standard.standardNumber = QStringLiteral("ESB 1234");
    standard.publisher = QStringLiteral("Example Standards Body");
    standard.year = 2019;
    items << std::make_pair(QStringLiteral("standard"), citationItem(standard));

    Publication preprint = publication(PublicationType::Preprint, QStringLiteral("Sampling Made Simple"));
    preprint.authors = {{QStringLiteral("Haddad"), QStringLiteral("Noor")}};
    preprint.arxivId = QStringLiteral("2101.00001");
    preprint.year = 2021;
    preprint.month = 1;
    items << std::make_pair(QStringLiteral("preprint"), citationItem(preprint));

    Publication document = publication(PublicationType::Other, QStringLiteral("Guidance on Pretend Things"));
    document.authors = {{QStringLiteral("Example Charity"), QString()}};
    document.url = QStringLiteral("https://example.org/guidance");
    items << std::make_pair(QStringLiteral("document"), citationItem(document));
    return items;
}

// Italics shown between asterisks, as the fixture files write them.
QString marked(const RichText &text)
{
    QString out;
    for (const RichText::Run &run : text.runs())
        out += run.italic ? u'*' + run.text + u'*' : run.text;
    return out.simplified().replace(QStringLiteral(" *"), QStringLiteral(" *"));
}

struct Expected {
    QString reference;
    QString citation;
};

QHash<QString, Expected> readFixtures(const QString &style)
{
    QFile file(QStringLiteral(PAPERTRAIL_TEST_FIXTURES "/citations/") + style + QStringLiteral(".txt"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QHash<QString, Expected> fixtures;
    QString current;
    for (const QString &line : QString::fromUtf8(file.readAll()).split(u'\n')) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(u'[') && trimmed.endsWith(u']'))
            current = trimmed.mid(1, trimmed.size() - 2);
        else if (trimmed.startsWith(QStringLiteral("ref: ")))
            fixtures[current].reference = trimmed.mid(5);
        else if (trimmed.startsWith(QStringLiteral("cite: ")))
            fixtures[current].citation = trimmed.mid(6);
    }
    return fixtures;
}

CitationItem simpleItem(const QString &family, const QString &title, int year)
{
    Publication p = publication(PublicationType::JournalArticle, title);
    p.authors = {{family, QStringLiteral("Pat")}};
    p.containerTitle = QStringLiteral("Pretend Letters");
    p.year = year;
    return citationItem(p);
}

} // namespace

class CitationsTest : public QObject {
    Q_OBJECT

private slots:
    // ---- Every kind in every style ----
    void eachStyleMatchesItsFixtures_data()
    {
        QTest::addColumn<int>("style");
        QTest::addColumn<QString>("name");
        for (const CitationStyle style : allCitationStyles()) {
            for (const auto &[name, item] : fixtureItems())
                QTest::newRow(qPrintable(styleKey(style) + u'/' + name)) << int(style) << name;
        }
    }

    void eachStyleMatchesItsFixtures()
    {
        QFETCH(int, style);
        QFETCH(QString, name);
        const CitationStyle chosen = CitationStyle(style);
        const QHash<QString, Expected> fixtures = readFixtures(styleKey(chosen));
        QVERIFY2(fixtures.contains(name), qPrintable(QStringLiteral("no fixture for ") + name));

        CitationItem item;
        for (const auto &[fixtureName, fixtureItem] : fixtureItems()) {
            if (fixtureName == name)
                item = fixtureItem;
        }
        StyleOptions options;
        options.accessed = kAccessed;
        const FormattedReferences formatted = formatReferences({item}, chosen, options);
        QCOMPARE(formatted.entries.size(), 1);
        QCOMPARE(marked(formatted.entries.first()), fixtures.value(name).reference);
        QCOMPARE(marked(formatted.inText.first()), fixtures.value(name).citation);
    }

    void everyKindHasAFixtureInEveryStyle()
    {
        for (const CitationStyle style : allCitationStyles()) {
            const QHash<QString, Expected> fixtures = readFixtures(styleKey(style));
            QSet<CitationItem::Kind> kinds;
            for (const auto &[name, item] : fixtureItems()) {
                QVERIFY2(fixtures.contains(name), qPrintable(styleKey(style) + QStringLiteral(" has no ") + name));
                kinds.insert(item.kind);
            }
            QCOMPARE(kinds.size(), 12);
        }
    }

    // ---- Names, pages and editions ----
    void initialsAreTakenFromGivenNames_data()
    {
        QTest::addColumn<QString>("given");
        QTest::addColumn<QString>("joined");
        QTest::addColumn<QString>("spaced");
        QTest::newRow("one") << "Ada" << "A." << "A.";
        QTest::newRow("two") << "Ada Beatrice" << "A.B." << "A. B.";
        QTest::newRow("already initials") << "J. R. R." << "J.R.R." << "J. R. R.";
        QTest::newRow("run together") << "J.R.R." << "J.R.R." << "J. R. R.";
        QTest::newRow("hyphenated") << "Hans-Peter" << "H.-P." << "H.-P.";
        QTest::newRow("lowercase") << "jean-luc" << "J.-L." << "J.-L.";
        QTest::newRow("accented") << "Émile" << "É." << "É.";
        QTest::newRow("none") << "" << "" << "";
    }

    void initialsAreTakenFromGivenNames()
    {
        QFETCH(QString, given);
        QFETCH(QString, joined);
        QFETCH(QString, spaced);
        QCOMPARE(initials(given, false), joined);
        QCOMPARE(initials(given, true), spaced);
    }

    void namesInOtherScriptsAreWrittenWhole()
    {
        Publication p = publication(PublicationType::Book, QStringLiteral("A Made-Up Book"));
        p.authors = {{QStringLiteral("山田"), QStringLiteral("太郎")}};
        p.year = 2020;
        p.publisher = QStringLiteral("Example Press");
        const CitationItem item = citationItem(p);
        StyleOptions options;
        options.accessed = kAccessed;
        QVERIFY(marked(formatReferences({item}, CitationStyle::Harvard, options).entries.first()).startsWith(QStringLiteral("山田太郎, 2020.")));
        QCOMPARE(marked(formatReferences({item}, CitationStyle::Harvard, options).inText.first()), QStringLiteral("(山田太郎 2020)"));
        QVERIFY(marked(formatReferences({item}, CitationStyle::Ieee).entries.first()).startsWith(QStringLiteral("[1] 山田太郎,")));
    }

    void chicagoShortensPageRanges_data()
    {
        QTest::addColumn<QString>("pages");
        QTest::addColumn<QString>("shortened");
        QTest::newRow("under 100") << "3-10" << "3–10";
        QTest::newRow("under 100, close") << "71-72" << "71–72";
        QTest::newRow("across 100") << "96-117" << "96–117";
        QTest::newRow("hundreds") << "100-104" << "100–104";
        QTest::newRow("thousands") << "1100-1113" << "1100–1113";
        QTest::newRow("101 to 109") << "101-108" << "101–8";
        QTest::newRow("808") << "808-33" << "808–33";
        QTest::newRow("808 written in full") << "808-833" << "808–33";
        QTest::newRow("1103") << "1103-1104" << "1103–4";
        QTest::newRow("321") << "321-328" << "321–28";
        QTest::newRow("498") << "498-532" << "498–532";
        QTest::newRow("1087") << "1087-1089" << "1087–89";
        QTest::newRow("1496") << "1496-1500" << "1496–500";
        QTest::newRow("11564") << "11564-11615" << "11564–615";
        QTest::newRow("12991") << "12991-13001" << "12991–3001";
        QTest::newRow("en dash") << "43–70" << "43–70";
        QTest::newRow("single") << "e1234" << "e1234";
        QTest::newRow("words") << "pp. 1, 3, 5" << "pp. 1, 3, 5";
    }

    void chicagoShortensPageRanges()
    {
        QFETCH(QString, pages);
        QFETCH(QString, shortened);
        // A range written short, such as 808-33, is put back in full first, then shortened by the rules.
        const std::optional<PageRange> range = pageRange(pages);
        QString input = pages;
        if (range && range->isRange() && range->last.size() < range->first.size())
            input = range->first + u'-' + range->first.left(range->first.size() - range->last.size()) + range->last;
        QCOMPARE(chicagoPages(input), shortened);
    }

    void editionsAreOnlyGivenAfterTheFirst_data()
    {
        QTest::addColumn<QString>("edition");
        QTest::addColumn<QString>("text");
        QTest::newRow("none") << "" << "";
        QTest::newRow("first") << "1" << "";
        QTest::newRow("1st ed.") << "1st ed." << "";
        QTest::newRow("second") << "2" << "2nd ed.";
        QTest::newRow("third edition") << "3rd edition" << "3rd ed.";
        QTest::newRow("eleventh") << "11" << "11th ed.";
        QTest::newRow("twenty-first") << "21" << "21st ed.";
        QTest::newRow("words") << "Revised edition" << "Revised edition";
        QTest::newRow("other words") << "Second" << "Second ed.";
    }

    void editionsAreOnlyGivenAfterTheFirst()
    {
        QFETCH(QString, edition);
        QFETCH(QString, text);
        QCOMPARE(editionText(edition, QStringLiteral("ed.")), text);
    }

    // ---- Lists ----
    void worksBySomeoneInOneYearGetLetters()
    {
        const QList<CitationItem> items = {simpleItem(QStringLiteral("Dent"), QStringLiteral("Second Title"), 2019),
                                           simpleItem(QStringLiteral("Dent"), QStringLiteral("First Title"), 2019),
                                           simpleItem(QStringLiteral("Dent"), QStringLiteral("Other Year"), 2020)};
        for (const CitationStyle style : {CitationStyle::Harvard, CitationStyle::Apa, CitationStyle::Chicago}) {
            const FormattedReferences formatted = formatReferences(items, style);
            QCOMPARE(formatted.order, (QList<int>{1, 0, 2}));
            QVERIFY2(formatted.entries.at(0).plain().contains(QStringLiteral("2019a")), qPrintable(formatted.entries.at(0).plain()));
            QVERIFY(formatted.entries.at(1).plain().contains(QStringLiteral("2019b")));
            QVERIFY(!formatted.entries.at(2).plain().contains(QStringLiteral("2020a")));
            QVERIFY(formatted.inText.at(1).plain().contains(QStringLiteral("2019a")));
            QVERIFY(formatted.inText.at(0).plain().contains(QStringLiteral("2019b")));
        }
        // IEEE numbers them, in the order given.
        const FormattedReferences ieee = formatReferences(items, CitationStyle::Ieee);
        QCOMPARE(ieee.order, (QList<int>{0, 1, 2}));
        QVERIFY(ieee.entries.at(1).plain().startsWith(QStringLiteral("[2] ")));
        QVERIFY(!ieee.entries.at(0).plain().contains(QStringLiteral("2019a")));
    }

    void theSameWorkTwiceGetsNoLetters()
    {
        const CitationItem item = simpleItem(QStringLiteral("Dent"), QStringLiteral("Only Title"), 2019);
        const FormattedReferences formatted = formatReferences({item, item}, CitationStyle::Harvard);
        QVERIFY(!formatted.entries.first().plain().contains(QStringLiteral("2019a")));
    }

    void authorDateListsAreAlphabetical()
    {
        CitationItem anonymous = simpleItem(QStringLiteral("x"), QStringLiteral("Unsigned"), 2020);
        anonymous.authors.clear();
        const QList<CitationItem> items = {simpleItem(QStringLiteral("Novak"), QStringLiteral("N"), 2017),
                                           simpleItem(QStringLiteral("Écrivain"), QStringLiteral("E"), 2018),
                                           simpleItem(QStringLiteral("dent"), QStringLiteral("D"), 2019), anonymous};
        // Harvard files the anonymous under "Anon.", and accents and case don't affect the order.
        QCOMPARE(formatReferences(items, CitationStyle::Harvard).order, (QList<int>{3, 2, 1, 0}));
        // APA and Chicago file it by its title.
        QCOMPARE(formatReferences(items, CitationStyle::Apa).order, (QList<int>{2, 1, 0, 3}));
    }

    void severalCitationsGoTogether()
    {
        const QList<CitationItem> items = {simpleItem(QStringLiteral("Novak"), QStringLiteral("N"), 2017),
                                           simpleItem(QStringLiteral("Dent"), QStringLiteral("D"), 2019)};
        QCOMPARE(combinedInTextCitation(items, CitationStyle::Harvard).plain(), QStringLiteral("(Dent 2019; Novak 2017)"));
        QCOMPARE(combinedInTextCitation(items, CitationStyle::Apa).plain(), QStringLiteral("(Dent, 2019; Novak, 2017)"));
        QCOMPARE(combinedInTextCitation(items, CitationStyle::Chicago).plain(), QStringLiteral("(Dent 2019; Novak 2017)"));
        QCOMPARE(combinedInTextCitation(items, CitationStyle::Ieee).plain(), QStringLiteral("[1], [2]"));
        QCOMPARE(combinedInTextCitation(items + items.mid(0, 1), CitationStyle::Ieee).plain(), QStringLiteral("[1]–[3]"));
        QVERIFY(combinedInTextCitation({}, CitationStyle::Harvard).isEmpty());
    }

    void harvardDatesWhenItWasAccessed()
    {
        StyleOptions options;
        options.accessed = QDate(2027, 1, 31);
        const CitationItem item = fixtureItems().first().second;
        QVERIFY(formatReferences({item}, CitationStyle::Harvard, options).entries.first().plain().endsWith(
            QStringLiteral("[Accessed 31 January 2027].")));
    }

    void htmlIsEscapedAndItalicised()
    {
        Publication p = publication(PublicationType::Book, QStringLiteral("Pretend <b>Tags</b> & Things"));
        p.authors = {{QStringLiteral("Dent"), QStringLiteral("Carla")}};
        p.year = 2019;
        const QString html = formatReferences({citationItem(p)}, CitationStyle::Harvard).entries.first().html();
        QVERIFY2(html.contains(QStringLiteral("<i>Pretend &lt;b&gt;Tags&lt;/b&gt; &amp; Things</i>")), qPrintable(html));
        QVERIFY(!html.contains(QStringLiteral("<b>")));
    }

    void stylesHaveKeysAndNames()
    {
        for (const CitationStyle style : allCitationStyles()) {
            QCOMPARE(styleFromKey(styleKey(style)), std::optional<CitationStyle>(style));
            QVERIFY(!styleName(style).isEmpty());
        }
        QVERIFY(!styleFromKey(QStringLiteral("nonsense")));
    }

    // ---- What an article takes from what it's in ----
    void anArticleTakesWhatItLacksFromItsHolder()
    {
        Publication holder = publication(PublicationType::JournalIssue, QStringLiteral("An Issue's Own Title"));
        holder.containerTitle = QStringLiteral("Journal of Imaginary Findings");
        holder.volume = QStringLiteral("7");
        holder.issue = QStringLiteral("11");
        holder.year = 2026;
        holder.month = 11;
        holder.editors = {{QStringLiteral("Guest"), QStringLiteral("Ed")}};
        Publication article = publication(PublicationType::JournalArticle, QStringLiteral("Inside"));
        article.parentId = 1;
        article.pageStart = 5; // A position in the file, which is never a citation's pages.
        article.pageEnd = 9;
        const CitationItem item = citationItem(article, holder);
        QCOMPARE(item.containerTitle, QStringLiteral("Journal of Imaginary Findings"));
        QCOMPARE(item.volume, QStringLiteral("7"));
        QCOMPARE(item.issue, QStringLiteral("11"));
        QCOMPARE(item.year.value(), 2026);
        QCOMPARE(item.month.value(), 11);
        QVERIFY(item.pages.isEmpty());
        QVERIFY(item.editors.isEmpty()); // A journal's editors aren't cited.
    }

    void gapsInTheDetailsAreNoticed()
    {
        // Every made-up fixture with authors, a year and, if it's in something, what it's in, is complete.
        for (const auto &[name, item] : fixtureItems()) {
            const bool expected = name == QStringLiteral("article-no-author") || name == QStringLiteral("issue")
                || name == QStringLiteral("document");
            QVERIFY2(lacksDetails(item) == expected, qPrintable(name));
        }
        CitationItem noJournal = simpleItem(QStringLiteral("Dent"), QStringLiteral("A Title"), 2019);
        QVERIFY(!lacksDetails(noJournal));
        noJournal.containerTitle.clear();
        QVERIFY(lacksDetails(noJournal));
    }

    // ---- Exports ----
    void bibTeXHasAnEntryForEach()
    {
        QList<CitationItem> items;
        for (const auto &[name, item] : fixtureItems())
            items << item;
        const QString bib = QString::fromUtf8(toBibTeX(items));
        const QStringList keys = citationKeys(items);
        QCOMPARE(QSet<QString>(keys.cbegin(), keys.cend()).size(), keys.size());
        QCOMPARE(bib.count(QRegularExpression(QStringLiteral("^@\\w+\\{"), QRegularExpression::MultilineOption)), items.size());
        QCOMPARE(bib.count(u'{'), bib.count(u'}'));

        QVERIFY(bib.contains(QStringLiteral("@article{example2021lattice,")));
        QVERIFY(bib.contains(QStringLiteral("  author = {Example, Ada Beatrice and Sample, Ben},")));
        QVERIFY(bib.contains(QStringLiteral("  pages = {43--70},")));
        QVERIFY(bib.contains(QStringLiteral("  month = may,")));
        QVERIFY(bib.contains(QStringLiteral("  doi = {10.5555/jif.2021.0002}")));
        QVERIFY(bib.contains(QStringLiteral("@incollection{")));
        QVERIFY(bib.contains(QStringLiteral("  booktitle = {Collected Pretend Essays},")));
        QVERIFY(bib.contains(QStringLiteral("  editor = {Editor, Eve},")));
        QVERIFY(bib.contains(QStringLiteral("@inproceedings{")));
        QVERIFY(bib.contains(QStringLiteral("@phdthesis{oneill2020pretend,")));
        QVERIFY(bib.contains(QStringLiteral("@mastersthesis{")));
        QVERIFY(bib.contains(QStringLiteral("@techreport{")));
        QVERIFY(bib.contains(QStringLiteral("  number = {EA-2020-04},")));
        QVERIFY(bib.contains(QStringLiteral("  eprint = {2101.00001},")));
        QVERIFY(bib.contains(QStringLiteral("  author = {{Example Charity}},")));
    }

    void bibTeXEscapesAndProtects()
    {
        QCOMPARE(bibTeXText(QStringLiteral("50% of R&D_costs #1 {x} $5 ~ ^ \\")),
                 QStringLiteral("50\\% of R\\&D\\_costs \\#1 \\{x\\} \\$5 \\textasciitilde{} \\textasciicircum{} \\textbackslash{}"));
        QCOMPARE(bibTeXText(QStringLiteral("Measuring DNA with an iPhone in 3D: a Study"), true),
                 QStringLiteral("Measuring {DNA} with an {iPhone} in {3D}: a Study"));
        QCOMPARE(bibTeXText(QStringLiteral("(ROBDD)"), true), QStringLiteral("({ROBDD})"));
    }

    void keysStayUniqueAndPlain()
    {
        CitationItem accented = simpleItem(QStringLiteral("Müller"), QStringLiteral("The Über Study"), 2020);
        const QStringList keys = citationKeys({accented, accented, simpleItem(QStringLiteral("Dent"), QStringLiteral("On X"), 2019)});
        QCOMPARE(keys, (QStringList{QStringLiteral("muller2020uber") + u'a', QStringLiteral("muller2020uberb"), QStringLiteral("dent2019x")}));
    }

    void risHasAnEntryForEach()
    {
        QList<CitationItem> items;
        for (const auto &[name, item] : fixtureItems())
            items << item;
        items.first().keywords = {QStringLiteral("Physics > Crystals"), QStringLiteral("Reading")};
        const QString ris = QString::fromUtf8(toRis(items));
        QCOMPARE(ris.count(QStringLiteral("TY  - ")), items.size());
        QCOMPARE(ris.count(QStringLiteral("ER  - ")), items.size());
        QVERIFY(ris.startsWith(QStringLiteral("TY  - JOUR\r\nAU  - Example, Ada Beatrice\r\nAU  - Sample, Ben\r\n")));
        QVERIFY(ris.contains(QStringLiteral("SP  - 43\r\nEP  - 70\r\n")));
        QVERIFY(ris.contains(QStringLiteral("KW  - Physics > Crystals\r\nKW  - Reading\r\n")));
        QVERIFY(ris.contains(QStringLiteral("TY  - CHAP\r\n")));
        QVERIFY(ris.contains(QStringLiteral("ED  - Editor, Eve\r\n")));
        QVERIFY(ris.contains(QStringLiteral("TY  - THES\r\n")));
        QVERIFY(ris.contains(QStringLiteral("TY  - STAND\r\n")));
        QVERIFY(ris.contains(QStringLiteral("AU  - Example Charity\r\n")));
        QVERIFY(ris.contains(QStringLiteral("DA  - 2021/05//\r\n")));
        // Every line is a tag, two spaces, a hyphen and a space.
        static const QRegularExpression line(QStringLiteral("^[A-Z][A-Z0-9]  - .*$"));
        for (const QString &text : ris.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts))
            QVERIFY2(line.match(text).hasMatch(), qPrintable(text));
    }

    void cslJsonHasAnItemForEach()
    {
        QList<CitationItem> items;
        for (const auto &[name, item] : fixtureItems())
            items << item;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(toCslJson(items), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        const QJsonArray array = document.array();
        QCOMPARE(array.size(), items.size());
        const QJsonObject article = array.at(0).toObject();
        QCOMPARE(article.value(QStringLiteral("type")).toString(), QStringLiteral("article-journal"));
        QCOMPARE(article.value(QStringLiteral("id")).toString(), QStringLiteral("example2021lattice"));
        QCOMPARE(article.value(QStringLiteral("issued")).toObject().value(QStringLiteral("date-parts")).toArray(),
                 (QJsonArray{QJsonArray{2021, 5}}));
        QCOMPARE(article.value(QStringLiteral("author")).toArray().at(1).toObject().value(QStringLiteral("family")).toString(),
                 QStringLiteral("Sample"));
        QCOMPARE(article.value(QStringLiteral("page")).toString(), QStringLiteral("43-70"));
        QCOMPARE(article.value(QStringLiteral("DOI")).toString(), QStringLiteral("10.5555/jif.2021.0002"));

        QSet<QString> types;
        for (const QJsonValue &value : array)
            types.insert(value.toObject().value(QStringLiteral("type")).toString());
        for (const QString &type : {QStringLiteral("book"), QStringLiteral("chapter"), QStringLiteral("paper-conference"),
                                    QStringLiteral("thesis"), QStringLiteral("report"), QStringLiteral("standard"),
                                    QStringLiteral("article"), QStringLiteral("periodical"), QStringLiteral("document")})
            QVERIFY2(types.contains(type), qPrintable(type));
        const QJsonObject charity = array.last().toObject();
        QCOMPARE(charity.value(QStringLiteral("author")).toArray().at(0).toObject().value(QStringLiteral("literal")).toString(),
                 QStringLiteral("Example Charity"));
        QVERIFY(!charity.contains(QStringLiteral("issued")));
    }

    // ---- The real tools, where they're installed ----
    void bibTeXAndBiberReadTheExport()
    {
        const QString pdflatex = QStandardPaths::findExecutable(QStringLiteral("pdflatex"));
        const QString bibtex = QStandardPaths::findExecutable(QStringLiteral("bibtex"));
        const QString biber = QStandardPaths::findExecutable(QStringLiteral("biber"));
        if (pdflatex.isEmpty() || bibtex.isEmpty() || biber.isEmpty())
            QSKIP("pdflatex, bibtex and biber aren't all installed here");

        QList<CitationItem> items;
        for (const auto &[name, item] : fixtureItems())
            items << item;
        // Text that BibTeX finds awkward.
        CitationItem awkward = simpleItem(QStringLiteral("Müller"), QStringLiteral("Measuring DNA & RNA at 50% Cost: Café_Results #2"), 2024);
        awkward.authors << PersonName{QStringLiteral("Smith & Sons Ltd"), QString()};
        awkward.abstract = QStringLiteral("Braces { and } and a backslash \\ and ~tilde^caret.");
        awkward.keywords = {QStringLiteral("A > B"), QStringLiteral("C")};
        items << awkward;

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile bib(dir.filePath(QStringLiteral("refs.bib")));
        QVERIFY(bib.open(QIODevice::WriteOnly));
        bib.write(toBibTeX(items));
        bib.close();

        const auto run = [&](const QString &program, const QStringList &arguments, int &exitCode) {
            QProcess process;
            process.setWorkingDirectory(dir.path());
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(program, arguments);
            if (!process.waitForFinished(120000))
                return QStringLiteral("timed out");
            exitCode = process.exitCode();
            return QString::fromUtf8(process.readAll());
        };

        // Classic BibTeX, with the plain style.
        {
            QFile tex(dir.filePath(QStringLiteral("classic.tex")));
            QVERIFY(tex.open(QIODevice::WriteOnly));
            tex.write("\\documentclass{article}\n\\usepackage[T1]{fontenc}\n\\usepackage{url}\n\\begin{document}\n"
                      "\\nocite{*}\n\\bibliographystyle{plain}\n\\bibliography{refs}\n\\end{document}\n");
            tex.close();
            int code = -1;
            QString output = run(pdflatex, {QStringLiteral("-interaction=nonstopmode"), QStringLiteral("classic.tex")}, code);
            QCOMPARE(code, 0);
            output = run(bibtex, {QStringLiteral("classic")}, code);
            // 1 means warnings, such as a missing field, which aren't errors.
            QVERIFY2(code <= 1, qPrintable(output));
            QVERIFY2(!output.contains(QStringLiteral("error message")), qPrintable(output));
            output = run(pdflatex, {QStringLiteral("-interaction=nonstopmode"), QStringLiteral("classic.tex")}, code);
            QVERIFY2(code == 0, qPrintable(output.right(3000)));
            QFile bbl(dir.filePath(QStringLiteral("classic.bbl")));
            QVERIFY(bbl.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromUtf8(bbl.readAll()).count(QStringLiteral("\\bibitem")), items.size());
        }

        // biblatex with biber, which reads the newer fields too.
        {
            QFile tex(dir.filePath(QStringLiteral("modern.tex")));
            QVERIFY(tex.open(QIODevice::WriteOnly));
            tex.write("\\documentclass{article}\n\\usepackage[T1]{fontenc}\n\\usepackage[backend=biber]{biblatex}\n"
                      "\\addbibresource{refs.bib}\n\\begin{document}\n\\nocite{*}\n\\printbibliography\n\\end{document}\n");
            tex.close();
            int code = -1;
            QString output = run(pdflatex, {QStringLiteral("-interaction=nonstopmode"), QStringLiteral("modern.tex")}, code);
            QCOMPARE(code, 0);
            output = run(biber, {QStringLiteral("modern")}, code);
            QVERIFY2(code == 0, qPrintable(output));
            QVERIFY2(!output.contains(QStringLiteral("ERROR")), qPrintable(output));
            output = run(pdflatex, {QStringLiteral("-interaction=nonstopmode"), QStringLiteral("modern.tex")}, code);
            QVERIFY2(code == 0, qPrintable(output.right(3000)));
            QFile bbl(dir.filePath(QStringLiteral("modern.bbl")));
            QVERIFY(bbl.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromUtf8(bbl.readAll()).count(QStringLiteral("\\entry{")), items.size());
        }

        // For looking at, or trying in another program, if asked for.
        const QString output = qEnvironmentVariable("PAPERTRAIL_TEST_OUTPUT");
        if (!output.isEmpty()) {
            for (const QString &name : {QStringLiteral("refs.bib"), QStringLiteral("classic.blg"), QStringLiteral("modern.blg"),
                                        QStringLiteral("classic.pdf"), QStringLiteral("modern.pdf")}) {
                QFile::remove(QDir(output).filePath(name));
                QFile::copy(dir.filePath(name), QDir(output).filePath(name));
            }
            QFile ris(QDir(output).filePath(QStringLiteral("refs.ris")));
            if (ris.open(QIODevice::WriteOnly))
                ris.write(toRis(items));
            QFile json(QDir(output).filePath(QStringLiteral("refs.json")));
            if (json.open(QIODevice::WriteOnly))
                json.write(toCslJson(items));
        }
    }
};

QTEST_GUILESS_MAIN(CitationsTest)
#include "test_citations.moc"
