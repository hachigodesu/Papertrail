#include "core/repo/NoticeRepository.h"

#include "core/db/Database.h"

#include <QSet>

namespace papertrail::core {

// The kinds that call a work's findings into question. Corrections and the like are listed, but not warned about.
const char *const kWarningKinds = "('retraction', 'partial_retraction', 'withdrawal', 'removal', 'expression_of_concern')";

bool Notice::isWarning() const
{
    static const QStringList kinds = {QStringLiteral("retraction"), QStringLiteral("partial_retraction"),
                                      QStringLiteral("withdrawal"), QStringLiteral("removal"),
                                      QStringLiteral("expression_of_concern")};
    return kinds.contains(kind);
}

QList<Notice> NoticeRepository::forPublication(qint64 publicationId)
{
    QList<Notice> notices;
    auto select = m_db.prepare("SELECT kind, label, notice_doi, notice_date, source FROM publication_notice "
                               "WHERE publication_id = ? ORDER BY notice_date, notice_doi");
    select->bind(1, publicationId);
    while (select->step())
        notices.append({select->textAt(0), select->textAt(1), select->textAt(2), select->textAt(3), select->textAt(4)});
    return notices;
}

QList<QPair<qint64, QString>> NoticeRepository::dueForCheck(int limit, qint64 checkedBefore)
{
    QList<QPair<qint64, QString>> due;
    auto select = m_db.prepare(
        "SELECT p.id, p.doi FROM publication p LEFT JOIN notice_check c ON c.publication_id = p.id "
        "WHERE p.doi IS NOT NULL AND p.doi <> '' AND p.id NOT IN (SELECT publication_id FROM hidden_publication) "
        "AND (c.checked_at IS NULL OR c.checked_at < ?) ORDER BY c.checked_at IS NOT NULL, c.checked_at, p.id LIMIT ?");
    select->bind(1, checkedBefore);
    select->bind(2, limit);
    while (select->step())
        due.append({select->int64At(0), select->textAt(1)});
    return due;
}

void NoticeRepository::record(const QList<qint64> &checked, const QHash<qint64, QList<Notice>> &found, qint64 when)
{
    // A notice found again keeps the time it was first found, which is how a new one is told from one already known.
    Transaction transaction(m_db);
    auto existing = m_db.prepare("SELECT notice_doi, kind FROM publication_notice WHERE publication_id = ?");
    auto drop = m_db.prepare("DELETE FROM publication_notice WHERE publication_id = ? AND notice_doi = ? AND kind = ?");
    auto insert = m_db.prepare(
        "INSERT INTO publication_notice (publication_id, notice_doi, kind, label, notice_date, source, found_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT (publication_id, notice_doi, kind) DO UPDATE SET "
        "label = excluded.label, notice_date = excluded.notice_date, source = excluded.source");
    auto mark = m_db.prepare("INSERT OR REPLACE INTO notice_check (publication_id, checked_at) VALUES (?, ?)");
    // A null string would be stored as NULL, which these don't take: unknown is empty.
    const auto text = [](const QString &value) { return value.isNull() ? QStringLiteral("") : value; };
    for (const qint64 id : checked) {
        const QList<Notice> now = found.value(id);
        QSet<QPair<QString, QString>> current;
        for (const Notice &notice : now)
            current.insert({text(notice.noticeDoi), notice.kind});

        QList<QPair<QString, QString>> gone;
        existing->reset();
        existing->bind(1, id);
        while (existing->step()) {
            const QPair<QString, QString> key{existing->textAt(0), existing->textAt(1)};
            if (!current.contains(key))
                gone << key;
        }
        for (const auto &key : gone) {
            drop->reset();
            drop->bind(1, id);
            drop->bind(2, key.first);
            drop->bind(3, key.second);
            drop->step();
        }
        for (const Notice &notice : now) {
            insert->reset();
            insert->bind(1, id);
            insert->bind(2, text(notice.noticeDoi));
            insert->bind(3, notice.kind);
            insert->bind(4, text(notice.label));
            insert->bind(5, text(notice.date));
            insert->bind(6, text(notice.source));
            insert->bind(7, when);
            insert->step();
        }
        mark->reset();
        mark->bind(1, id);
        mark->bind(2, when);
        mark->step();
    }
    transaction.commit();
}

QList<NewWarning> NoticeRepository::unseenWarnings(qint64 since)
{
    QList<NewWarning> warnings;
    auto select = m_db.prepare(
        QByteArray("SELECT n.publication_id, p.title, n.kind, n.label, n.notice_date, n.found_at "
                   "FROM publication_notice n JOIN publication p ON p.id = n.publication_id WHERE n.kind IN ")
        + kWarningKinds
        + " AND n.found_at > ? AND n.publication_id NOT IN (SELECT publication_id FROM hidden_publication) "
          "ORDER BY n.found_at DESC, p.title_sort, n.publication_id");
    select->bind(1, since);
    QSet<qint64> listed;
    while (select->step()) {
        // One line for each publication, with its newest warning.
        const qint64 id = select->int64At(0);
        if (listed.contains(id))
            continue;
        listed.insert(id);
        NewWarning warning;
        warning.publicationId = id;
        warning.title = select->textAt(1);
        warning.notice = {select->textAt(2), select->textAt(3), QString(), select->textAt(4), QString()};
        warning.foundAt = select->int64At(5);
        warnings << warning;
    }
    return warnings;
}

qint64 NoticeRepository::newestFoundAt()
{
    auto select = m_db.prepare("SELECT coalesce(max(found_at), 0) FROM publication_notice");
    select->step();
    return select->int64At(0);
}

qint64 NoticeRepository::warningCount()
{
    auto select = m_db.prepare(QByteArray("SELECT count(DISTINCT publication_id) FROM publication_notice WHERE kind IN ") + kWarningKinds
                               + " AND publication_id NOT IN (SELECT publication_id FROM hidden_publication)");
    select->step();
    return select->int64At(0);
}

} // namespace papertrail::core
