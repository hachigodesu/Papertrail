#include "core/repo/TagRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QStringList>
#include <QUuid>

#include <stdexcept>

namespace papertrail::core {

namespace {

const auto kPathSeparator = QStringLiteral(" > ");

QString validName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        throw std::invalid_argument("a tag name can't be empty");
    if (trimmed.contains(u'>'))
        throw std::invalid_argument("a tag name can't contain >");
    return trimmed;
}

Tag readTag(const Statement &select)
{
    Tag tag;
    tag.id = select.int64At(0);
    tag.uuid = select.textAt(1);
    tag.name = select.textAt(2);
    tag.parentId = select.optInt64At(3);
    tag.colour = select.textAt(4);
    return tag;
}

} // namespace

qint64 TagRepository::create(const QString &name, std::optional<qint64> parentId, const QString &colour)
{
    const QString cleaned = validName(name);
    Statement &insert = m_db.cached(
        "INSERT INTO tag (uuid, name, parent_id, colour, created_at) VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, QUuid::createUuid().toString(QUuid::WithoutBraces));
    insert.bind(2, cleaned);
    insert.bind(3, parentId);
    insert.bind(4, colour.isEmpty() ? QString() : colour);
    insert.bind(5, QDateTime::currentMSecsSinceEpoch());
    insert.step();
    insert.reset();
    return m_db.lastInsertId();
}

qint64 TagRepository::ensurePath(const QString &path)
{
    const QStringList parts = path.split(u'>');
    Transaction transaction(m_db);
    std::optional<qint64> parent;
    for (const QString &part : parts) {
        const QString name = validName(part);
        Statement &select = m_db.cached(
            "SELECT id FROM tag WHERE name = ?1 COLLATE NOCASE AND parent_id IS ?2");
        select.bind(1, name);
        select.bind(2, parent);
        std::optional<qint64> existing;
        if (select.step())
            existing = select.int64At(0);
        select.reset();
        parent = existing ? *existing : create(name, parent);
    }
    transaction.commit();
    return *parent;
}

void TagRepository::rename(qint64 id, const QString &name)
{
    auto update = m_db.prepare("UPDATE tag SET name = ? WHERE id = ?");
    update->bind(1, validName(name));
    update->bind(2, id);
    update->step();
}

void TagRepository::setColour(qint64 id, const QString &colour)
{
    auto update = m_db.prepare("UPDATE tag SET colour = ? WHERE id = ?");
    update->bind(1, colour.isEmpty() ? QString() : colour);
    update->bind(2, id);
    update->step();
}

void TagRepository::move(qint64 id, std::optional<qint64> newParentId)
{
    if (newParentId && withDescendants(id).contains(*newParentId))
        throw std::invalid_argument("a tag can't be moved beneath itself");
    auto update = m_db.prepare("UPDATE tag SET parent_id = ? WHERE id = ?");
    update->bind(1, newParentId);
    update->bind(2, id);
    update->step();
}

void TagRepository::remove(qint64 id)
{
    auto remove = m_db.prepare("DELETE FROM tag WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

std::optional<Tag> TagRepository::get(qint64 id)
{
    auto select = m_db.prepare("SELECT id, uuid, name, parent_id, colour FROM tag WHERE id = ?");
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return readTag(*select);
}

QList<Tag> TagRepository::all()
{
    QList<Tag> tags;
    auto select = m_db.prepare(
        "SELECT id, uuid, name, parent_id, colour FROM tag ORDER BY name COLLATE NOCASE");
    while (select->step())
        tags << readTag(*select);
    return tags;
}

QString TagRepository::path(qint64 id)
{
    QStringList names;
    auto select = m_db.prepare(
        "WITH RECURSIVE up(id, name, parent_id, depth) AS ("
        "  SELECT id, name, parent_id, 0 FROM tag WHERE id = ? "
        "  UNION ALL "
        "  SELECT t.id, t.name, t.parent_id, up.depth + 1 FROM tag t JOIN up ON t.id = up.parent_id"
        ") SELECT name FROM up ORDER BY depth DESC");
    select->bind(1, id);
    while (select->step())
        names << select->textAt(0);
    return names.join(kPathSeparator);
}

QList<qint64> TagRepository::withDescendants(qint64 id)
{
    QList<qint64> ids;
    auto select = m_db.prepare(
        "WITH RECURSIVE down(id) AS ("
        "  SELECT ?1 UNION SELECT t.id FROM tag t JOIN down ON t.parent_id = down.id"
        ") SELECT id FROM down");
    select->bind(1, id);
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

void TagRepository::assign(qint64 publicationId, qint64 tagId)
{
    Statement &insert = m_db.cached(
        "INSERT OR IGNORE INTO publication_tag (publication_id, tag_id) VALUES (?, ?)");
    insert.bind(1, publicationId);
    insert.bind(2, tagId);
    insert.step();
    insert.reset();
}

void TagRepository::assign(const QList<qint64> &publicationIds, qint64 tagId)
{
    Transaction transaction(m_db);
    for (const qint64 id : publicationIds)
        assign(id, tagId);
    transaction.commit();
}

void TagRepository::unassign(qint64 publicationId, qint64 tagId)
{
    auto remove = m_db.prepare("DELETE FROM publication_tag WHERE publication_id = ? AND tag_id = ?");
    remove->bind(1, publicationId);
    remove->bind(2, tagId);
    remove->step();
}

QList<Tag> TagRepository::tagsFor(qint64 publicationId)
{
    QList<Tag> tags;
    auto select = m_db.prepare(
        "SELECT t.id, t.uuid, t.name, t.parent_id, t.colour FROM publication_tag pt "
        "JOIN tag t ON t.id = pt.tag_id WHERE pt.publication_id = ? ORDER BY t.name COLLATE NOCASE");
    select->bind(1, publicationId);
    while (select->step())
        tags << readTag(*select);
    return tags;
}

} // namespace papertrail::core
