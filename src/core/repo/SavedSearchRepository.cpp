#include "core/repo/SavedSearchRepository.h"

#include "core/db/Database.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

#include <stdexcept>

namespace papertrail::core {

namespace {

SavedSearch savedSearchFrom(Statement &select)
{
    SavedSearch search;
    search.id = select.int64At(0);
    search.uuid = select.textAt(1);
    search.name = select.textAt(2);
    search.definition = QJsonDocument::fromJson(select.textAt(3).toUtf8()).object();
    search.createdAt = select.int64At(4);
    return search;
}

QString definitionText(const QJsonObject &definition)
{
    return QString::fromUtf8(QJsonDocument(definition).toJson(QJsonDocument::Compact));
}

QString textFor(Database &db, const QByteArray &sql, qint64 id)
{
    auto select = db.prepare(sql);
    select->bind(1, id);
    return select->step() ? select->textAt(0) : QString();
}

std::optional<qint64> idFor(Database &db, const QByteArray &sql, const QString &key)
{
    if (key.isEmpty())
        return std::nullopt;
    auto select = db.prepare(sql);
    select->bind(1, key);
    return select->step() ? std::optional<qint64>(select->int64At(0)) : std::nullopt;
}

} // namespace

QList<SavedSearch> SavedSearchRepository::all()
{
    QList<SavedSearch> found;
    auto select = m_db.prepare("SELECT id, uuid, name, definition, created_at FROM saved_search ORDER BY name COLLATE NOCASE, id");
    while (select->step())
        found << savedSearchFrom(*select);
    return found;
}

std::optional<SavedSearch> SavedSearchRepository::get(qint64 id)
{
    auto select = m_db.prepare("SELECT id, uuid, name, definition, created_at FROM saved_search WHERE id = ?");
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return savedSearchFrom(*select);
}

void SavedSearchRepository::checkName(const QString &name, qint64 exceptId)
{
    if (name.trimmed().isEmpty())
        throw std::invalid_argument("a saved search needs a name");
    auto select = m_db.prepare("SELECT 1 FROM saved_search WHERE name = ? COLLATE NOCASE AND id <> ?");
    select->bind(1, name.trimmed());
    select->bind(2, exceptId);
    if (select->step())
        throw std::invalid_argument("there's already a saved search with that name");
}

qint64 SavedSearchRepository::create(const QString &name, const QJsonObject &definition)
{
    checkName(name, 0);
    auto insert = m_db.prepare("INSERT INTO saved_search (uuid, name, definition, created_at) VALUES (?, ?, ?, ?)");
    insert->bind(1, QUuid::createUuid().toString(QUuid::WithoutBraces));
    insert->bind(2, name.trimmed());
    insert->bind(3, definitionText(definition));
    insert->bind(4, QDateTime::currentMSecsSinceEpoch());
    insert->step();
    return m_db.lastInsertId();
}

void SavedSearchRepository::rename(qint64 id, const QString &name)
{
    checkName(name, id);
    auto update = m_db.prepare("UPDATE saved_search SET name = ? WHERE id = ?");
    update->bind(1, name.trimmed());
    update->bind(2, id);
    update->step();
}

void SavedSearchRepository::setDefinition(qint64 id, const QJsonObject &definition)
{
    auto update = m_db.prepare("UPDATE saved_search SET definition = ? WHERE id = ?");
    update->bind(1, definitionText(definition));
    update->bind(2, id);
    update->step();
}

void SavedSearchRepository::remove(qint64 id)
{
    auto remove = m_db.prepare("DELETE FROM saved_search WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

QString SavedSearchRepository::tagKey(qint64 tagId)
{
    return textFor(m_db, "SELECT uuid FROM tag WHERE id = ?", tagId);
}

std::optional<qint64> SavedSearchRepository::tagFromKey(const QString &uuid)
{
    return idFor(m_db, "SELECT id FROM tag WHERE uuid = ?", uuid);
}

QString SavedSearchRepository::collectionKey(qint64 collectionId)
{
    return textFor(m_db, "SELECT uuid FROM collection WHERE id = ?", collectionId);
}

std::optional<qint64> SavedSearchRepository::collectionFromKey(const QString &uuid)
{
    return idFor(m_db, "SELECT id FROM collection WHERE uuid = ?", uuid);
}

QString SavedSearchRepository::personKey(qint64 personId)
{
    return textFor(m_db, "SELECT name_key FROM person WHERE id = ?", personId);
}

std::optional<qint64> SavedSearchRepository::personFromKey(const QString &nameKey)
{
    return idFor(m_db, "SELECT id FROM person WHERE name_key = ?", nameKey);
}

QString SavedSearchRepository::publicationKey(qint64 publicationId)
{
    return textFor(m_db, "SELECT uuid FROM publication WHERE id = ?", publicationId);
}

std::optional<qint64> SavedSearchRepository::publicationFromKey(const QString &uuid)
{
    return idFor(m_db, "SELECT id FROM publication WHERE uuid = ?", uuid);
}

} // namespace papertrail::core
