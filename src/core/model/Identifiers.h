#pragma once

#include <QString>

namespace papertrail::core {

// Identifiers are stored in one canonical form, so lookups and duplicate checks can compare them
// directly. Each returns a null QString for empty input. A value that doesn't look valid is kept,
// lightly cleaned, because it's the user's data.
QString normaliseDoi(const QString &value);
QString normaliseIsbn(const QString &value);
QString normaliseIssn(const QString &value);
QString normaliseArxivId(const QString &value);

// A key for sorting: case-folded, without accents, and ignoring a leading "The", "A" or "An".
QString titleSortKey(const QString &title);

// As above, but keeps leading words, because a family name can start with "A" or "The".
QString nameSortKey(const QString &name);

// A title without its subtitle, ready to compare. A PDF's metadata often has only one or the other.
QString mainTitleKey(const QString &title);
// Long enough that two different works are unlikely to share it, unlike "Introduction" or "Notes".
bool isDistinctiveTitle(const QString &key);

// Text for matching what someone types: case-folded, without accents, and with its spaces tidied.
QString foldedText(const QString &text);

} // namespace papertrail::core
