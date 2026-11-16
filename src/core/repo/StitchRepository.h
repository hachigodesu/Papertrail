#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// The order a book's parts are stitched in, for a book downloaded as a zip of PDFs. It belongs to the zip's contents,
// as highlights do, so it survives the zip moving. The JSON is import::orderToJson's, and the highlights and bookmarks
// set aside on parts left out are kept with it, to come back if those parts are put back.
struct StitchOrder {
    QString sha256;
    QByteArray partsJson;
    bool confirmed = false;
    QByteArray setAsideJson;
    qint64 updatedAt = 0;
};

class StitchRepository {
public:
    explicit StitchRepository(Database &db) : m_db(db) {}

    std::optional<StitchOrder> get(const QString &sha256);
    QList<StitchOrder> all();
    // Keeps the order, leaving what's set aside as it was.
    void set(const QString &sha256, const QByteArray &partsJson, bool confirmed);
    void setSetAside(const QString &sha256, const QByteArray &setAsideJson);
    void setConfirmed(const QString &sha256, bool confirmed);
    // For an export brought in from another computer: every field as it was there.
    void put(const StitchOrder &order);

    // Zips whose order the user hasn't confirmed yet, whose publications aren't hidden, newest first.
    QList<QString> unconfirmed();
    // The publication whose file has these contents, if any isn't hidden.
    std::optional<qint64> publicationFor(const QString &sha256);
    // The zip's contents, if this publication's file is a book in a zip whose order hasn't been confirmed.
    std::optional<QString> unconfirmedFor(qint64 publicationId);

    // Zips found not to be books, by place, size and date.
    bool isSkippedZip(qint64 rootId, const QString &relativePath, qint64 size, qint64 modifiedAt);
    void skipZip(qint64 rootId, const QString &relativePath, qint64 size, qint64 modifiedAt);

private:
    Database &m_db;
};

} // namespace papertrail::core
