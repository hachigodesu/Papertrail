#pragma once

#include "core/model/Publication.h"

#include <QList>
#include <QString>

#include <optional>

namespace papertrail::citation {

// Text with italic parts, which is all the styling a reference needs.
class RichText {
public:
    struct Run {
        QString text;
        bool italic = false;
    };

    RichText &add(const QString &text, bool italic = false);
    RichText &italic(const QString &text) { return add(text, true); }
    RichText &append(const RichText &other);

    // Ends what's there with a full stop, unless it already ends a sentence, and adds a space if more follows.
    RichText &endSentence();

    bool isEmpty() const;
    QChar lastChar() const;
    QString plain() const;
    QString html() const; // Escaped, with <i> for italics.
    const QList<Run> &runs() const { return m_runs; }

private:
    QList<Run> m_runs;
};

// "J.R.R." from "John Ronald Reuel", "H.-P." from "Hans-Peter". `spaced` gives "J. R. R.".
QString initials(const QString &given, bool spaced);

// A name with no given name is an organisation's, and is written as it is.
bool isOrganisation(const core::PersonName &person);

// A name written in Chinese, Japanese or Korean characters, which has no initials to take.
bool isCjkName(const core::PersonName &person);

// "Hamamoto" and "Sato" for sorting and in-text citations.
QString familyName(const core::PersonName &person);

// Page ranges. Any dash or hyphen between two numbers is a range.
struct PageRange {
    QString first;
    QString last; // Empty for a single page.
    bool isRange() const { return !last.isEmpty(); }
};
std::optional<PageRange> pageRange(const QString &pages);
QString joinedPages(const QString &pages, const QString &dash); // "43" + dash + "70", or the text as it was.
QString chicagoPages(const QString &pages); // Shortened by Chicago's rules: 321–28, 101–8, 1496–500.

QString englishMonth(int month);       // "November"
QString englishShortMonth(int month);  // "Nov." as IEEE writes it, "May" and "June" in full.
QString accessedDate(int day, int month, int year); // "7 November 2026"

// "3rd", "2nd". Anything that isn't a plain number is returned as it is.
QString ordinal(const QString &number);
// An edition worth mentioning, such as "3rd ed.": empty for a first edition or none.
QString editionText(const QString &edition, const QString &abbreviation);

// Whether a degree is a doctorate or a master's, for styles that say which.
enum class DegreeLevel { Unknown, Doctoral, Masters };
DegreeLevel degreeLevel(const QString &degree, core::PublicationType type);

// For sorting a reference list by its first words, ignoring case and accents.
QString sortKey(const QString &text);

} // namespace papertrail::citation
