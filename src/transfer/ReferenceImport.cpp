#include "transfer/ReferenceImport.h"

#include "core/db/Database.h"
#include "core/model/Identifiers.h"
#include "core/model/PublicationFields.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/RemovedContent.h"
#include "core/repo/TagRepository.h"
#include "import/ImportService.h"
#include "import/JournalIssues.h"
#include "metadata/Matching.h"
#include "metadata/Providers.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace papertrail::transfer {

using namespace core;
using citation::ImportedReference;

namespace {

bool isHidden(Database &db, qint64 publicationId)
{
    auto select = db.prepare("SELECT 1 FROM hidden_publication WHERE publication_id = ?");
    select->bind(1, publicationId);
    return select->step();
}

// What a path the other manager recorded leads to here: a file in one of the library's folders, or nothing.
struct FileFound {
    qint64 fileId = 0;
    bool outsideLibrary = false;
    bool inRemovedFolder = false;
    QString folder;
};

FileFound findFile(Database &db, const QList<LibraryRoot> &roots, const QList<LibraryRoot> &removed, const QString &recorded)
{
    FileFound found;
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(recorded.trimmed()));
    found.folder = QFileInfo(path).path();
    const auto lookIn = [&](const QList<LibraryRoot> &folders) -> qint64 {
        for (const LibraryRoot &root : folders) {
            const QString rootPath = QDir::cleanPath(QDir::fromNativeSeparators(root.path));
            if (!import::isPathInside(path, rootPath))
                continue;
            const QString relative = QDir(rootPath).relativeFilePath(path);
#ifdef Q_OS_WIN
            auto select = db.prepare("SELECT id FROM file WHERE root_id = ? AND relative_path = ? COLLATE NOCASE");
#else
            auto select = db.prepare("SELECT id FROM file WHERE root_id = ? AND relative_path = ?");
#endif
            select->bind(1, root.id);
            select->bind(2, relative);
            if (select->step())
                return select->int64At(0);
            return -1; // In this folder, but not a file it has.
        }
        return 0;
    };
    const qint64 inLibrary = lookIn(roots);
    if (inLibrary > 0) {
        found.fileId = inLibrary;
        return found;
    }
    if (inLibrary < 0)
        return found;
    if (lookIn(removed) != 0) {
        found.inRemovedFolder = true;
        return found;
    }
    found.outsideLibrary = true;
    return found;
}

bool titlesAgree(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return true;
    return mainTitleKey(a) == mainTitleKey(b) || metadata::titleSimilarity(a, b) >= 0.6;
}

std::optional<qint64> byIdentifier(Database &db, const Publication &entry)
{
    const auto lookUp = [&](const char *sql, const QString &value) -> std::optional<qint64> {
        if (value.isEmpty())
            return std::nullopt;
        auto select = db.prepare(sql);
        select->bind(1, value);
        while (select->step()) {
            const qint64 id = select->int64At(0);
            // An identifier copied wrongly into one manager shouldn't join two different works.
            if (titlesAgree(entry.title, select->textAt(1)))
                return id;
        }
        return std::nullopt;
    };
    if (auto id = lookUp("SELECT id, title FROM publication WHERE doi = ?", normaliseDoi(entry.doi)))
        return id;
    if (auto id = lookUp("SELECT id, title FROM publication WHERE isbn13 = ?", normaliseIsbn(entry.isbn13)))
        return id;
    if (auto id = lookUp("SELECT id, title FROM publication WHERE isbn10 = ?", normaliseIsbn(entry.isbn10)))
        return id;
    return lookUp("SELECT id, title FROM publication WHERE arxiv_id = ?", normaliseArxivId(entry.arxivId));
}

