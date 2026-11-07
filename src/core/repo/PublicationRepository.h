#pragma once

#include "core/model/Publication.h"

#include <QList>

#include <optional>

namespace papertrail::core {

class Database;

struct ProvenanceEntry {
    QString field;
    QString source;
    std::optional<double> confidence;
    QString previousValue;
    QString newValue;
    qint64 recordedAt = 0;
};

class PublicationRepository {
public:
    explicit PublicationRepository(Database &db) : m_db(db) {}

    // Saves a new publication and fills in its id, uuid and timestamps. A uuid already set is
    // kept, which is how an imported library keeps its identity.
    qint64 create(Publication &publication);

    // Replaces the stored record, including its people, and refreshes updatedAt.
    void update(Publication &publication);

    // Copies across whatever `from` has that `into` doesn't. Nothing already there is replaced.
    static void fillGaps(Publication &into, const Publication &from);

    // As update(), and also keeps each changed field's earlier value under `source`.
    // `recordedAt` lets several changes count as one, for undoing them together.
    void updateWithProvenance(Publication &publication, const QString &source, qint64 recordedAt = 0);

    // For many publications at once, without rewriting each one's people.
    void setReadStatus(const QList<qint64> &ids, ReadStatus status);
    void setNeedsReview(const QList<qint64> &ids, bool needsReview);

    void remove(qint64 id);

    std::optional<Publication> get(qint64 id);
    std::optional<qint64> idForUuid(const QString &uuid);
    QList<qint64> childIds(qint64 parentId);
    qint64 count();

    // Keeps the earlier value of a field, so an automatic change can be undone.
    void recordProvenance(qint64 publicationId, const ProvenanceEntry &entry);
    QList<ProvenanceEntry> provenance(qint64 publicationId, const QString &field = QString());

private:
    void validate(const Publication &publication);
    void writePeople(qint64 id, const Publication &publication);
    QList<PersonName> readPeople(qint64 id, PersonRole role);
    qint64 personId(const PersonName &name);

    Database &m_db;
};

} // namespace papertrail::core
