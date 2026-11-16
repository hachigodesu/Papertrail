#include "import/Restitch.h"

#include "core/db/Database.h"
#include "core/repo/ReadingStateRepository.h"
#include "core/repo/StitchRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace papertrail::import {

using core::Database;
using core::Statement;

namespace {

// A row of a table as a JSON object, with every column, so it can be put back exactly as it was. `table` and its
// columns come from this file, never from anything read.
QByteArray rowSelect(Database &db, const char *table, QStringList *columnsOut = nullptr)
{
    QStringList columns;
    auto info = db.prepare(QByteArray("PRAGMA table_info(") + table + ")");
    while (info->step())
        columns << info->textAt(1);
    QStringList pairs;
    for (const QString &column : std::as_const(columns))
        pairs << QStringLiteral("'%1', %1").arg(column);
    if (columnsOut)
        *columnsOut = columns;
    return QStringLiteral("SELECT id, json_object(%1) FROM %2 WHERE content_hash = ?").arg(pairs.join(u','), QLatin1StringView(table)).toUtf8();
}

// Whether it went back: not if its publication's gone since, or the same one's come back another way, as from an export.
bool putRowBack(Database &db, const char *table, const QStringList &columns, const QJsonObject &row)
{
    QStringList names;
    QStringList values;
    for (const QString &column : columns) {
        if (column == QLatin1StringView("id"))
            continue; // It gets a new one.
        names << column;
        values << QStringLiteral("json_extract(?1, '$.%1')").arg(column);
    }
    auto insert = db.prepare(QStringLiteral("INSERT OR IGNORE INTO %1 (%2) SELECT %3 WHERE EXISTS "
                                            "(SELECT 1 FROM publication WHERE id = json_extract(?1, '$.publication_id'))")
                                 .arg(QLatin1StringView(table), names.join(u','), values.join(u','))
                                 .toUtf8());
    insert->bind(1, QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact)));
    insert->step();
    return db.changes() > 0;
}

