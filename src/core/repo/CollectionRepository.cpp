#include "core/repo/CollectionRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <stdexcept>

namespace papertrail::core {

namespace {

// Publications hidden with a removed folder keep their place, but aren't counted until they're back.
const QByteArray kCollectionColumns =
    "SELECT c.id, c.uuid, c.name, (SELECT count(*) FROM collection_publication m "
    "WHERE m.collection_id = c.id AND m.publication_id NOT IN (SELECT publication_id FROM hidden_publication)), "
    "(SELECT parent_id FROM collection_parent WHERE collection_id = c.id) FROM collection c ";

Collection collectionFrom(Statement &select)
{
    return Collection{select.int64At(0), select.textAt(1), select.textAt(2), select.int64At(3), select.optInt64At(4), QString()};
}

} // namespace

bool CollectionRepository::nameTaken(const QString &name, qint64 exceptId)
{
    auto select = m_db.prepare("SELECT 1 FROM collection WHERE name = ? AND id <> ?");
    select->bind(1, name.trimmed());
    select->bind(2, exceptId);
    return select->step();
}

void CollectionRepository::checkName(const QString &name, qint64 exceptId)
{
    if (name.trimmed().isEmpty())
        throw std::invalid_argument("a collection needs a name");
    if (nameTaken(name, exceptId))
        throw std::invalid_argument("there's already a collection with that name");
}

qint64 CollectionRepository::create(const QString &name, std::optional<qint64> parentId)
{
    checkName(name, 0);
    if (parentId && !get(*parentId))
        throw std::invalid_argument("the collection to put it in doesn't exist");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction transaction(m_db);
    auto insert = m_db.prepare("INSERT INTO collection (uuid, name, created_at, updated_at) VALUES (?, ?, ?, ?)");
    insert->bind(1, QUuid::createUuid().toString(QUuid::WithoutBraces));
    insert->bind(2, name.trimmed());
    insert->bind(3, now);
    insert->bind(4, now);
    insert->step();
    const qint64 id = m_db.lastInsertId();
    if (parentId)
        move(id, parentId);
    transaction.commit();
    return id;
}

QList<qint64> CollectionRepository::withInside(qint64 id)
{
    QList<qint64> ids;
    auto select = m_db.prepare("WITH RECURSIVE down(id) AS (SELECT ?1 UNION SELECT cp.collection_id FROM collection_parent cp "
                               "JOIN down ON cp.parent_id = down.id) SELECT id FROM down");
    select->bind(1, id);
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

void CollectionRepository::move(qint64 id, std::optional<qint64> parentId)
{
    if (parentId && withInside(id).contains(*parentId))
        throw std::invalid_argument("a collection can't go inside itself");
    Transaction transaction(m_db);
    auto clear = m_db.prepare("DELETE FROM collection_parent WHERE collection_id = ?");
    clear->bind(1, id);
    clear->step();
    if (parentId) {
        auto insert = m_db.prepare("INSERT INTO collection_parent (collection_id, parent_id) VALUES (?, ?)");
        insert->bind(1, id);
        insert->bind(2, *parentId);
        insert->step();
    }
    touch(id);
    transaction.commit();
}

void CollectionRepository::rename(qint64 id, const QString &name)
{
    checkName(name, id);
    auto update = m_db.prepare("UPDATE collection SET name = ?, updated_at = ? WHERE id = ?");
    update->bind(1, name.trimmed());
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, id);
    update->step();
}

void CollectionRepository::remove(qint64 id)
{
    Transaction transaction(m_db);
    auto remove = m_db.prepare("DELETE FROM collection WHERE id = ?");
    for (const qint64 each : withInside(id)) {
        remove->reset();
        remove->bind(1, each);
        remove->step();
    }
    transaction.commit();
}

QList<Collection> CollectionRepository::all()
{
    QList<Collection> collections;
    auto select = m_db.prepare(kCollectionColumns + "ORDER BY c.name");
    while (select->step())
        collections << collectionFrom(*select);
    QHash<qint64, const Collection *> byId;
    for (const Collection &collection : std::as_const(collections))
        byId.insert(collection.id, &collection);
    for (Collection &collection : collections) {
        QStringList names = {collection.name};
        QSet<qint64> seen = {collection.id};
        for (std::optional<qint64> up = collection.parentId; up && byId.contains(*up) && !seen.contains(*up);
             up = byId.value(*up)->parentId) {
            seen.insert(*up);
            names.prepend(byId.value(*up)->name);
        }
        collection.path = names.join(QStringLiteral(" > "));
    }
    std::sort(collections.begin(), collections.end(), [](const Collection &a, const Collection &b) {
        return QString::compare(a.path, b.path, Qt::CaseInsensitive) < 0;
    });
    return collections;
}

std::optional<Collection> CollectionRepository::get(qint64 id)
{
    for (const Collection &collection : all()) {
        if (collection.id == id)
            return collection;
    }
    return std::nullopt;
}

void CollectionRepository::touch(qint64 id)
{
    auto update = m_db.prepare("UPDATE collection SET updated_at = ? WHERE id = ?");
    update->bind(1, QDateTime::currentMSecsSinceEpoch());
    update->bind(2, id);
    update->step();
}

void CollectionRepository::add(qint64 id, const QList<qint64> &publicationIds)
{
    Transaction transaction(m_db);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Statement &insert = m_db.cached(
        "INSERT OR IGNORE INTO collection_publication (collection_id, publication_id, added_at) "
        "SELECT ?1, p.id, ?3 FROM publication p WHERE p.id = ?2 OR p.parent_id = ?2");
    for (const qint64 publicationId : publicationIds) {
        insert.bind(1, id);
        insert.bind(2, publicationId);
        insert.bind(3, now);
        insert.step();
        insert.reset();
    }
    touch(id);
    transaction.commit();
}

void CollectionRepository::removeFrom(qint64 id, const QList<qint64> &publicationIds)
{
    Transaction transaction(m_db);
    Statement &remove = m_db.cached(
        "DELETE FROM collection_publication WHERE collection_id = ?1 AND publication_id IN "
        "(SELECT id FROM publication WHERE id = ?2 OR parent_id = ?2)");
    for (const qint64 publicationId : publicationIds) {
        remove.bind(1, id);
        remove.bind(2, publicationId);
        remove.step();
        remove.reset();
    }
    touch(id);
    transaction.commit();
}

bool CollectionRepository::contains(qint64 id, qint64 publicationId)
{
    auto select = m_db.prepare("SELECT 1 FROM collection_publication WHERE collection_id = ? AND publication_id = ?");
    select->bind(1, id);
    select->bind(2, publicationId);
    return select->step();
}

QHash<qint64, qint64> CollectionRepository::membershipCounts(const QList<qint64> &publicationIds)
{
    QHash<qint64, qint64> counts;
    if (publicationIds.isEmpty())
        return counts;
    QByteArray placeholders = "?";
    for (qsizetype i = 1; i < publicationIds.size(); ++i)
        placeholders += ", ?";
    auto select = m_db.prepare("SELECT collection_id, count(*) FROM collection_publication WHERE publication_id IN ("
                               + placeholders + ") GROUP BY collection_id");
    for (qsizetype i = 0; i < publicationIds.size(); ++i)
        select->bind(int(i) + 1, publicationIds.at(i));
    while (select->step())
        counts.insert(select->int64At(0), select->int64At(1));
    return counts;
}

QList<Collection> CollectionRepository::collectionsFor(qint64 publicationId)
{
    QList<Collection> collections;
    auto select = m_db.prepare(
        kCollectionColumns
        + "WHERE c.id IN (SELECT collection_id FROM collection_publication WHERE publication_id = ?) ORDER BY c.name");
    select->bind(1, publicationId);
    while (select->step())
        collections << collectionFrom(*select);
    return collections;
}

} // namespace papertrail::core
