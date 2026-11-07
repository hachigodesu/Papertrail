#pragma once

#include "metadata/Providers.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace papertrail::core {
class Database;
}

// Deciding whether something found online is the publication in question, and what to change if it is.
namespace papertrail::metadata {

// From 0 (no words in common) to 1 (the same words), ignoring case, accents and word order. A title
// with a subtitle scores 1 against the same title without it.
double titleSimilarity(const QString &a, const QString &b);

// How likely a search result is to be this publication, from 0 to 1. It goes by the title, and also
// the first author and the year, where both sides have them.
double matchScore(const core::Publication &publication, const FoundRecord &found);

// Whether what an identifier led to is really this publication. DOIs are read from a document's first
// pages, and could be something it cites, so an identifier on its own isn't taken as proof.
bool titleAgrees(const core::Publication &publication, const QStringList &fileNames, const FoundRecord &found);

struct FieldChange {
    QString field;
    QString before;
    QString after;
};

// What applying this would change. Anything set by hand is left alone, and an identifier or abstract
// already there isn't replaced. It never marks anything a preprint: that's the user's to say.
QList<FieldChange> plannedChanges(const core::Publication &publication, const FoundRecord &found,
                                  const QSet<QString> &setByHand);

// The fields that have a value set by hand.
QSet<QString> fieldsSetByHand(core::Database &db, qint64 publicationId);

// Applies a result, keeping each earlier value so that it can be undone in one go. Returns how many
// fields changed.
int applyFoundRecord(core::Database &db, qint64 publicationId, const FoundRecord &found, bool needsReview);

// The online source behind the most recent changes that can still be undone, or empty if there aren't any.
QString undoableSource(core::Database &db, qint64 publicationId);

// Puts back everything the most recent lookup changed. Returns how many fields it put back.
int undoLastLookup(core::Database &db, qint64 publicationId);

// A readable name for a source.
QString providerLabel(const QString &provider);

} // namespace papertrail::metadata
