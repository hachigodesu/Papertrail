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
    std::optional<qint64> parentId;
    QString path; // Its name after those it's inside, such as "Thesis > Chapter 2".
};

// A named grouping of publications, for something like a project. It only ever says which publications
// belong: removing one, or deleting the collection, leaves the publications themselves alone.
class CollectionRepository {
public:
    explicit CollectionRepository(Database &db) : m_db(db) {}

    // Throws std::invalid_argument if the name is empty or taken. Names ignore case, so two can't look
    // the same in a menu except for that, and are unique across the library, wherever they sit.
    qint64 create(const QString &name, std::optional<qint64> parentId = std::nullopt);
    void rename(qint64 id, const QString &name);
    // Collections inside it go with it. The publications stay in the library.
    void remove(qint64 id);
    // Puts a collection inside another, or at the top with none. Throws std::invalid_argument for a move
    // into itself or one inside it.
    void move(qint64 id, std::optional<qint64> parentId);
    // It and every collection inside it, at any depth.
    QList<qint64> withInside(qint64 id);
    bool nameTaken(const QString &name, qint64 exceptId = 0);

    // Each after the one it's inside, in order of their paths.
    QList<Collection> all();
    std::optional<Collection> get(qint64 id);

    // An issue's articles go with it, both ways.
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
