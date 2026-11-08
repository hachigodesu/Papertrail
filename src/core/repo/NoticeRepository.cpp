#include "core/repo/NoticeRepository.h"

#include "core/db/Database.h"

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
    Transaction transaction(m_db);
    auto clear = m_db.prepare("DELETE FROM publication_notice WHERE publication_id = ?");
    auto insert = m_db.prepare(
        "INSERT OR REPLACE INTO publication_notice (publication_id, notice_doi, kind, label, notice_date, source, found_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");
    auto mark = m_db.prepare("INSERT OR REPLACE INTO notice_check (publication_id, checked_at) VALUES (?, ?)");
    for (const qint64 id : checked) {
        clear->reset();
        clear->bind(1, id);
        clear->step();
        for (const Notice &notice : found.value(id)) {
            insert->reset();
            insert->bind(1, id);
            insert->bind(2, notice.noticeDoi);
            insert->bind(3, notice.kind);
            // A null string would be stored as NULL, which these don't take: unknown is empty.
            const auto text = [](const QString &value) { return value.isNull() ? QStringLiteral("") : value; };
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

qint64 NoticeRepository::warningCount()
{
    auto select = m_db.prepare(QByteArray("SELECT count(DISTINCT publication_id) FROM publication_notice WHERE kind IN ") + kWarningKinds
                               + " AND publication_id NOT IN (SELECT publication_id FROM hidden_publication)");
    select->step();
    return select->int64At(0);
}

} // namespace papertrail::core
