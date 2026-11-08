#pragma once

#include "core/model/Publication.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace papertrail::core {

// What the library view is currently showing. Values within one field combine with OR, and
// different fields combine with AND. An empty field applies no restriction.
struct PublicationFilter {
    QString searchText; // The search box, including field searches such as author:knuth.
    bool searchContents = false; // Also looks for its plain words inside documents, if they've been indexed.
    QList<PublicationType> tabTypes; // From the selected tab.
    QList<PublicationType> types;

    QList<qint64> tagIds; // A tag also matches everything beneath it.
    QList<qint64> collectionIds;
    QList<qint64> authorIds;
    QStringList journals;
    QStringList publishers;
    QStringList conferences;
    QStringList institutions;
    QStringList languages;
    QStringList formats;
    QList<ReadStatus> readStatuses;
    std::optional<int> yearMin;
    std::optional<int> yearMax;

    bool needsReview = false;
    bool noText = false;
    bool noFile = false;
    bool missingOrOffline = false;
    bool unreadable = false;
    bool retracted = false; // Retracted, withdrawn, or with an expression of concern.
};

enum class SortField { Relevance, Title, FirstAuthor, Year, Added };

struct PublicationSort {
    SortField field = SortField::Title;
    bool ascending = true;
};

} // namespace papertrail::core
