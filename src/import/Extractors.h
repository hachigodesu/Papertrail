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

} // namespace papertrail::import
