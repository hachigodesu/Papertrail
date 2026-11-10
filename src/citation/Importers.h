#pragma once

#include "core/model/Publication.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace papertrail::citation {

// One entry from another reference manager's export, such as Zotero's, Mendeley's, JabRef's or EndNote's.
struct ImportedReference {
    core::Publication publication; // Its details and type. Never a preprint: see preprintHint.
    bool preprintHint = false;     // The entry calls it a preprint, which only the user can make it here.
    QStringList tags;              // Its keywords, as tag paths such as "Engineering > Structures".
    QStringList files;             // The files the other manager kept for it, as it recorded their paths.
    QString note;                  // Its note or annotation.
    QString key;                   // Its citation key or ID.
    int line = 0;                  // Where it starts in the file, counting from 1.
};

enum class ReferenceFormat { BibTeX, Ris, Unknown };

struct ParsedReferences {
    ReferenceFormat format = ReferenceFormat::Unknown;
    QList<ImportedReference> entries;
    QStringList problems; // Entries that couldn't be read, such as "Line 12: the entry isn't closed".
};

// Reads a whole export. The text is UTF-8, or Latin-1 if it isn't valid UTF-8, with any byte order mark dropped. The
// format is told from what's in it, so a .txt file of either is read too.
ParsedReferences parseReferences(const QByteArray &bytes);
ParsedReferences parseBibTeX(const QString &text);
ParsedReferences parseRis(const QString &text);

// BibTeX's LaTeX as plain text: accents such as {\"u} and \'e as their letters, escapes such as \& as their
// characters, and the braces gone.
QString decodeLatex(const QString &text);

// The people in a BibTeX author or editor field: "Family, Given and Given von Family and {An Organisation}".
QList<core::PersonName> parseBibTeXNames(const QString &names);

// The paths in a BibTeX file field, as Zotero, Mendeley and JabRef write them: "Description:path:type" entries,
// separated by semicolons, with their colons and backslashes escaped.
QStringList parseFileField(const QString &field);

} // namespace papertrail::citation
