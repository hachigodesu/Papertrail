#include "core/repo/CollectionRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QUuid>

#include <stdexcept>

namespace papertrail::core {

namespace {

// Publications hidden with a removed folder keep their place, but aren't counted until they're back.
const QByteArray kCollectionColumns =
    "SELECT c.id, c.uuid, c.name, (SELECT count(*) FROM collection_publication m "
    "WHERE m.collection_id = c.id AND m.publication_id NOT IN (SELECT publication_id FROM hidden_publication)) "
    "FROM collection c ";

Collection collectionFrom(Statement &select)
{
    return Collection{select.int64At(0), select.textAt(1), select.textAt(2), select.int64At(3)};
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

qint64 CollectionRepository::create(const QString &name)
{
    checkName(name, 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto insert = m_db.prepare("INSERT INTO collection (uuid, name, created_at, updated_at) VALUES (?, ?, ?, ?)");
    insert->bind(1, QUuid::createUuid().toString(QUuid::WithoutBraces));
    insert->bind(2, name.trimmed());
    insert->bind(3, now);
    insert->bind(4, now);
    insert->step();
    return m_db.lastInsertId();
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
    auto remove = m_db.prepare("DELETE FROM collection WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

QList<Collection> CollectionRepository::all()
{
    QList<Collection> collections;
    auto select = m_db.prepare(kCollectionColumns + "ORDER BY c.name");
    while (select->step())
        collections << collectionFrom(*select);
    return collections;
}

std::optional<Collection> CollectionRepository::get(qint64 id)
{
    auto select = m_db.prepare(kCollectionColumns + "WHERE c.id = ?");
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return collectionFrom(*select);
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
        "INSERT OR IGNORE INTO collection_publication (collection_id, publication_id, added_at) VALUES (?, ?, ?)");
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
    Statement &remove = m_db.cached("DELETE FROM collection_publication WHERE collection_id = ? AND publication_id = ?");
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
