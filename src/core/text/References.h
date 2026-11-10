#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {

// Something a document's text points at, such as "Figure 3", "Eq. (4)", "[12]" or "Smith et al. (2020)", read from
// the words where someone clicked, for finding what it means even where the file has no links.
struct TextReference {
    enum class Kind { Figure, Table, Equation, Section, Appendix, Algorithm, Theorem, Citation, AuthorYear };
    Kind kind = Kind::Figure;
    QString word;    // How a caption or heading names it, such as "Figure", "Lemma" or "Section".
    QString number;  // Such as "3", "3b", "2.1" or "A". For a citation, its number in the references.
    QString surname; // For an author-year citation, the first author's family name.
    QString year;    // For an author-year citation, such as "2020" or "2020a".

    // How it's described to someone, such as "Figure 3", "Equation 4", "reference 12" or "Smith 2020".
    QString label() const;
};

// The reference at `offset` in `text`, if there's one there. `text` can run over several lines joined by spaces, so a
// reference split across two lines is still read whole. Where several numbers share one, such as "Figs. 3 and 4" or
// "[3, 7–9]", it's the one at `offset`, or the first when that's the word.
std::optional<TextReference> referenceAt(const QString &text, int offset);

// Whether a line is where a figure, table, section, theorem and the like starts: its caption or heading, such as
// "Figure 3: ..." or "2.1 Methods". A mention in a sentence, such as "Figure 3 shows", isn't.
bool isTargetLine(const TextReference &reference, const QString &line);

// Whether a line ends with an equation's number, such as "(4)".
bool endsWithEquationNumber(const QString &line, const QString &number);

// Whether a line is the heading of a list of references, such as "References" or "Bibliography".
bool isReferencesHeading(const QString &line);

// Whether a line starts an entry in a numbered list of references: "[12] ...", "12. ..." or "12 A. Author". Any
// number if `number` is empty.
bool startsNumberedEntry(const QString &line, const QString &number = QString());

// Whether a line starts an entry for this family name in a list of references, ignoring case and accents, after any
// number it has.
bool startsAuthorEntry(const QString &line, const QString &surname);

// Whether an entry's text has this year, such as "2020" or, if it's lettered, "2020a".
bool entryHasYear(const QString &entry, const QString &year);

// An entry in a table of contents, or a list of figures or tables, such as "2.1 Variance ........ 9" or
// "Appendix E: Statistical Tables   171": what it's called, and the page it gives, as printed.
struct ContentsEntry {
    QString title;     // Such as "2.1 Variance" or "Appendix E: Statistical Tables".
    QString pageLabel; // Such as "9" or "xiii".
    int page = 0;      // The printed page as a number, counting from 1, roman or not.
    bool roman = false;
};

// The entry a line of contents is, if it ends with a page number after its title. Whether the number is far enough
// from the title for it to be an entry is for whoever knows where its characters are.
std::optional<ContentsEntry> contentsEntry(const QString &line);

// Whether a line is the heading of a table of contents, or a list of figures or tables.
bool isContentsHeading(const QString &line);

// Whether a line is the heading a contents entry leads to, ignoring case, accents, punctuation and spacing: "2.1
// Variance" for "2.1  Variance", or the title alone, "Variance", where the heading's number is on a line of its own.
// A heading that wraps matches with its first line, if that's most of it.
bool headingMatchesEntry(const QString &line, const ContentsEntry &entry);

} // namespace papertrail::core
