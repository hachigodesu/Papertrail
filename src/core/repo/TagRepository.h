#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

struct Tag {
    qint64 id = 0;
    QString uuid;
    QString name;
    std::optional<qint64> parentId;
    QString colour;
};

class TagRepository {
public:
    explicit TagRepository(Database &db) : m_db(db) {}

    // Names can't be empty or contain ">", which separates levels in a tag path.
    qint64 create(const QString &name, std::optional<qint64> parentId = std::nullopt,
                  const QString &colour = QString());

    // Finds or creates each level of a path such as "Engineering > Structures > FEM",
    // and returns the last tag's id.
    qint64 ensurePath(const QString &path);

    void rename(qint64 id, const QString &name);
    void setColour(qint64 id, const QString &colour);

    // Moves a tag and everything under it. Refuses to move a tag beneath itself.
    void move(qint64 id, std::optional<qint64> newParentId);

    // Removes a tag and everything under it. Publications stay, without those tags.
    void remove(qint64 id);

    std::optional<Tag> get(qint64 id);
    QList<Tag> all();
    QString path(qint64 id); // "Engineering > Structures > FEM"
    QList<qint64> withDescendants(qint64 id);

    void assign(qint64 publicationId, qint64 tagId);
    void assign(const QList<qint64> &publicationIds, qint64 tagId);
    void unassign(qint64 publicationId, qint64 tagId);
    QList<Tag> tagsFor(qint64 publicationId);

private:
    Database &m_db;
};

} // namespace papertrail::core
