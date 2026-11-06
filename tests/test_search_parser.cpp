#include "core/query/SearchParser.h"

#include <QtTest>

using namespace papertrail::core;

class SearchParserTest : public QObject {
    Q_OBJECT

private slots:
    void emptyInputGivesNothing()
    {
        for (const QString &text : {QString(), QStringLiteral(""), QStringLiteral("   ")}) {
            const ParsedSearch parsed = parseSearch(text);
            QVERIFY(parsed.ftsMatch.isEmpty());
            QVERIFY(parsed.tagPrefixes.isEmpty());
            QVERIFY(!parsed.yearMin.has_value());
        }
    }

    void plainWordsMatchAsPrefixes()
    {
        QCOMPARE(parseSearch(QStringLiteral("finite elem")).ftsMatch, QStringLiteral("\"finite\"* \"elem\"*"));
    }

    void quotesMakeAnExactPhrase()
    {
        QCOMPARE(parseSearch(QStringLiteral("\"finite element\" analysis")).ftsMatch,
                 QStringLiteral("\"finite element\" \"analysis\"*"));
    }

    void unclosedQuoteIsTolerated()
    {
        QCOMPARE(parseSearch(QStringLiteral("\"finite element")).ftsMatch, QStringLiteral("\"finite element\""));
    }

    void fieldsTargetTheirColumn()
    {
        QCOMPARE(parseSearch(QStringLiteral("title:bridge")).ftsMatch, QStringLiteral("title : \"bridge\"*"));
        QCOMPARE(parseSearch(QStringLiteral("journal:nature")).ftsMatch, QStringLiteral("container : \"nature\"*"));
        QCOMPARE(parseSearch(QStringLiteral("conference:icml")).ftsMatch, QStringLiteral("container : \"icml\"*"));
        QCOMPARE(parseSearch(QStringLiteral("publisher:wiley")).ftsMatch, QStringLiteral("publisher : \"wiley\"*"));
        QCOMPARE(parseSearch(QStringLiteral("abstract:beam")).ftsMatch, QStringLiteral("abstract : \"beam\"*"));
        QCOMPARE(parseSearch(QStringLiteral("TITLE:Bridge")).ftsMatch, QStringLiteral("title : \"Bridge\"*"));
    }

    void authorWordsMatchInAnyOrder()
    {
        QCOMPARE(parseSearch(QStringLiteral("author:\"donald knuth\"")).ftsMatch,
                 QStringLiteral("authors : \"donald\"* authors : \"knuth\"*"));
    }

    void quotedFieldValuesStayTogether()
    {
        QCOMPARE(parseSearch(QStringLiteral("title:\"finite element\"")).ftsMatch,
                 QStringLiteral("title : \"finite element\""));
    }

    void unknownFieldsAreOrdinaryText()
    {
        QCOMPARE(parseSearch(QStringLiteral("foo:bar")).ftsMatch, QStringLiteral("\"foo:bar\"*"));
        QCOMPARE(parseSearch(QStringLiteral("http://example.org")).ftsMatch, QStringLiteral("\"http://example.org\"*"));
    }

    void fieldWithNoValueIsIgnored()
    {
        QVERIFY(parseSearch(QStringLiteral("author:")).ftsMatch.isEmpty());
    }

    void tagsAreSeparateFromTextSearch()
    {
        const ParsedSearch parsed = parseSearch(QStringLiteral("tag:fem tag:\"solid mechanics\" beam"));
        QCOMPARE(parsed.tagPrefixes, (QStringList{QStringLiteral("fem"), QStringLiteral("solid mechanics")}));
        QCOMPARE(parsed.ftsMatch, QStringLiteral("\"beam\"*"));
    }

    void identifiersAreNormalised()
    {
        const ParsedSearch parsed = parseSearch(
            QStringLiteral("doi:https://doi.org/10.1000/ABC isbn:978-0-306-40615-7 issn:12345678 arxiv:arXiv:2101.00001"));
        QCOMPARE(parsed.doiPrefixes, QStringList{QStringLiteral("10.1000/abc")});
        QCOMPARE(parsed.isbns, QStringList{QStringLiteral("9780306406157")});
        QCOMPARE(parsed.issns, QStringList{QStringLiteral("1234-5678")});
        QCOMPARE(parsed.arxivPrefixes, QStringList{QStringLiteral("2101.00001")});
    }

    void yearExpressions_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("low");
        QTest::addColumn<int>("high");
        QTest::newRow("exact") << QStringLiteral("year:2010") << 2010 << 2010;
        QTest::newRow("after") << QStringLiteral("year:>2010") << 2011 << -1;
        QTest::newRow("from") << QStringLiteral("year:>=2010") << 2010 << -1;
        QTest::newRow("before") << QStringLiteral("year:<2010") << -1 << 2009;
        QTest::newRow("until") << QStringLiteral("year:<=2010") << -1 << 2010;
        QTest::newRow("range") << QStringLiteral("year:2000..2010") << 2000 << 2010;
    }

    void yearExpressions()
    {
        QFETCH(QString, text);
        QFETCH(int, low);
        QFETCH(int, high);
        const ParsedSearch parsed = parseSearch(text);
        QCOMPARE(parsed.yearMin.value_or(-1), low);
        QCOMPARE(parsed.yearMax.value_or(-1), high);
        QVERIFY(parsed.ftsMatch.isEmpty());
    }

    void invalidYearBecomesText()
    {
        const ParsedSearch parsed = parseSearch(QStringLiteral("year:recent"));
        QVERIFY(!parsed.yearMin.has_value());
        QCOMPARE(parsed.ftsMatch, QStringLiteral("\"recent\"*"));
    }

    void typesMatchByNameOrWord()
    {
        QCOMPARE(parseSearch(QStringLiteral("type:thesis")).types, QList<PublicationType>{PublicationType::Thesis});
        QCOMPARE(parseSearch(QStringLiteral("type:\"journal article\"")).types, QList<PublicationType>{PublicationType::JournalArticle});
        QCOMPARE(parseSearch(QStringLiteral("type:journal")).types,
                 (QList<PublicationType>{PublicationType::JournalVolume, PublicationType::JournalArticle}));
        QCOMPARE(parseSearch(QStringLiteral("type:nonsense")).ftsMatch, QStringLiteral("\"nonsense\"*"));
    }

    void quotesInsideTermsAreEscaped()
    {
        QCOMPARE(parseSearch(QStringLiteral("say\"hello")).ftsMatch, QStringLiteral("\"say\"\"hello\"*"));
    }

    void punctuationOnlyTermsAreDropped()
    {
        QVERIFY(parseSearch(QStringLiteral("-- ** ()")).ftsMatch.isEmpty());
        QCOMPARE(parseSearch(QStringLiteral("beam -- load")).ftsMatch, QStringLiteral("\"beam\"* \"load\"*"));
    }

    void ftsOperatorsAreJustWords()
    {
        // Quoted, so these can't act as FTS5 operators.
        QCOMPARE(parseSearch(QStringLiteral("cats AND NOT dogs")).ftsMatch,
                 QStringLiteral("\"cats\"* \"AND\"* \"NOT\"* \"dogs\"*"));
    }
};

QTEST_GUILESS_MAIN(SearchParserTest)
#include "test_search_parser.moc"
