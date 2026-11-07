#pragma once

#include "citation/CitationItem.h"
#include "citation/Formatting.h"

#include <QDate>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::citation {

enum class CitationStyle { Harvard, Apa, Ieee, Chicago };

const QList<CitationStyle> &allCitationStyles();
QString styleKey(CitationStyle style); // As stored in the citation.style setting.
std::optional<CitationStyle> styleFromKey(const QString &key);
QString styleName(CitationStyle style); // As shown, such as "APA (7th edition)".

struct StyleOptions {
    // When an electronic source was looked at, for styles that say so. Usually today.
    QDate accessed = QDate::currentDate();
};

// A reference list in a style's order: alphabetical for author-date styles, and as given for IEEE, which numbers
// them. Works by one author in one year get letters after the year, as in 2020a and 2020b.
struct FormattedReferences {
    QList<RichText> entries;       // Each starts with its number for IEEE, as in "[1] ".
    QList<int> order;              // Which item each entry is, by its position in the list given.
    QList<RichText> inText;        // The in-text citation for each item, in the order given.
};

FormattedReferences formatReferences(const QList<CitationItem> &items, CitationStyle style,
                                     const StyleOptions &options = {});

// One in-text citation for all of these together, such as "(Smith 2020; Jones 2021)" or "[1]–[3]".
RichText combinedInTextCitation(const QList<CitationItem> &items, CitationStyle style, const StyleOptions &options = {});

} // namespace papertrail::citation
