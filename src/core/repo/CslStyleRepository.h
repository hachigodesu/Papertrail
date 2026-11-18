#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// A CSL style added to this library from a file. Checking that it's a style is citation::readCslStyle's job, done
// before it's added here.
struct AddedCslStyle {
    QString id; // Its info/id.
    QString title;
    QByteArray xml;
    qint64 addedAt = 0;
};

class CslStyleRepository {
public:
    explicit CslStyleRepository(Database &db) : m_db(db) {}

    // By title, without their XML.
    QList<AddedCslStyle> all();
    std::optional<AddedCslStyle> get(const QString &id);
    QByteArray xml(const QString &id); // Empty if it isn't here.
    // Adds a style, or replaces the one with the same id, such as a newer version of it.
    void put(const QString &id, const QString &title, const QByteArray &xml, qint64 addedAt = 0);
    void remove(const QString &id);

private:
    Database &m_db;
};

} // namespace papertrail::core
