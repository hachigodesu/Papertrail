#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// A search kept by name, with its filters. Its definition is the window's to read, and refers to tags,
// collections and people by names that mean the same in another library (see the *Key functions).
struct SavedSearch {
    qint64 id = 0;
    QString uuid;
    QString name;
    QJsonObject definition;
    qint64 createdAt = 0;
};

class SavedSearchRepository {
public:
    explicit SavedSearchRepository(Database &db) : m_db(db) {}

    // By name, ignoring case.
    QList<SavedSearch> all();
    std::optional<SavedSearch> get(qint64 id);
    // Throws std::invalid_argument for an empty name or one that's taken.
    qint64 create(const QString &name, const QJsonObject &definition);
    void rename(qint64 id, const QString &name);
    void setDefinition(qint64 id, const QJsonObject &definition);
    void remove(qint64 id);

    // For a definition: tags and collections by uuid, people by their name key. Empty or nothing where
    // there isn't one, such as a tag deleted since.
    QString tagKey(qint64 tagId);
    std::optional<qint64> tagFromKey(const QString &uuid);
    QString collectionKey(qint64 collectionId);
    std::optional<qint64> collectionFromKey(const QString &uuid);
    QString personKey(qint64 personId);
    std::optional<qint64> personFromKey(const QString &nameKey);
    // A publication, such as the issue whose articles are shown, by its uuid.
    QString publicationKey(qint64 publicationId);
    std::optional<qint64> publicationFromKey(const QString &uuid);

private:
    void checkName(const QString &name, qint64 exceptId);

    Database &m_db;
};

} // namespace papertrail::core