// The same main title, and the same year and first author where both have them. Certain only for a distinctive one.
std::optional<std::pair<qint64, bool>> byTitle(Database &db, const Publication &entry)
{
    const QString key = mainTitleKey(entry.title);
    if (key.isEmpty())
        return std::nullopt;
    auto select = db.prepare("SELECT id FROM publication WHERE title_sort >= ? AND title_sort < ? ORDER BY id");
    select->bind(1, key);
    select->bind(2, key + QChar(0xFFFF));
    QList<qint64> candidates;
    while (select->step())
        candidates << select->int64At(0);
    PublicationRepository publications(db);
    for (const qint64 id : std::as_const(candidates)) {
        const std::optional<Publication> found = publications.get(id);
        if (!found || mainTitleKey(found->title) != key)
            continue;
        if (entry.year && found->year && *entry.year != *found->year)
            continue;
        if (!entry.authors.isEmpty() && !found->authors.isEmpty()
            && nameSortKey(entry.authors.first().family) != nameSortKey(found->authors.first().family))
            continue;
        return std::make_pair(id, isDistinctiveTitle(key));
    }
    return std::nullopt;
}

} // namespace

int ReferencePlan::count(ReferenceMatch match) const
{
    return int(std::count_if(entries.cbegin(), entries.cend(), [&](const PlannedReference &entry) { return entry.match == match; }));
}

ReferencePlan planReferenceImport(Database &db, const citation::ParsedReferences &parsed, const QString &name)
{
    ReferencePlan plan;
    plan.format = parsed.format;
    plan.name = name;
    plan.problems = parsed.problems;
    FileRepository files(db);
    RemovedContent removed(db);
    PublicationRepository publications(db);
    const QList<LibraryRoot> roots = files.roots();
    const QList<LibraryRoot> removedRoots = files.removedRoots();
    QSet<qint64> matched;
    QSet<QString> outside;

    for (const ImportedReference &entry : parsed.entries) {
        PlannedReference planned;
        planned.entry = entry;
        bool keptOut = false;

        // A file it names is the surest match.
        for (const QString &path : entry.files) {
            const FileFound found = findFile(db, roots, removedRoots, path);
            if (found.outsideLibrary) {
                planned.filesOutsideLibrary = true;
                if (!found.folder.isEmpty())
                    outside.insert(found.folder);
            }
            keptOut = keptOut || found.inRemovedFolder;
            if (found.fileId == 0)
                continue;
            const std::optional<FileRecord> file = files.get(found.fileId);
            if (file && removed.covers(file->id, file->sha256, file->size)) {
                keptOut = true;
                continue;
            }
            const QList<qint64> owners = files.publicationsFor(found.fileId);
            if (!owners.isEmpty()) {
                planned.match = ReferenceMatch::ByFile;
                planned.publicationId = owners.first();
                break;
            }
        }
        if (planned.match == ReferenceMatch::None) {
            if (const std::optional<qint64> id = byIdentifier(db, entry.publication)) {
                planned.match = ReferenceMatch::ByIdentifier;
                planned.publicationId = *id;
            } else if (const auto title = byTitle(db, entry.publication)) {
                planned.match = title->second ? ReferenceMatch::ByTitle : ReferenceMatch::ByTitleUnsure;
                planned.publicationId = title->first;
            }
        }
        if (planned.publicationId != 0 && isHidden(db, planned.publicationId)) {
            planned.match = ReferenceMatch::KeptOut;
        } else if (planned.publicationId != 0 && matched.contains(planned.publicationId)) {
            planned.match = ReferenceMatch::Repeated;
        } else if (planned.match == ReferenceMatch::None && keptOut) {
            planned.match = ReferenceMatch::KeptOut;
        }
        if (planned.publicationId != 0) {
            matched.insert(planned.publicationId);
            if (const std::optional<Publication> found = publications.get(planned.publicationId))
                planned.matchedTitle = found->title;
        }
        plan.entries << planned;
    }
    plan.foldersOutside = QStringList(outside.cbegin(), outside.cend());
    plan.foldersOutside.sort();
    return plan;
}