void deleteRow(Database &db, const char *table, qint64 id)
{
    auto remove = db.prepare(QByteArray("DELETE FROM ") + table + " WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

int mapped(const QList<int> &mapping, int page)
{
    return page >= 0 && page < mapping.size() ? mapping.at(page) : page;
}

} // namespace

QList<qint64> publicationsWithContents(Database &db, const QString &zipSha256)
{
    auto select = db.prepare("SELECT DISTINCT pf.publication_id FROM publication_file pf JOIN file f ON f.id = pf.file_id "
                             "WHERE f.sha256 = ? ORDER BY pf.publication_id");
    select->bind(1, zipSha256);
    QList<qint64> ids;
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

int marksLeftOut(Database &db, const QString &zipSha256, const QList<OrderedPart> &to)
{
    const std::optional<core::StitchOrder> kept = core::StitchRepository(db).get(zipSha256);
    if (!kept)
        return 0;
    const QList<int> mapping = pageMapping(orderFromJson(kept->partsJson), to);
    int count = 0;
    auto annotations = db.prepare("SELECT location_json FROM annotation WHERE content_hash = ?");
    annotations->bind(1, zipSha256);
    while (annotations->step()) {
        const QJsonArray parts = QJsonDocument::fromJson(annotations->textAt(0).toUtf8()).object().value(QStringLiteral("parts")).toArray();
        for (const QJsonValue &part : parts) {
            if (part.toObject().contains(QStringLiteral("page")) && mapped(mapping, part.toObject().value(QStringLiteral("page")).toInt()) < 0) {
                ++count;
                break;
            }
        }
    }
    auto bookmarks = db.prepare("SELECT page FROM bookmark WHERE content_hash = ?");
    bookmarks->bind(1, zipSha256);
    while (bookmarks->step()) {
        if (mapped(mapping, bookmarks->intAt(0)) < 0)
            ++count;
    }
    return count;
}

RestitchCounts applyStitchOrder(Database &db, const QString &zipSha256, const QList<OrderedPart> &to, bool confirmed)
{
    RestitchCounts counts;
    core::Transaction transaction(db);
    core::StitchRepository orders(db);
    const std::optional<core::StitchOrder> kept = orders.get(zipSha256);
    const QList<OrderedPart> from = kept ? orderFromJson(kept->partsJson) : QList<OrderedPart>();
    const QList<int> mapping = pageMapping(from, to);
    QJsonObject setAside = kept ? QJsonDocument::fromJson(kept->setAsideJson).object() : QJsonObject();
    QJsonArray asideAnnotations = setAside.value(QStringLiteral("annotations")).toArray();
    QJsonArray asideBookmarks = setAside.value(QStringLiteral("bookmarks")).toArray();

    // Highlights and notes: each part of one moves with its page, and one with any part on a page left out is set aside
    // whole, each part noting which part of the book and which page of it it was on.
    if (!from.isEmpty()) {
        QStringList columns;
        auto annotations = db.prepare(rowSelect(db, "annotation", &columns));
        annotations->bind(1, zipSha256);
        QList<QPair<qint64, QJsonObject>> rows;
        while (annotations->step())
            rows << qMakePair(annotations->int64At(0), QJsonDocument::fromJson(annotations->textAt(1).toUtf8()).object());
        for (auto &[id, row] : rows) {
            QJsonObject location = QJsonDocument::fromJson(row.value(QStringLiteral("location_json")).toString().toUtf8()).object();
            QJsonArray parts = location.value(QStringLiteral("parts")).toArray();
            bool leftOut = false;
            bool moved = false;
            QJsonArray places;
            for (qsizetype i = 0; i < parts.size(); ++i) {
                QJsonObject part = parts.at(i).toObject();
                if (!part.contains(QStringLiteral("page")))
                    continue;
                const int page = part.value(QStringLiteral("page")).toInt();
                QString partName;
                int pageInPart = 0;
                if (partOfPage(from, page, partName, pageInPart))
                    places.append(QJsonObject{{QStringLiteral("part"), partName}, {QStringLiteral("page"), pageInPart}});
                else
                    places.append(QJsonObject{{QStringLiteral("part"), QString()}, {QStringLiteral("page"), page}});
                const int now = mapped(mapping, page);
                if (now < 0)
                    leftOut = true;
                else if (now != page)
                    moved = true;
                part.insert(QStringLiteral("page"), now);
                parts.replace(i, part);
            }
            if (leftOut) {
                row.insert(QStringLiteral("parts_in_book"), places);
                asideAnnotations.append(row);
                deleteRow(db, "annotation", id);
                ++counts.setAside;
            } else if (moved) {
                location.insert(QStringLiteral("parts"), parts);
                auto update = db.prepare("UPDATE annotation SET location_json = ? WHERE id = ?");
                update->bind(1, QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact)));
                update->bind(2, id);
                update->step();
                ++counts.moved;
            }
        }

        auto bookmarks = db.prepare(rowSelect(db, "bookmark"));
        bookmarks->bind(1, zipSha256);
        QList<QPair<qint64, QJsonObject>> marks;
        while (bookmarks->step())
            marks << qMakePair(bookmarks->int64At(0), QJsonDocument::fromJson(bookmarks->textAt(1).toUtf8()).object());
        for (auto &[id, row] : marks) {
            const int page = row.value(QStringLiteral("page")).toInt();
            const int now = mapped(mapping, page);
            if (now < 0) {
                QString partName;
                int pageInPart = 0;
                partOfPage(from, page, partName, pageInPart);
                row.insert(QStringLiteral("part_in_book"), QJsonObject{{QStringLiteral("part"), partName}, {QStringLiteral("page"), pageInPart}});
                asideBookmarks.append(row);
                deleteRow(db, "bookmark", id);
                ++counts.setAside;
            } else if (now != page) {
                auto update = db.prepare("UPDATE bookmark SET page = ? WHERE id = ?");
                update->bind(1, now);
                update->bind(2, id);
                update->step();
                ++counts.moved;
            }
        }
    }

    // Those set aside earlier come back if every part of the book they were on is in again.
    {
        QStringList columns;
        rowSelect(db, "annotation", &columns);
        QJsonArray stillAside;
        for (const QJsonValue &value : std::as_const(asideAnnotations)) {
            QJsonObject row = value.toObject();
            const QJsonArray places = row.value(QStringLiteral("parts_in_book")).toArray();
            QJsonObject location = QJsonDocument::fromJson(row.value(QStringLiteral("location_json")).toString().toUtf8()).object();
            QJsonArray parts = location.value(QStringLiteral("parts")).toArray();
            bool back = true;
            qsizetype placeIndex = 0;
            for (qsizetype i = 0; i < parts.size() && back; ++i) {
                QJsonObject part = parts.at(i).toObject();
                if (!part.contains(QStringLiteral("page")))
                    continue;
                const QJsonObject place = places.at(placeIndex++).toObject();
                const int page = pageOfPart(to, place.value(QStringLiteral("part")).toString(), place.value(QStringLiteral("page")).toInt());
                if (page < 0) {
                    back = false;
                    break;
                }
                part.insert(QStringLiteral("page"), page);
                parts.replace(i, part);
            }
            if (!back) {
                stillAside.append(row);
                continue;
            }
            location.insert(QStringLiteral("parts"), parts);
            row.insert(QStringLiteral("location_json"), QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact)));
            row.remove(QStringLiteral("parts_in_book"));
            if (putRowBack(db, "annotation", columns, row))
                ++counts.broughtBack;
        }
        asideAnnotations = stillAside;
    }
    {
        QStringList columns;
        rowSelect(db, "bookmark", &columns);
        QJsonArray stillAside;
        for (const QJsonValue &value : std::as_const(asideBookmarks)) {
            QJsonObject row = value.toObject();
            const QJsonObject place = row.value(QStringLiteral("part_in_book")).toObject();
            const int page = pageOfPart(to, place.value(QStringLiteral("part")).toString(), place.value(QStringLiteral("page")).toInt());
            if (page < 0) {
                stillAside.append(row);
                continue;
            }
            row.insert(QStringLiteral("page"), page);
            row.remove(QStringLiteral("part_in_book"));
            if (putRowBack(db, "bookmark", columns, row))
                ++counts.broughtBack;
        }
        asideBookmarks = stillAside;
    }

    // Where each publication with this zip was being read, and the pages of the chapters inside them.
    if (!from.isEmpty()) {
        core::ReadingStateRepository states(db);
        for (const qint64 publication : publicationsWithContents(db, zipSha256)) {
            QJsonObject place = states.placeFor(publication, QStringLiteral("zip"));
            if (place.contains(QStringLiteral("page"))) {
                const int now = mapped(mapping, place.value(QStringLiteral("page")).toInt());
                place.insert(QStringLiteral("page"), qMax(0, now));
                if (now < 0)
                    place.remove(QStringLiteral("offset"));
                const std::optional<core::ReadingState> state = states.get(publication);
                states.setPlaceFor(publication, QStringLiteral("zip"), place, state ? state->lastOpened : 0);
            }

            auto chapters = db.prepare("SELECT id, page_start, page_end FROM publication WHERE parent_id = ?");
            chapters->bind(1, publication);
            QList<std::tuple<qint64, std::optional<int>, std::optional<int>>> found;
            while (chapters->step())
                found << std::make_tuple(chapters->int64At(0), chapters->optIntAt(1), chapters->optIntAt(2));
            for (const auto &[id, start, end] : found) {
                if (!start)
                    continue;
                // Pages here count from 1. A chapter on a part left out keeps its pages, in case the part comes back.
                const int newStart = mapped(mapping, *start - 1);
                if (newStart < 0)
                    continue;
                int newEnd = newStart;
                if (end) {
                    for (int page = *end - 1; page >= *start - 1; --page) {
                        const int candidate = mapped(mapping, page);
                        if (candidate >= newStart) {
                            newEnd = candidate;
                            break;
                        }
                    }
                }
                auto update = db.prepare("UPDATE publication SET page_start = ?, page_end = ? WHERE id = ?");
                update->bind(1, newStart + 1);
                update->bind(2, newEnd + 1);
                update->bind(3, id);
                update->step();
            }
        }
    }

    // The stitched book's pages, as the library counts them.
    int pages = 0;
    for (const OrderedPart &part : to) {
        if (part.included)
            pages += qMax(0, part.pageCount);
    }
    if (pages > 0) {
        auto count = db.prepare("UPDATE file SET page_count = ? WHERE sha256 = ?");
        count->bind(1, pages);
        count->bind(2, zipSha256);
        count->step();
    }

    orders.set(zipSha256, orderToJson(to), confirmed);
    setAside.insert(QStringLiteral("annotations"), asideAnnotations);
    setAside.insert(QStringLiteral("bookmarks"), asideBookmarks);
    orders.setSetAside(zipSha256, QJsonDocument(setAside).toJson(QJsonDocument::Compact));
    transaction.commit();
    return counts;
}

} // namespace papertrail::import
