#pragma once

// Made-up publications of every kind, as citation items, shared by test_citations and test_csl.

#include "citation/CitationItem.h"
#include "core/model/Publication.h"

#include <QList>
#include <QString>
#include <QUuid>

#include <utility>

namespace papertrail::tests {

using citation::CitationItem;
using citation::citationItem;
using core::Publication;
using core::PublicationType;

inline core::Publication publication(core::PublicationType type, const QString &title)
{
    Publication p;
    p.type = type;
    p.title = title;
    p.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return p;
}

// Every kind of publication, all made up, by the names the fixture files use.
inline QList<std::pair<QString, citation::CitationItem>> fixtureItems()
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

    // Made-up cases, one in each legal form. The Bluebook's is the one used unless OSCOLA's is chosen.
    Publication usCase = publication(PublicationType::CaseLaw, QStringLiteral("Sample v Example County Board"));
    usCase.containerTitle = QStringLiteral("F.4th");
    usCase.volume = QStringLiteral("12");
    usCase.pages = QStringLiteral("345-360");
    usCase.court = QStringLiteral("9th Cir.");
    usCase.jurisdiction = QStringLiteral("United States");
    usCase.year = 2021;
    items << std::make_pair(QStringLiteral("case_us"), citationItem(usCase));

    Publication ukCase = publication(PublicationType::CaseLaw, QStringLiteral("Made-Up Holdings Ltd v. Another Example Plc"));
    ukCase.neutralCitation = QStringLiteral("[2021] EWCA Civ 1234");
    ukCase.containerTitle = QStringLiteral("WLR");
    ukCase.volume = QStringLiteral("1");
    ukCase.pages = QStringLiteral("567");
    ukCase.court = QStringLiteral("Court of Appeal (Civil Division)");
    ukCase.year = 2021;
    ukCase.caseCitationForm = QStringLiteral("uk");
    items << std::make_pair(QStringLiteral("case_uk"), citationItem(ukCase));

    // A made-up product's datasheet, under its manufacturer.
    Publication manual = publication(PublicationType::DatasheetManual, QStringLiteral("PX-100 Pretend Voltage Regulator"));
    manual.publisher = QStringLiteral("Example Devices");
    manual.place = QStringLiteral("Leeds");
    manual.reportNumber = QStringLiteral("EXD-0042");
    manual.standardRevision = QStringLiteral("C");
    manual.productNumbers = QStringLiteral("PX-100, PX-100A");
    manual.year = 2023;
    manual.url = QStringLiteral("https://example.org/px100.pdf");
    items << std::make_pair(QStringLiteral("manual"), citationItem(manual));
    return items;
}

} // namespace papertrail::tests
