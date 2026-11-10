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

    std::optional<qint64> parentId; // Only the articles in this issue, proceedings or book.
    QList<qint64> tagIds; // A tag also matches everything beneath it.
    QList<qint64> collectionIds;
    QList<qint64> authorIds;
    QStringList journals;
    QStringList publishers;
    QStringList conferences;
    QStringList institutions;
    QStringList courts;        // Case law's. The window only offers these on the Case Law tab.
    QStringList jurisdictions;
    QStringList languages;
    QStringList formats;
    QList<ReadStatus> readStatuses;
    // Opened at or after this, in milliseconds since the epoch. With read statuses, either will do, as they're
    // offered together.
    std::optional<qint64> openedSince;
    QList<int> ratings; // 1 to 5, or 0 for none.
    std::optional<int> yearMin;
    std::optional<int> yearMax;

    bool needsReview = false;
    bool noText = false;
    bool noFile = false;
    bool missingOrOffline = false;
    bool unreadable = false;
    bool retracted = false; // Retracted, withdrawn, or with an expression of concern.
};

// How long ago something can have been opened and still count as recently read.
constexpr qint64 kRecentlyOpenedMs = 30LL * 24 * 60 * 60 * 1000;

enum class SortField { Relevance, Title, FirstAuthor, Year, Added, Rating, LastOpened };

struct PublicationSort {
    SortField field = SortField::Title;
    bool ascending = true;
};

} // namespace papertrail::core
