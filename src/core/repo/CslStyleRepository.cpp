#include "core/repo/CslStyleRepository.h"

#include "core/db/Database.h"

#include <QDateTime>

#include <stdexcept>

namespace papertrail::core {

QList<AddedCslStyle> CslStyleRepository::all()
{
    QList<AddedCslStyle> styles;
    auto select = m_db.prepare("SELECT id, title, added_at FROM csl_style ORDER BY title COLLATE NOCASE, id");
    while (select->step())
        styles << AddedCslStyle{select->textAt(0), select->textAt(1), {}, select->int64At(2)};
    return styles;
}

std::optional<AddedCslStyle> CslStyleRepository::get(const QString &id)
{
    auto select = m_db.prepare("SELECT id, title, xml, added_at FROM csl_style WHERE id = ?");
    select->bind(1, id);
    if (!select->step())
        return std::nullopt;
    return AddedCslStyle{select->textAt(0), select->textAt(1), select->textAt(2).toUtf8(), select->int64At(3)};
}

QByteArray CslStyleRepository::xml(const QString &id)
{
    auto select = m_db.prepare("SELECT xml FROM csl_style WHERE id = ?");
    select->bind(1, id);
    return select->step() ? select->textAt(0).toUtf8() : QByteArray();
}

void CslStyleRepository::put(const QString &id, const QString &title, const QByteArray &xml, qint64 addedAt)
{
    if (id.trimmed().isEmpty() || title.trimmed().isEmpty() || xml.isEmpty())
        throw std::invalid_argument("a style needs an id, a title and its XML");
    auto insert = m_db.prepare("INSERT INTO csl_style (id, title, xml, added_at) VALUES (?, ?, ?, ?) "
                               "ON CONFLICT (id) DO UPDATE SET title = excluded.title, xml = excluded.xml, added_at = excluded.added_at");
    insert->bind(1, id);
    insert->bind(2, title.simplified());
    insert->bind(3, QString::fromUtf8(xml));
    insert->bind(4, addedAt > 0 ? addedAt : QDateTime::currentSecsSinceEpoch());
    insert->step();
}

void CslStyleRepository::remove(const QString &id)
{
    auto remove = m_db.prepare("DELETE FROM csl_style WHERE id = ?");
    remove->bind(1, id);
    remove->step();
}

} // namespace papertrail::core
