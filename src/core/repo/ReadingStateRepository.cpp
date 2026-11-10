#include "core/repo/ReadingStateRepository.h"

#include "core/db/Database.h"

#include <QJsonDocument>

namespace papertrail::core {

namespace {

const QLatin1StringView kFormats("formats");

QJsonObject parsed(const ReadingState &state)
{
    return QJsonDocument::fromJson(state.location.toUtf8()).object();
}

// A place from before formats were kept apart: the location itself, with the zoom in its own column.
QJsonObject legacyPlace(const ReadingState &state)
{
    QJsonObject place = parsed(state);
    if (state.zoom && !place.contains(QLatin1StringView("zoom")))
        place.insert(QStringLiteral("zoom"), *state.zoom);
    return place;
}

bool hasFormats(const QJsonObject &location)
{
    return location.contains(kFormats);
}

} // namespace

std::optional<double> readingProgress(const QString &location, int pageStart, int pageEnd, std::optional<int> pageCount)
{
    const QJsonObject parsedLocation = QJsonDocument::fromJson(location.toUtf8()).object();
    QList<QJsonObject> places;
    if (hasFormats(parsedLocation)) {
        const QJsonObject formats = parsedLocation.value(kFormats).toObject();
        for (auto it = formats.begin(); it != formats.end(); ++it)
            places << it.value().toObject();
    } else if (!parsedLocation.isEmpty()) {
        places << parsedLocation;
    }

    std::optional<double> best;
    double bestSavedAt = -1;
    for (const QJsonObject &place : std::as_const(places)) {
        std::optional<double> progress;
        if (place.contains(QLatin1StringView("reached"))) {
            progress = place.value(QLatin1StringView("reached")).toDouble();
        } else if (place.contains(QLatin1StringView("page"))) {
            // Pages count from 0 in a place, and from 1 in an article's range.
            const int page = place.value(QLatin1StringView("page")).toInt() + 1;
            if (pageStart > 0 && pageEnd >= pageStart)
                progress = double(page - pageStart + 1) / double(pageEnd - pageStart + 1);
            else if (pageCount && *pageCount > 0)
                progress = double(page) / double(*pageCount);
        }
        if (!progress)
            continue;
        const double savedAt = place.value(QLatin1StringView("savedAt")).toDouble();
        if (!best || savedAt > bestSavedAt || (savedAt == bestSavedAt && *progress > *best)) {
            best = qBound(0.0, *progress, 1.0);
            bestSavedAt = savedAt;
        }
    }
    return best;
}

std::optional<ReadingState> ReadingStateRepository::get(qint64 publicationId)
{
    auto select = m_db.prepare("SELECT location, zoom, last_opened FROM reading_state WHERE publication_id = ?");
    select->bind(1, publicationId);
    if (!select->step())
        return std::nullopt;
    ReadingState state;
    state.location = select->textAt(0);
    if (!select->isNull(1))
        state.zoom = select->doubleAt(1);
    state.lastOpened = select->optInt64At(2).value_or(0);
    return state;
}

QJsonObject ReadingStateRepository::placeFor(qint64 publicationId, const QString &format)
{
    const std::optional<ReadingState> state = get(publicationId);
    if (!state)
        return {};
    const QJsonObject location = parsed(*state);
    if (hasFormats(location))
        return location.value(kFormats).toObject().value(format).toObject();
    return legacyPlace(*state);
}

void ReadingStateRepository::write(qint64 publicationId, const QJsonObject &formats, qint64 lastOpened)
{
    QJsonObject location;
    location.insert(kFormats, formats);
    auto upsert = m_db.prepare(
        "INSERT INTO reading_state (publication_id, location, zoom, last_opened) VALUES (?, ?, NULL, ?) "
        "ON CONFLICT (publication_id) DO UPDATE SET location = excluded.location, zoom = NULL, "
        "last_opened = excluded.last_opened");
    upsert->bind(1, publicationId);
    upsert->bind(2, QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact)));
    upsert->bind(3, lastOpened);
    upsert->step();
}

void ReadingStateRepository::setPlaceFor(qint64 publicationId, const QString &format, const QJsonObject &place,
                                         qint64 lastOpened)
{
    Transaction transaction(m_db);
    assignToFormat(publicationId, format);
    const std::optional<ReadingState> state = get(publicationId);
    QJsonObject formats = state ? parsed(*state).value(kFormats).toObject() : QJsonObject();
    formats.insert(format, place);
    write(publicationId, formats, lastOpened);
    transaction.commit();
}

void ReadingStateRepository::assignToFormat(qint64 publicationId, const QString &format)
{
    const std::optional<ReadingState> state = get(publicationId);
    if (!state || hasFormats(parsed(*state)))
        return;
    QJsonObject formats;
    const QJsonObject place = legacyPlace(*state);
    if (!place.isEmpty())
        formats.insert(format, place);
    write(publicationId, formats, state->lastOpened);
}

void ReadingStateRepository::absorb(qint64 publicationId, qint64 otherId)
{
    const std::optional<ReadingState> other = get(otherId);
    if (!other)
        return;
    const std::optional<ReadingState> own = get(publicationId);
    QJsonObject formats = own ? parsed(*own).value(kFormats).toObject() : QJsonObject();
    const QJsonObject theirs = parsed(*other).value(kFormats).toObject();
    for (auto it = theirs.begin(); it != theirs.end(); ++it) {
        if (!formats.contains(it.key()))
            formats.insert(it.key(), it.value());
    }
    write(publicationId, formats, qMax(own ? own->lastOpened : 0, other->lastOpened));
}

void ReadingStateRepository::moveFormat(qint64 fromId, qint64 toId, const QString &format)
{
    const std::optional<ReadingState> from = get(fromId);
    if (!from)
        return;
    assignToFormat(fromId, format);
    const std::optional<ReadingState> current = get(fromId);
    QJsonObject formats = parsed(*current).value(kFormats).toObject();
    if (!formats.contains(format))
        return;
    const QJsonObject place = formats.value(format).toObject();
    formats.remove(format);
    write(fromId, formats, current->lastOpened);
    write(toId, QJsonObject{{format, place}}, current->lastOpened);
}

} // namespace papertrail::core
