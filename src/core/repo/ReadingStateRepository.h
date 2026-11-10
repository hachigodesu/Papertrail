#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace papertrail::core {

class Database;

// How far through a publication has been read, from 0 to 1, from its reading state's location. Each format's place
// says how far it reached, and the one saved last counts. A place saved before that was kept only says which page it
// was on, which is measured against an article's pages of its file, from `pageStart` to `pageEnd`, or else the file's
// page count. Null if there's nothing to tell from.
std::optional<double> readingProgress(const QString &location, int pageStart, int pageEnd, std::optional<int> pageCount);

struct ReadingState {
    QString location; // A place per format. See placeFor.
    std::optional<double> zoom; // Only in places saved before formats were kept apart.
    qint64 lastOpened = 0;
};

// Where the reader left off in each publication. Page 40 of a PDF and page 40 of an EPUB are different
// places, so each format of a publication has a place of its own. What's in a place is up to the reader.
class ReadingStateRepository {
public:
    explicit ReadingStateRepository(Database &db) : m_db(db) {}

    std::optional<ReadingState> get(qint64 publicationId);

    // Empty if this format hasn't been read yet. A place saved before formats were kept apart applies
    // to any of them, until the next save.
    QJsonObject placeFor(qint64 publicationId, const QString &format);
    void setPlaceFor(qint64 publicationId, const QString &format, const QJsonObject &place, qint64 lastOpened);

    // Says which format a place from before formats were kept apart belongs to.
    void assignToFormat(qint64 publicationId, const QString &format);

    // For merging: takes on another publication's places, and keeps its own where both have one.
    void absorb(qint64 publicationId, qint64 otherId);

    // For separating: hands one format's place to another publication.
    void moveFormat(qint64 fromId, qint64 toId, const QString &format);

private:
    void write(qint64 publicationId, const QJsonObject &formats, qint64 lastOpened);

    Database &m_db;
};

} // namespace papertrail::core
