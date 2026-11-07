#pragma once

#include <QList>
#include <QPair>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// A summary the user chose to keep. It's kept apart from the publication's details and the user's notes, and
// says what made it, since it's generated and could be wrong.
struct StoredSummary {
    qint64 id = 0;
    QString uuid;
    qint64 publicationId = 0;
    QString model;
    QString effort;
    QString basis;
    bool identificationUnsure = false;
    QString text;
    QList<QPair<QString, QString>> sources; // Web address and title.
    std::optional<double> costDollars;
    bool sentPaper = false;
    qint64 createdAt = 0;
};

class SummaryRepository {
public:
    explicit SummaryRepository(Database &db) : m_db(db) {}

    // A publication has one, so this replaces any it had.
    void save(StoredSummary &summary);
    std::optional<StoredSummary> forPublication(qint64 publicationId);
    void remove(qint64 publicationId);

private:
    Database &m_db;
};

} // namespace papertrail::core