ReferenceImportReport applyReferenceImport(Database &db, const ReferencePlan &plan, const ReferenceImportOptions &options,
                                           const std::function<bool()> &cancelled,
                                           const std::function<void(int, int)> &progress)
{
    ReferenceImportReport report;
    PublicationRepository publications(db);
    TagRepository tags(db);
    const QString source = plan.format == citation::ReferenceFormat::Ris ? metadata::kRisImport : metadata::kBibTeXImport;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction transaction(db);
    QList<qint64> touched;

    for (qsizetype index = 0; index < plan.entries.size(); ++index) {
        if (cancelled && cancelled()) {
            report.cancelled = true;
            return report; // Not committed, so nothing changes.
        }
        if (progress)
            progress(int(index), int(plan.entries.size()));
        const PlannedReference &planned = plan.entries.at(index);
        const ImportedReference &entry = planned.entry;

        qint64 id = 0;
        switch (planned.match) {
        case ReferenceMatch::KeptOut:
            ++report.keptOut;
            continue;
        case ReferenceMatch::None: {
            if (!options.addUnmatched) {
                ++report.notAdded;
                continue;
            }
            Publication p = entry.publication;
            if (p.type == PublicationType::Preprint)
                p.type = PublicationType::Other;
            p.notes = entry.note;
            if (entry.preprintHint)
                p.extraJson = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("preprintHint"), source}}).toJson(QJsonDocument::Compact));
            id = publications.create(p);
            ++report.added;
            break;
        }
        default: {
            id = planned.publicationId;
            std::optional<Publication> current = publications.get(id);
            if (!current)
                continue;
            const bool onlyGaps = planned.match == ReferenceMatch::Repeated;
            const QList<metadata::FieldChange> changes =
                metadata::importedChanges(*current, entry.publication, metadata::fieldsSetByHand(db, id), onlyGaps);
            Publication changed = *current;
            for (const metadata::FieldChange &change : changes)
                setMetadataFieldValue(changed, change.field, change.after);
            if (!entry.note.isEmpty()) {
                if (changed.notes.trimmed().isEmpty())
                    changed.notes = entry.note;
                else if (!changed.notes.contains(entry.note))
                    ++report.notesLeftOut;
            }
            if (entry.preprintHint && changed.type != PublicationType::Preprint) {
                QJsonObject extra = QJsonDocument::fromJson(changed.extraJson.toUtf8()).object();
                if (!extra.contains(QStringLiteral("preprintHint"))) {
                    extra.insert(QStringLiteral("preprintHint"), source);
                    changed.extraJson = QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact));
                }
            }
            if (planned.match == ReferenceMatch::ByTitleUnsure && !changed.needsReview) {
                changed.needsReview = true;
                ++report.toReview;
                report.reviewIds << id;
            }
            const bool same = changes.isEmpty() && changed.notes == current->notes && changed.extraJson == current->extraJson
                              && changed.needsReview == current->needsReview;
            if (same) {
                ++report.unchanged;
            } else {
                // One time for all of them, so undoing takes them back together.
                publications.updateWithProvenance(changed, source, now);
                ++report.updated;
                if (changed.parentId)
                    import::tidyJournalIssue(db, *changed.parentId, publications.get(id));
                else if (changed.type == PublicationType::JournalIssue)
                    import::tidyJournalIssue(db, id);
            }
            break;
        }
        }

        for (const QString &path : entry.tags) {
            try {
                tags.assign(id, tags.ensurePath(path));
            } catch (const std::invalid_argument &) {
                // A tag that can't be a name here, such as an empty one, is left out.
            }
        }
        touched << id;
    }

    if (options.intoNewCollection && !touched.isEmpty()) {
        CollectionRepository collections(db);
        QString name = plan.name.trimmed().isEmpty() ? QStringLiteral("Imported references") : plan.name.trimmed();
        const QString base = name;
        for (int n = 2; collections.nameTaken(name); ++n)
            name = QStringLiteral("%1 (%2)").arg(base).arg(n);
        report.collectionId = collections.create(name);
        collections.add(report.collectionId, touched);
    }
    if (progress)
        progress(int(plan.entries.size()), int(plan.entries.size()));
    transaction.commit();
    return report;
}

} // namespace papertrail::transfer
