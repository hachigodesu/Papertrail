#pragma once

#include "core/settings/SettingsStore.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace papertrail::core {
class Database;
}

namespace papertrail::transfer {

// A library export is a zip of JSON files, with a manifest saying which version wrote it. Documents
// aren't in it, nor covers or the full-text index, which are made again on the other computer. Files are
// described by their contents (SHA-256), size, format and name, never by where they were, so they can be
// found again in a completely different folder structure.
constexpr int kBundleVersion = 1;
QString bundleSuffix(); // "papertrail"

struct Progress {
    std::function<bool()> cancelled;
    std::function<void(qint64 done, qint64 total)> progress;
};

struct ExportReport {
    qint64 publications = 0;
    qint64 tags = 0;
    qint64 collections = 0;
    qint64 annotations = 0;
    qint64 bookmarks = 0;
    qint64 summaries = 0;
    qint64 removed = 0;
    bool cancelled = false;
};

// Everything in the library that a person put there or chose, except publications only kept for a
// folder that's been removed. Throws std::runtime_error if it can't be read.
QByteArray buildBundle(core::Database &db, ExportReport &report, const Progress &progress = {});

// Writes the bundle to `path` in one go, so a failure never leaves half a file. Throws on failure.
ExportReport exportLibrary(core::Database &db, const QString &path, const Progress &progress = {});

struct Bundle {
    int version = 0;
    QJsonObject manifest;
    QJsonArray publications;
    QJsonArray tags;
    QJsonArray collections;
    QJsonArray savedSearches;
    QJsonArray annotations;
    QJsonArray bookmarks; // From 1.2.0 on. An export from before has none.
    QJsonArray readingStates;
    QJsonArray provenance;
    QJsonArray summaries;
    QJsonArray removed; // Files of publications removed by hand, by contents, so they stay out here too.
    QJsonObject settings;
};

// Reads an export. Anything that isn't one, is damaged, or comes from a newer version is refused with a reason.
std::optional<Bundle> readBundle(const QByteArray &data, QString *error);
std::optional<Bundle> readBundleFile(const QString &path, QString *error);

enum class ImportMode {
    Merge,   // Adds to the library. Where both have a publication, the more recently edited wins.
    Replace, // The library becomes a copy of the export, after a backup.
};

// Which parts of an export to bring in.
struct ImportParts {
    bool details = true;      // Publications, their read status, ratings, notes, summaries and history.
    bool tags = true;         // Tags, collections and saved searches.
    bool annotations = true;  // Highlights and notes in documents, and bookmarks.
    bool readingPlaces = true;
    bool settings = true;
};

struct ImportOptions {
    ImportMode mode = ImportMode::Merge;
    ImportParts parts;
    QString backupPath; // Where Replace keeps a copy of the library first. Required for Replace.
    Progress progress;
};

struct ImportReport {
    qint64 added = 0;
    qint64 updated = 0;
    qint64 unchanged = 0;
    qint64 linked = 0;    // Publications whose files were found here by their contents.
    qint64 proposed = 0;  // With a possible file to check, such as another download of the same paper.
    qint64 waiting = 0;   // With no file found yet. They're connected if one turns up later.
    qint64 tagsAdded = 0;
    qint64 collectionsAdded = 0;
    qint64 annotationsAdded = 0;
    qint64 annotationsUpdated = 0;
    qint64 bookmarksAdded = 0;
    qint64 summariesAdded = 0;
    qint64 readingPlaces = 0;
    qint64 removedAdded = 0; // Files to keep out, as they were removed on the other computer.
    core::SettingsImportResult settings;
    QString backupPath;
    bool cancelled = false; // Nothing was changed.
};

// Brings an export into the library in one transaction, so it either all happens or none of it does.
// Throws std::runtime_error or core::DbError if it can't.
ImportReport importBundle(core::Database &db, const Bundle &bundle, const ImportOptions &options);

// Possible files for imported publications, waiting for the user to accept or reject.
struct FileProposal {
    qint64 id = 0;
    qint64 publicationId = 0;
    QString publicationTitle;
    qint64 fileId = 0;
    QString filePath; // Relative to its library folder, with the folder's name first.
    QString reason;   // "identifier" or "name_and_size".
};
QList<FileProposal> fileProposals(core::Database &db);
qint64 fileProposalCount(core::Database &db);

// Accepting connects the file. If it was already another publication's, as when the same paper was found
// here separately, the two become one, keeping the imported one's details. Highlights made on the other
// computer's copy are marked as needing checking, since a different copy's pages may not line up.
void acceptFileProposal(core::Database &db, qint64 proposalId);
void rejectFileProposal(core::Database &db, qint64 proposalId);

} // namespace papertrail::transfer
