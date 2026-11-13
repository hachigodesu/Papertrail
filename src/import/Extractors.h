#pragma once

#include "core/model/Publication.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::import {

// Bibliographic details found inside a file.
struct EmbeddedMetadata {
    QString title;
    QStringList authors; // As written in the file.
    QString publisher;
    QString date;
    QString language;
    QString doi;
    QString isbn;
    QString issn;
    QString description;
    QString keywords;

    // Fills any empty field from `other`.
    void fillGapsFrom(const EmbeddedMetadata &other);

    // Turns codes such as &#x2014; into the characters they stand for, in every text field.
    void decodeEntities();
};

// Publishers often leave HTML codes in metadata: "Samples&#x2019; Challenges &amp; Perspectives".
QString decodeHtmlEntities(const QString &text);

EmbeddedMetadata parseXmp(const QByteArray &xmp);
EmbeddedMetadata parseOpf(const QByteArray &opf);
EmbeddedMetadata parsePdfInfo(const QString &title, const QString &author, const QString &subject,
                              const QString &keywords);

// What a Kindle file's header says. KF8 files need converting before they can be read: MuPDF
// opens them without complaint, but gives a blank page.
struct KindleHeader {
    bool isEncrypted = false; // Locked with DRM.
    bool isKindleFile = false;
    bool isKf8 = false;
    EmbeddedMetadata metadata;
};

// `head` is the start of the file. 256 KB covers any real header.
KindleHeader parseKindleHeader(const QByteArray &head);

// Each returns the first valid match, in canonical form, or a null QString.
QString findDoi(const QString &text);
QString findIsbn(const QString &text); // Checks the check digit.
QString findIssn(const QString &text); // Checks the check digit.
QString findArxivId(const QString &text);
bool isValidIsbn(const QString &digits);

// The DOI of the article starting on this page, if it's printed there. Anything after a list of references
// is left out, and so is a page with more than one DOI, since there's then no telling which is the article's.
QString articleDoiOnPage(const QString &pageText);
// Every DOI on a page in the order they're printed, each once, leaving out any after a list of references.
QStringList doisOnPage(const QString &pageText);

// A journal issue's numbers as printed on its cover or in a running header, such as
// "VOL. 7, NO. 11, NOVEMBER 2026" or "NOVEMBER 2026 VOLUME 7 NUMBER 11".
struct IssueNumbering {
    QString volume;
    QString issue;
    std::optional<int> month;
    std::optional<int> year;
};
std::optional<IssueNumbering> findIssueNumbering(const QString &text);

// A likely publication year from text such as a copyright line, or from a metadata date.
std::optional<int> findCopyrightYear(const QString &text);
std::optional<int> yearFromDate(const QString &date);

// Whether a title from a file's metadata is obviously not its real title, for example
// "Microsoft Word - draft3.docx" or "untitled".
bool isJunkTitle(const QString &title);

// Whether an author from a file's metadata is obviously not a person, for example "Administrator".
bool isJunkAuthor(const QString &author);

// A readable title from a file name: "fatigue_of-steel.beams_v2.pdf" gives
// "fatigue of steel beams v2".
QString titleFromFileName(const QString &fileName);

// Understands "Family, Given" and "Given Family", and keeps particles such as "van der" with
// the family name.
core::PersonName parsePersonName(const QString &name);

// Splits "A Smith; B Jones and C Lee" into its people.
QStringList splitAuthorList(const QString &authors);

// Whether a title is a real-looking one of a few words, rather than a logo or a section name.
bool looksLikeFullTitle(const QString &title);

struct ClassificationInput {
    QString format;
    std::optional<int> pageCount;
    QString firstPage;
    // Later pages. These hold references, which mention theses, reports and conferences that have
    // nothing to do with this document, so they only ever count as weak evidence.
    QString laterPages;
    QString title;
    QString doi;
    QString isbn;
    QString issn;
    QString arxivId;
};

struct Classification {
    core::PublicationType type = core::PublicationType::Other;
    bool confident = false;
};

// A best guess at what kind of publication a file is.
Classification classify(const ClassificationInput &input);

// What a court's judgment says about itself on its first page.
struct CaseDetails {
    QString name;            // Such as "Donoghue v Stevenson", if a line reads as one.
    QString court;           // Such as "Supreme Court of the United Kingdom", or "9th Cir." as the Bluebook has it.
    QString jurisdiction;    // "United Kingdom", "England and Wales", "United States", and so on.
    QString neutralCitation; // Such as "[2020] UKSC 5".
    QString caseNumber;      // Such as "No. 19-123" or "A2/2019/1234".
    QString report;          // The law report it's printed in, such as "S.C.R.", with its volume and first page.
    QString reportVolume;
    QString reportPage;
    std::optional<int> year;
    std::optional<int> month;
    std::optional<int> day;
};

// A first page that reads as a judgment: a UK-style neutral citation or a court's heading, with a case's name or its
// parties, and not an article's abstract. Only the first page decides, since articles and reference lists cite
// cases all the time. Never certain, so a publication found this way is marked for review. `title` is what the page's
// largest text or the file says it's called, which is often the case's name when the text runs its lines together.
// `laterPages` are only read for a judgment printed in the Supreme Court Reports, whose first page heads it with the
// report and whose neutral citation and name come a few pages on, under "Indexed as:".
std::optional<CaseDetails> caseDetailsOnFirstPage(const QString &firstPage, const QString &title = QString(),
                                                  const QString &laterPages = QString());

} // namespace papertrail::import
