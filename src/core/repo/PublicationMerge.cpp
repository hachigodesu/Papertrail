#include "core/repo/PublicationMerge.h"

#include "core/db/Database.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/ReadingStateRepository.h"
#include "core/repo/TagRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <stdexcept>

namespace papertrail::core {

namespace {

int readStatusRank(ReadStatus status)
{
    switch (status) {
    case ReadStatus::Unread: return 0;
    case ReadStatus::Reading: return 1;
    case ReadStatus::Read: return 2;
    }
    return 0;
}

// So that a library export from before the merge can still find this publication.
void rememberMergedUuid(Publication &keep, const QString &uuid)
{
    QJsonObject extra = QJsonDocument::fromJson(keep.extraJson.toUtf8()).object();
    QJsonArray uuids = extra.value(QStringLiteral("mergedUuids")).toArray();
    uuids.append(uuid);
    extra.insert(QStringLiteral("mergedUuids"), uuids);
    keep.extraJson = QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact));
}

} // namespace

void mergePublications(Database &db, qint64 keepId, qint64 otherId, const QString &source)
{
    if (keepId == otherId)
        return;

    PublicationRepository publications(db);
    FileRepository files(db);
    ReadingStateRepository reading(db);

    Transaction transaction(db);
    std::optional<Publication> keep = publications.get(keepId);
    const std::optional<Publication> other = publications.get(otherId);
    if (!keep || !other)
        throw std::invalid_argument("publication doesn't exist");

    // Places are worked out first, while each publication still has its own files.
    const QList<FileRecord> keptFiles = files.filesFor(keepId);
    const QList<FileRecord> otherFiles = files.filesFor(otherId);
    if (!keptFiles.isEmpty())
        reading.assignToFormat(keepId, keptFiles.first().format);
    if (!otherFiles.isEmpty())
        reading.assignToFormat(otherId, otherFiles.first().format);
    reading.absorb(keepId, otherId);

    for (const FileRecord &file : otherFiles)
        files.link(keepId, file.id, false);

    PublicationRepository::fillGaps(*keep, *other);
    if (readStatusRank(other->readStatus) > readStatusRank(keep->readStatus))
        keep->readStatus = other->readStatus;
    if (!other->notes.isEmpty() && other->notes != keep->notes) {
        keep->notes = keep->notes.isEmpty() ? other->notes
                                            : keep->notes + QStringLiteral("\n\n") + other->notes;
    }
    rememberMergedUuid(*keep, other->uuid);
    publications.update(*keep);

    const auto move = [&](const char *sql) {
        auto statement = db.prepare(sql);
        statement->bind(1, keepId);
        statement->bind(2, otherId);
        statement->step();
    };
    move("INSERT OR IGNORE INTO publication_tag (publication_id, tag_id) "
         "SELECT ?1, tag_id FROM publication_tag WHERE publication_id = ?2");
    move("INSERT OR IGNORE INTO collection_publication (collection_id, publication_id, added_at) "
         "SELECT collection_id, ?1, added_at FROM collection_publication WHERE publication_id = ?2");
    move("UPDATE annotation SET publication_id = ?1 WHERE publication_id = ?2");
    move("UPDATE metadata_provenance SET publication_id = ?1 WHERE publication_id = ?2");
    move("UPDATE publication SET parent_id = ?1 WHERE parent_id = ?2");

    publications.recordProvenance(keepId, {QStringLiteral("merged"), source, std::nullopt, other->title, keep->title, 0});
    publications.remove(otherId);
    transaction.commit();
}

qint64 separateFormat(Database &db, qint64 publicationId, const QString &format)
{
    PublicationRepository publications(db);
    FileRepository files(db);
    TagRepository tags(db);
    ReadingStateRepository reading(db);

    Transaction transaction(db);
    const std::optional<Publication> original = publications.get(publicationId);
    if (!original)
        throw std::invalid_argument("publication doesn't exist");

    QList<FileRecord> moving;
    QList<FileRecord> staying;
    for (const FileRecord &file : files.filesFor(publicationId))
        (file.format == format ? moving : staying) << file;
    if (moving.isEmpty() || staying.isEmpty())
        throw std::invalid_argument("a publication has to keep at least one format");

    Publication copy = *original;
    copy.uuid.clear();
    copy.createdAt = 0;
    copy.updatedAt = 0;
    copy.needsReview = true;
    copy.notes.clear(); // These stay with the original, rather than being in two places.
    const qint64 newId = publications.create(copy);

    for (const Tag &tag : tags.tagsFor(publicationId))
        tags.assign(newId, tag.id);
    auto inCollections = db.prepare(
        "INSERT OR IGNORE INTO collection_publication (collection_id, publication_id, added_at) "
        "SELECT collection_id, ?1, added_at FROM collection_publication WHERE publication_id = ?2");
    inCollections->bind(1, newId);
    inCollections->bind(2, publicationId);
    inCollections->step();

    bool primary = true;
    for (const FileRecord &file : moving) {
        files.unlink(publicationId, file.id);
        files.link(newId, file.id, primary);
        primary = false;
    }
    files.link(publicationId, staying.first().id, true);

    reading.moveFormat(publicationId, newId, format);
    publications.recordProvenance(newId, {QStringLiteral("separated"), QStringLiteral("manual"), std::nullopt, QString(), format, 0});
    transaction.commit();
    return newId;
}

} // namespace papertrail::core
