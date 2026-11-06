#include "import/Extractors.h"

#include <QtTest>

using namespace papertrail;
using namespace papertrail::import;
using core::PersonName;
using core::PublicationType;

class ExtractorsTest : public QObject {
    Q_OBJECT

private slots:
    void doisAreFoundAndCleaned_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        QTest::newRow("bare") << "see 10.1234/abc.def for more" << "10.1234/abc.def";
        QTest::newRow("url") << "https://doi.org/10.1234/ABC" << "10.1234/abc";
        QTest::newRow("trailing dot") << "doi:10.1234/abc." << "10.1234/abc";
        QTest::newRow("in brackets") << "(10.1234/abc)" << "10.1234/abc";
        QTest::newRow("slashes inside") << "10.1016/j.ijfatigue.2019.01.005," << "10.1016/j.ijfatigue.2019.01.005";
        QTest::newRow("first of two") << "10.1111/first and 10.2222/second" << "10.1111/first";
        QTest::newRow("none") << "version 10.12 of the tool" << "";
        QTest::newRow("short prefix") << "10.12/abc" << "";
    }

    void doisAreFoundAndCleaned()
    {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        QCOMPARE(findDoi(text), expected);
    }

    void isbnsMustHaveValidCheckDigits_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        QTest::newRow("13 hyphens") << "ISBN 978-0-306-40615-7" << "9780306406157";
        QTest::newRow("13 label") << "ISBN-13: 9780306406157" << "9780306406157";
        QTest::newRow("10") << "ISBN: 0-306-40615-2" << "0306406152";
        QTest::newRow("10 with X") << "ISBN 0-8044-2957-X" << "080442957X";
        QTest::newRow("bad check digit") << "ISBN 978-0-306-40615-8" << "";
        QTest::newRow("bad then good") << "ISBN 978-0-306-40615-8 ISBN 978-0-306-40615-7" << "9780306406157";
        QTest::newRow("bare long") << "printed as 978-0-306-40615-7 here" << "9780306406157";
        QTest::newRow("bare ten is ignored") << "phone 0306406152" << "";
        QTest::newRow("none") << "no book number here" << "";
    }

    void isbnsMustHaveValidCheckDigits()
    {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        QCOMPARE(findIsbn(text), expected);
    }

    void issnsMustHaveValidCheckDigits()
    {
        QCOMPARE(findIssn(QStringLiteral("ISSN 0378-5955")), QStringLiteral("0378-5955"));
        QCOMPARE(findIssn(QStringLiteral("ISSN: 2049-3630")), QStringLiteral("2049-3630"));
        QCOMPARE(findIssn(QStringLiteral("ISSN 1234-5678")), QString());
        QCOMPARE(findIssn(QStringLiteral("1234-5679 without a label")), QString());
    }

    void arxivIdsAreFound()
    {
        QCOMPARE(findArxivId(QStringLiteral("arXiv:2101.00001v2 [cs.LG]")), QStringLiteral("2101.00001v2"));
        QCOMPARE(findArxivId(QStringLiteral("arXiv: hep-th/9901001")), QStringLiteral("hep-th/9901001"));
        QCOMPARE(findArxivId(QStringLiteral("no identifier")), QString());
    }

    void yearsAreFoundOnlyWhereTheyMeanSomething()
    {
        QCOMPARE(findCopyrightYear(QStringLiteral("Copyright 2015 Example Press")), 2015);
        QCOMPARE(findCopyrightYear(QStringLiteral("© 1998 by the author")), 1998);
        QCOMPARE(findCopyrightYear(QStringLiteral("Published online 2021")), 2021);
        QCOMPARE(findCopyrightYear(QStringLiteral("Published: 12 May 2021")), 2021);
        QCOMPARE(findCopyrightYear(QStringLiteral("Available online 3 March 2019")), 2019);
        QCOMPARE(findCopyrightYear(QStringLiteral("Received 1 June 2017; accepted 9 July 2017")), 2017);
        QCOMPARE(findCopyrightYear(QStringLiteral("Received 2017. Copyright 2018 the authors")), 2018); // Most trusted first.
        QVERIFY(!findCopyrightYear(QStringLiteral("there were 2015 samples")).has_value());
        QCOMPARE(yearFromDate(QStringLiteral("2019-06-15")), 2019);
        QCOMPARE(yearFromDate(QStringLiteral("D:20190615120000")), std::nullopt);
        QVERIFY(!yearFromDate(QStringLiteral("9999")).has_value());
    }

    void junkTitlesAreRecognised_data()
    {
        QTest::addColumn<QString>("title");
        QTest::addColumn<bool>("junk");
        QTest::newRow("word") << "Microsoft Word - draft3.docx" << true;
        QTest::newRow("untitled") << "Untitled" << true;
        QTest::newRow("untitled doc") << "untitled document" << true;
        QTest::newRow("pdf name") << "final_version.pdf" << true;
        QTest::newRow("tex name") << "main.tex" << true;
        QTest::newRow("numbers") << "1234567" << true;
        QTest::newRow("id") << "s10853019" << true;
        QTest::newRow("short") << "abc" << true;
        QTest::newRow("empty") << "" << true;
        QTest::newRow("real") << "Fatigue of Steel Beams" << false;
        QTest::newRow("real with number") << "Results for 2019" << false;
        QTest::newRow("real with colon") << "Analysis: A Review" << false;
        QTest::newRow("starts like document") << "Documenting Software" << false;
        // Publisher production codes, all seen in real files.
        QTest::newRow("page range") << "ab1c00123 1..10" << true;
        QTest::newRow("code and pages") << "ABCD_A_123456 359..372" << true;
        QTest::newRow("journal code") << "JABC-12-345 1140..1146" << true;
        QTest::newRow("template") << "TX_1~ABS:AT/HTC:OS:TX_2~ABS:AT//AFF:AT" << true;
        QTest::newRow("cover") << "JABC Cover" << true;
        QTest::newRow("real with hyphen") << "Anti-ABC-1 therapy for example conditions" << false;
        QTest::newRow("real discovering") << "Under Cover of Darkness at Night" << false;
        QTest::newRow("real short") << "Generation XXX" << false;
    }

    void junkTitlesAreRecognised()
    {
        QFETCH(QString, title);
        QFETCH(bool, junk);
        QCOMPARE(isJunkTitle(title), junk);
    }

    void htmlCodesAreDecoded_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("hex dash") << "Example&#x2014;Sample" << "Example—Sample";
        QTest::newRow("hex amp") << "Alpha &#x0026; Beta" << "Alpha & Beta";
        QTest::newRow("hex percent") << "100&#x0025; Example" << "100% Example";
        QTest::newRow("decimal") << "A&#38;B" << "A&B";
        QTest::newRow("named") << "example&quest;" << "example?";
        QTest::newRow("amp") << "Fish &amp; Chips" << "Fish & Chips";
        QTest::newRow("plain ampersand") << "Fish & Chips" << "Fish & Chips";
        QTest::newRow("unknown stays") << "&notacode;" << "&notacode;";
        QTest::newRow("invalid number stays") << "&#xFFFFFFF;" << "&#xFFFFFFF;";
        QTest::newRow("surrogate stays") << "&#xD800;" << "&#xD800;";
        QTest::newRow("zero stays") << "&#0;" << "&#0;";
        QTest::newRow("empty") << "" << "";
    }

    void htmlCodesAreDecoded()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(decodeHtmlEntities(input), expected);
    }

    void fullTitlesAreToldFromHeadings()
    {
        QVERIFY(looksLikeFullTitle(QStringLiteral("The underlying mechanisms of example materials")));
        QVERIFY(looksLikeFullTitle(QStringLiteral("示例材料的结构与性能")));
        QVERIFY(!looksLikeFullTitle(QStringLiteral("Masthead")));
        QVERIFY(!looksLikeFullTitle(QStringLiteral("RESEARCH ARTICLE")));
        QVERIFY(!looksLikeFullTitle(QString()));
    }

    void junkAuthorsAreRecognised()
    {
        for (const char *name : {"Administrator", "user", "Microsoft Office User", "someone@example.org",
                                 "Adobe Acrobat", "owner"})
            QVERIFY2(isJunkAuthor(QString::fromLatin1(name)), name);
        for (const char *name : {"Jane Smith", "Smith, J.", "Van der Berg", "Ada"})
            QVERIFY2(!isJunkAuthor(QString::fromLatin1(name)), name);
    }

    void titlesComeFromFileNames()
    {
        QCOMPARE(titleFromFileName(QStringLiteral("fatigue_of-steel.beams_v2.pdf")), QStringLiteral("fatigue of steel beams v2"));
        QCOMPARE(titleFromFileName(QStringLiteral("Plain Name.epub")), QStringLiteral("Plain Name"));
        QCOMPARE(titleFromFileName(QStringLiteral(".pdf")), QStringLiteral(".pdf"));
    }

    void namesAreSplitIntoFamilyAndGiven_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("family");
        QTest::addColumn<QString>("given");
        QTest::newRow("given family") << "Jane Smith" << "Smith" << "Jane";
        QTest::newRow("family comma") << "Smith, Jane" << "Smith" << "Jane";
        QTest::newRow("middle") << "Donald E. Knuth" << "Knuth" << "Donald E.";
        QTest::newRow("single") << "Plato" << "Plato" << "";
        QTest::newRow("particle") << "Jan van der Berg" << "van der Berg" << "Jan";
        QTest::newRow("de") << "Marie de Silva" << "de Silva" << "Marie";
        QTest::newRow("spaces") << "  Jane   Smith " << "Smith" << "Jane";
    }

    void namesAreSplitIntoFamilyAndGiven()
    {
        QFETCH(QString, input);
        QFETCH(QString, family);
        QFETCH(QString, given);
        const PersonName name = parsePersonName(input);
        QCOMPARE(name.family, family);
        QCOMPARE(name.given, given);
    }

    void authorListsAreSplit_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("semicolons") << "A Smith; B Jones" << QStringList{"A Smith", "B Jones"};
        QTest::newRow("and") << "A Smith and B Jones" << QStringList{"A Smith", "B Jones"};
        QTest::newRow("ampersand") << "A Smith & B Jones" << QStringList{"A Smith", "B Jones"};
        QTest::newRow("family comma") << "Smith, Jane" << QStringList{"Smith, Jane"};
        QTest::newRow("commas between people") << "Jane Smith, Omar Haddad, Li Wen" << QStringList{"Jane Smith", "Omar Haddad", "Li Wen"};
        QTest::newRow("mixed") << "Smith, Jane; Haddad, Omar" << QStringList{"Smith, Jane", "Haddad, Omar"};
        QTest::newRow("empty") << "" << QStringList{};
        QTest::newRow("name containing and") << "Anderson Sandler" << QStringList{"Anderson Sandler"};
        QTest::newRow("slashes") << "Viktor Sample/Jean-Luc Example" << QStringList{"Viktor Sample", "Jean-Luc Example"};
    }

    void authorListsAreSplit()
    {
        QFETCH(QString, input);
        QFETCH(QStringList, expected);
        QCOMPARE(splitAuthorList(input), expected);
    }

    void xmpIsParsedInEveryForm()
    {
        const QByteArray xmp =
            "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
            "<rdf:Description xmlns:dc='http://purl.org/dc/elements/1.1/' xmlns:prism='http://p/' xmlns:pdf='http://q/'"
            " pdf:Keywords='beams, fatigue' prism:doi='10.1234/ATTR'>"
            "<dc:title><rdf:Alt><rdf:li xml:lang='x-default'>A Title</rdf:li></rdf:Alt></dc:title>"
            "<dc:creator><rdf:Seq><rdf:li>Jane Smith</rdf:li><rdf:li>Omar Haddad</rdf:li></rdf:Seq></dc:creator>"
            "<dc:date>2019</dc:date>"
            "<prism:issn>0378-5955</prism:issn>"
            "</rdf:Description></rdf:RDF></x:xmpmeta>";
        const EmbeddedMetadata m = parseXmp(xmp);
        QCOMPARE(m.title, QStringLiteral("A Title"));
        QCOMPARE(m.authors, (QStringList{QStringLiteral("Jane Smith"), QStringLiteral("Omar Haddad")}));
        QCOMPARE(m.date, QStringLiteral("2019"));
        QCOMPARE(m.doi, QStringLiteral("10.1234/attr"));
        QCOMPARE(m.issn, QStringLiteral("0378-5955"));
        QCOMPARE(m.keywords, QStringLiteral("beams, fatigue"));
    }

    void opfIsParsed()
    {
        const QByteArray opf =
            "<package xmlns='http://www.idpf.org/2007/opf'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>"
            "<dc:identifier>urn:uuid:12345678</dc:identifier>"
            "<dc:identifier>urn:isbn:9780306406157</dc:identifier>"
            "<dc:title>Book Title</dc:title><dc:creator>Ada Example</dc:creator><dc:creator>Grace Sample</dc:creator>"
            "<dc:publisher>Example Press</dc:publisher><dc:date>2018-05-01</dc:date><dc:language>en</dc:language>"
            "</metadata></package>";
        const EmbeddedMetadata m = parseOpf(opf);
        QCOMPARE(m.title, QStringLiteral("Book Title"));
        QCOMPARE(m.authors.size(), 2);
        QCOMPARE(m.isbn, QStringLiteral("9780306406157"));
        QCOMPARE(m.publisher, QStringLiteral("Example Press"));
        QCOMPARE(m.language, QStringLiteral("en"));
    }

    void badMetadataIsHarmless()
    {
        for (const QByteArray &input : {QByteArray(), QByteArray("not xml"), QByteArray("<a><b>"),
                                        QByteArray(5000, '\xff'), QByteArray("<?xml?><dc:title>x")}) {
            parseXmp(input);
            parseOpf(input);
            parseKindleHeader(input);
        }
    }

    void malformedKindleHeadersAreSafe()
    {
        // Start from a valid-looking magic number, then cut or corrupt it at every length.
        QByteArray header(4096, '\0');
        header.replace(60, 8, "BOOKMOBI");
        header[77] = 1;
        for (int length : {60, 68, 76, 79, 85, 86, 200, 300, 4096}) {
            const KindleHeader parsed = parseKindleHeader(header.left(length));
            QVERIFY(!parsed.isKf8);
        }

        // Offsets pointing far past the end of the data.
        QByteArray wild = header;
        wild[78] = '\x7f';
        wild[79] = '\xff';
        wild[80] = '\xff';
        wild[81] = '\xff';
        QVERIFY(!parseKindleHeader(wild).isKf8);
    }

    void classifierRecognisesEachKind_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<int>("pages");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("doi");
        QTest::addColumn<QString>("isbn");
        QTest::addColumn<QString>("issn");
        QTest::addColumn<QString>("arxiv");
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("confident");
        const auto row = [](const char *name, const char *format, int pages, const char *text, const char *doi,
                            const char *isbn, const char *issn, const char *arxiv, PublicationType type, bool sure) {
            QTest::newRow(name) << QString::fromLatin1(format) << pages << QString::fromLatin1(text) << QString::fromLatin1(doi)
                                << QString::fromLatin1(isbn) << QString::fromLatin1(issn) << QString::fromLatin1(arxiv)
                                << int(type) << sure;
        };
        using T = PublicationType;
        // Preprint wording and identifiers never sort anything by themselves: published papers have them too.
        row("arxiv stamp", "pdf", 12, "", "", "", "", "2101.00001", T::Other, false);
        row("arxiv stamp and doi", "pdf", 12, "", "10.1/a", "", "", "2101.00001", T::JournalArticle, true);
        row("preprint submitted", "pdf", 12, "Preprint submitted to Elsevier", "10.1/a", "", "", "", T::JournalArticle, true);
        row("accepted manuscript", "pdf", 12, "Author Accepted Manuscript", "10.1/a", "", "", "", T::JournalArticle, true);
        row("biorxiv doi", "pdf", 12, "results", "10.1101/2020.01.01.123456", "", "", "", T::JournalArticle, true);
        row("stamp alone", "pdf", 12, "PREPRINT", "", "", "", "", T::Other, false);
        row("cold spring harbor journal", "pdf", 12, "results", "10.1101/gr.123456", "", "", "", T::JournalArticle, true);
        row("date on a final version", "pdf", 12, "Accepted for publication 12 March 2012", "10.1/a", "", "", "", T::JournalArticle, true);
        row("citation in press", "pdf", 12, "as shown by Example et al. (in press)", "10.1/a", "", "", "", T::JournalArticle, true);
        row("standard", "pdf", 30, "INTERNATIONAL STANDARD ISO 9001", "", "", "", "", T::Standard, true);
        row("dissertation", "pdf", 200, "Doctor of Philosophy", "", "", "", "", T::Dissertation, true);
        row("thesis", "pdf", 80, "A thesis submitted in partial fulfilment", "", "", "", "", T::Thesis, true);
        row("report", "pdf", 40, "Technical Report 12", "", "", "", "", T::TechnicalReport, true);
        row("paper", "pdf", 8, "In Proceedings of the Conference", "10.1/a", "", "", "", T::ConferencePaper, true);
        row("proceedings", "pdf", 600, "Proceedings of the Symposium", "", "978", "", "", T::ConferenceProceedings, true);
        row("textbook", "pdf", 400, "Second Edition with exercises", "", "9780306406157", "", "", T::Textbook, true);
        row("book", "pdf", 300, "A history", "", "9780306406157", "", "", T::Book, true);
        row("article with doi", "pdf", 12, "results", "10.1/a", "", "", "", T::JournalArticle, true);
        row("article guess", "pdf", 10, "Abstract: results", "", "", "", "", T::JournalArticle, false);
        row("volume", "pdf", 500, "contents", "", "", "1234-5679", "", T::JournalVolume, false);
        row("epub", "epub", 0, "chapter", "", "", "", "", T::Book, false);
        row("long unknown", "pdf", 300, "chapter", "", "", "", "", T::Book, false);
        row("unknown", "pdf", 5, "notes", "", "", "", "", T::Other, false);
        row("nothing", "pdf", 0, "", "", "", "", "", T::Other, false);
        // A long systematic review, from real files: a DOI and no ISBN means an article, not a book.
        row("long article", "pdf", 300, "review", "10.1/a", "", "", "", T::JournalArticle, false);
        // A short paper can't be a thesis, whatever it says.
        row("too short for thesis", "pdf", 7, "PhD thesis", "10.1/a", "", "", "", T::JournalArticle, true);
    }

    void classifierRecognisesEachKind()
    {
        QFETCH(QString, format);
        QFETCH(int, pages);
        QFETCH(QString, text);
        QFETCH(QString, doi);
        QFETCH(QString, isbn);
        QFETCH(QString, issn);
        QFETCH(QString, arxiv);
        QFETCH(int, type);
        QFETCH(bool, confident);

        ClassificationInput input;
        input.format = format;
        if (pages > 0)
            input.pageCount = pages;
        input.firstPage = text;
        input.doi = doi;
        input.isbn = isbn;
        input.issn = issn;
        input.arxivId = arxiv;
        const Classification result = classify(input);
        QCOMPARE(int(result.type), type);
        QCOMPARE(result.confident, confident);
    }

    void referencesCantDecideAType()
    {
        ClassificationInput input;
        input.format = QStringLiteral("pdf");
        input.pageCount = 12;
        input.firstPage = QStringLiteral("A short paper about example results.");
        input.doi = QStringLiteral("10.1234/a");
        input.laterPages = QStringLiteral(
            "References [1] A. Author, Technical report 7. [2] B. Author, Proceedings of the Conference on X. "
            "[3] INTERNATIONAL STANDARD ISO 9001. [4] Conference on Y.");
        QCOMPARE(classify(input).type, PublicationType::JournalArticle);
    }

    void journalsWithProceedingsInTheirNamesAreJournals()
    {
        for (const char *journal : {"Proceedings of the IEEE", "Proceedings of the National Academy of Sciences",
                                    "Proceedings of the Royal Society B"}) {
            ClassificationInput input;
            input.format = QStringLiteral("pdf");
            input.pageCount = 20;
            input.firstPage = QString::fromLatin1(journal);
            input.doi = QStringLiteral("10.1234/a");
            QCOMPARE(classify(input).type, PublicationType::JournalArticle);
        }
    }

    void thesisWordingCanFollowABlankFirstPage()
    {
        ClassificationInput input;
        input.format = QStringLiteral("pdf");
        input.pageCount = 200;
        input.firstPage = QString();
        input.laterPages = QStringLiteral("A thesis submitted in partial fulfilment of the requirements");
        QCOMPARE(classify(input).type, PublicationType::Thesis);
    }
};

QTEST_GUILESS_MAIN(ExtractorsTest)
#include "test_extractors.moc"
