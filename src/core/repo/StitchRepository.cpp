#include "core/repo/StitchRepository.h"

#include "core/db/Database.h"

#include <QDateTime>

namespace papertrail::core {

namespace {

StitchOrder readOrder(Statement &select)
{
    StitchOrder order;
    order.sha256 = select.textAt(0);
    order.partsJson = select.textAt(1).toUtf8();
    order.confirmed = select.intAt(2) != 0;
    order.setAsideJson = select.textAt(3).toUtf8();
    order.updatedAt = select.int64At(4);
    return order;
}

const char *const kColumns = "SELECT sha256, parts_json, confirmed, set_aside_json, updated_at FROM stitch_order ";

} // namespace

std::optional<StitchOrder> StitchRepository::get(const QString &sha256)
{
    auto select = m_db.prepare(QByteArray(kColumns) + "WHERE sha256 = ?");
    select->bind(1, sha256);
    if (!select->step())
        return std::nullopt;
    return readOrder(*select);
}

QList<StitchOrder> StitchRepository::all()
{
    auto select = m_db.prepare(QByteArray(kColumns) + "ORDER BY sha256");
    QList<StitchOrder> orders;
    while (select->step())
        orders << readOrder(*select);
    return orders;
}

void StitchRepository::set(const QString &sha256, const QByteArray &partsJson, bool confirmed)
{
    auto upsert = m_db.prepare("INSERT INTO stitch_order (sha256, parts_json, confirmed, updated_at) VALUES (?, ?, ?, ?) "
                               "ON CONFLICT (sha256) DO UPDATE SET parts_json = excluded.parts_json, "
                               "confirmed = excluded.confirmed, updated_at = excluded.updated_at");
    upsert->bind(1, sha256);
    upsert->bind(2, QString::fromUtf8(partsJson));
    upsert->bind(3, confirmed ? 1 : 0);
    upsert->bind(4, QDateTime::currentMSecsSinceEpoch());
    upsert->step();
}

void StitchRepository::setSetAside(const QString &sha256, const QByteArray &setAsideJson)
{
    auto update = m_db.prepare("UPDATE stitch_order SET set_aside_json = ?, updated_at = ? WHERE sha256 = ?");
    update->bind(1, QString::fromUtf8(setAsideJson.isEmpty() ? QByteArray("{}") : setAsideJson));
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, sha256);
    update->step();
}

void StitchRepository::setConfirmed(const QString &sha256, bool confirmed)
{
    auto update = m_db.prepare("UPDATE stitch_order SET confirmed = ?, updated_at = ? WHERE sha256 = ?");
    update->bind(1, confirmed ? 1 : 0);
    update->bind(2, QDateTime::currentMSecsSinceEpoch());
    update->bind(3, sha256);
    update->step();
}

void StitchRepository::put(const StitchOrder &order)
{
    auto upsert = m_db.prepare("INSERT OR REPLACE INTO stitch_order (sha256, parts_json, confirmed, set_aside_json, updated_at) "
                               "VALUES (?, ?, ?, ?, ?)");
    upsert->bind(1, order.sha256);
    upsert->bind(2, QString::fromUtf8(order.partsJson));
    upsert->bind(3, order.confirmed ? 1 : 0);
    upsert->bind(4, QString::fromUtf8(order.setAsideJson.isEmpty() ? QByteArray("{}") : order.setAsideJson));
    upsert->bind(5, order.updatedAt);
    upsert->step();
}

QList<QString> StitchRepository::unconfirmed()
{
    auto select = m_db.prepare(
        "SELECT s.sha256 FROM stitch_order s WHERE s.confirmed = 0 AND EXISTS ("
        "  SELECT 1 FROM file f JOIN publication_file pf ON pf.file_id = f.id "
        "  WHERE f.sha256 = s.sha256 AND pf.publication_id NOT IN (SELECT publication_id FROM hidden_publication)) "
        "ORDER BY s.updated_at DESC, s.sha256");
    QList<QString> hashes;
    while (select->step())
        hashes << select->textAt(0);
    return hashes;
}

std::optional<qint64> StitchRepository::publicationFor(const QString &sha256)
{
    auto select = m_db.prepare("SELECT pf.publication_id FROM file f JOIN publication_file pf ON pf.file_id = f.id "
                               "WHERE f.sha256 = ? AND pf.publication_id NOT IN (SELECT publication_id FROM hidden_publication) "
                               "ORDER BY pf.is_primary DESC, pf.publication_id LIMIT 1");
    select->bind(1, sha256);
    if (!select->step())
        return std::nullopt;
    return select->int64At(0);
}

std::optional<QString> StitchRepository::unconfirmedFor(qint64 publicationId)
{
    auto select = m_db.prepare("SELECT s.sha256 FROM publication_file pf JOIN file f ON f.id = pf.file_id "
                               "JOIN stitch_order s ON s.sha256 = f.sha256 WHERE pf.publication_id = ? AND s.confirmed = 0 "
                               "ORDER BY pf.is_primary DESC LIMIT 1");
    select->bind(1, publicationId);
    if (!select->step())
        return std::nullopt;
    return select->textAt(0);
}

bool StitchRepository::isSkippedZip(qint64 rootId, const QString &relativePath, qint64 size, qint64 modifiedAt)
{
    auto select = m_db.prepare("SELECT 1 FROM skipped_zip WHERE root_id = ? AND relative_path = ? AND size = ? "
                               "AND modified_at = ?");
    select->bind(1, rootId);
    select->bind(2, relativePath);
    select->bind(3, size);
    select->bind(4, modifiedAt);
    return select->step();
}

void StitchRepository::skipZip(qint64 rootId, const QString &relativePath, qint64 size, qint64 modifiedAt)
{
    auto upsert = m_db.prepare("INSERT OR REPLACE INTO skipped_zip (root_id, relative_path, size, modified_at) "
                               "VALUES (?, ?, ?, ?)");
    upsert->bind(1, rootId);
    upsert->bind(2, relativePath);
    upsert->bind(3, size);
    upsert->bind(4, modifiedAt);
    upsert->step();
}

} // namespace papertrail::core
