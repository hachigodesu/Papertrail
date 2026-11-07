#pragma once

#include "citation/CitationItem.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace papertrail::citation {

enum class ExportFormat { BibTeX, Ris, CslJson };

QString exportFormatName(ExportFormat format);      // "BibTeX"
QString exportFileSuffix(ExportFormat format);      // "bib"
QString exportFileFilter(ExportFormat format);      // For a file dialog.

// A key for each item, such as "hamamoto2020lattice", unique within the list.
QStringList citationKeys(const QList<CitationItem> &items);

// Each is UTF-8, ready to write to a file.
QByteArray toBibTeX(const QList<CitationItem> &items);
QByteArray toRis(const QList<CitationItem> &items);
QByteArray toCslJson(const QList<CitationItem> &items);
QByteArray exportCitations(const QList<CitationItem> &items, ExportFormat format);

// Text for BibTeX with its special characters escaped. `protectCapitals` keeps acronyms and names such as
// "DNA" or "iPhone" from being lowercased by styles that change a title's case.
QString bibTeXText(const QString &text, bool protectCapitals = false);

} // namespace papertrail::citation
