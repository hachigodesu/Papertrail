#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

struct Collection {
    qint64 id = 0;
    QString uuid;
    QString name;
    qint64 count = 0; // Publications in it that are in the library, so not those hidden with their folder.
};

// A named grouping of publications, for something like a project. It only ever says which publications
// belong: removing one, or deleting the collection, leaves the publications themselves alone.
class CollectionRepository {
public:
    explicit CollectionRepository(Database &db) : m_db(db) {}

    // Throws std::invalid_argument if the name is empty or taken. Names ignore case, so two can't look
    // the same in a menu except for that.
    qint64 create(const QString &name);
    void rename(qint64 id, const QString &name);
    void remove(qint64 id);
    bool nameTaken(const QString &name, qint64 exceptId = 0);

    QList<Collection> all();
    std::optional<Collection> get(qint64 id);

    void add(qint64 id, const QList<qint64> &publicationIds);
    void removeFrom(qint64 id, const QList<qint64> &publicationIds);
    bool contains(qint64 id, qint64 publicationId);
    QList<Collection> collectionsFor(qint64 publicationId);

    // How many of these publications each collection holds. Collections holding none aren't included.
    QHash<qint64, qint64> membershipCounts(const QList<qint64> &publicationIds);

private:
    void checkName(const QString &name, qint64 exceptId);
    void touch(qint64 id);

    Database &m_db;
};

} // namespace papertrail::core
