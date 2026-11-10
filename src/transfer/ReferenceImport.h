#pragma once

#include "citation/Importers.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace papertrail::core {
class Database;
}

// Bringing in details and tags from another reference manager's BibTeX or RIS export, for publications already in
// the library, and, if asked, the entries that aren't.
namespace papertrail::transfer {

enum class ReferenceMatch {
    ByFile,       // A file the entry names is one of this publication's.
    ByIdentifier, // The same DOI, ISBN or arXiv number, and a title that agrees.
    ByTitle,      // The same distinctive title, with the year and first author agreeing where both have them.
    ByTitleUnsure, // The same short title, so it's marked for review.
    Repeated,     // A publication an earlier entry in the file already matched, whose gaps this fills.
    KeptOut,      // A publication in a removed folder, or one that was removed, which is left alone.
    None,
};

struct PlannedReference {
    citation::ImportedReference entry;
    ReferenceMatch match = ReferenceMatch::None;
    qint64 publicationId = 0;
    QString matchedTitle;            // As the library has it.
    bool filesOutsideLibrary = false; // It names a file in none of the library's folders.
};

struct ReferencePlan {
    citation::ReferenceFormat format = citation::ReferenceFormat::Unknown;
    QString name; // The file's name without its suffix, for a collection named after it.
    QList<PlannedReference> entries;
    QStringList problems;
    // Folders the entries' files are in that aren't the library's. Only for showing: nothing's added for them.
    QStringList foldersOutside;

    int count(ReferenceMatch match) const;
};

// What each entry would do, without changing anything. `name` is the file's name, without its suffix.
ReferencePlan planReferenceImport(core::Database &db, const citation::ParsedReferences &parsed, const QString &name);

struct ReferenceImportOptions {
    bool addUnmatched = false;      // As publications without files, which join their files if they turn up later.
    bool intoNewCollection = false; // Everything the import touched, in a collection named after the file.
};

struct ReferenceImportReport {
    int updated = 0;      // Publications that had something changed or added.
    int unchanged = 0;    // Matched, with nothing new to add.
    int added = 0;        // New publications for entries that matched nothing.
    int notAdded = 0;     // Entries that matched nothing, left out.
    int keptOut = 0;
    int toReview = 0;     // Matched by a short title, so marked for review.
    int notesLeftOut = 0; // Entries with a note for a publication that already had one.
    qint64 collectionId = 0;
    QList<qint64> reviewIds;
    bool cancelled = false;
};

// Applies a plan in one transaction, which `cancelled` returning true rolls back. Fields set by hand are never changed,
// and every change is recorded so that the details panel can undo it. Make a backup first.
ReferenceImportReport applyReferenceImport(core::Database &db, const ReferencePlan &plan, const ReferenceImportOptions &options,
                                           const std::function<bool()> &cancelled = {},
                                           const std::function<void(int done, int total)> &progress = {});

} // namespace papertrail::transfer
