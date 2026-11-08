#include "metadata/NoticeChecker.h"

#include "core/db/Database.h"
#include "core/repo/NoticeRepository.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>

namespace papertrail::metadata {

NoticeChecker::NoticeChecker(core::Database &db, Transport &transport, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_transport(transport)
{
}

void NoticeChecker::start()
{
    if (m_running)
        return;
    m_running = true;
    m_stopping = false;
    m_checked = 0;
    m_newWarnings = 0;
    m_startedAt = QDateTime::currentMSecsSinceEpoch();
    // Never finishing inside this call, so whoever started it can always wait to hear.
    QTimer::singleShot(0, this, &NoticeChecker::next);
}

void NoticeChecker::stop()
{
    if (m_running)
        m_stopping = true;
}

void NoticeChecker::finish()
{
    m_running = false;
    m_stopping = false;
    emit finished(m_checked, m_newWarnings);
}

void NoticeChecker::next()
{
    if (m_stopping) {
        finish();
        return;
    }
    QList<QPair<qint64, QString>> due;
    try {
        // Never later than this run began, so that each publication is asked about once a run at most.
        const qint64 before = qMin(m_startedAt, QDateTime::currentMSecsSinceEpoch() - qint64(m_recheckDays) * 24 * 3600 * 1000);
        due = core::NoticeRepository(m_db).dueForCheck(m_batchSize, before);
    } catch (const std::exception &error) {
        emit problem(QString::fromUtf8(error.what()));
        finish();
        return;
    }
    if (due.isEmpty()) {
        finish();
        return;
    }

    // A comma would split the request's filter, so a DOI with one is counted as checked without asking.
    QStringList dois;
    QHash<QString, QList<qint64>> byDoi;
    QList<qint64> checked;
    for (const auto &[id, doi] : due) {
        checked << id;
        const QString key = doi.trimmed().toLower();
        if (key.contains(u',') || key.isEmpty())
            continue;
        if (!byDoi.contains(key))
            dois << key;
        byDoi[key] << id;
    }

    const auto record = [this, checked, byDoi](const QHash<QString, QList<core::Notice>> &found) {
        QHash<qint64, QList<core::Notice>> byPublication;
        for (auto it = byDoi.cbegin(); it != byDoi.cend(); ++it) {
            for (const qint64 id : it.value())
                byPublication.insert(id, found.value(it.key()));
        }
        core::NoticeRepository notices(m_db);
        QList<qint64> changed;
        for (const qint64 id : checked) {
            const QList<core::Notice> before = notices.forPublication(id);
            const QList<core::Notice> after = byPublication.value(id);
            QSet<QString> was;
            bool warnedBefore = false;
            for (const core::Notice &notice : before) {
                was.insert(notice.noticeDoi + u'|' + notice.kind);
                warnedBefore = warnedBefore || notice.isWarning();
            }
            QSet<QString> now;
            bool warnedNow = false;
            for (const core::Notice &notice : after) {
                now.insert(notice.noticeDoi + u'|' + notice.kind);
                warnedNow = warnedNow || notice.isWarning();
            }
            if (was != now)
                changed << id;
            if (warnedNow && !warnedBefore)
                ++m_newWarnings;
        }
        notices.record(checked, byPublication, QDateTime::currentMSecsSinceEpoch());
        m_checked += int(checked.size());
        if (!changed.isEmpty())
            emit noticesChanged(changed);
    };

    if (dois.isEmpty()) {
        try {
            record({});
        } catch (const std::exception &error) {
            emit problem(QString::fromUtf8(error.what()));
            finish();
            return;
        }
        QTimer::singleShot(0, this, &NoticeChecker::next);
        return;
    }

    QUrl url = crossrefNoticesUrl(m_urls, dois);
    if (!m_email.isEmpty()) {
        // Only if the user has chosen to give one, as for lookups.
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("mailto"), QString::fromLatin1(QUrl::toPercentEncoding(m_email)));
        url.setQuery(query);
    }
    QPointer<NoticeChecker> self(this);
    m_transport.get(url, [self, record](const TransportReply &reply) {
        if (!self)
            return;
        bool readable = false;
        const QHash<QString, QList<core::Notice>> found = reply.status == 200 ? parseCrossrefNotices(reply.body, &readable)
                                                                             : QHash<QString, QList<core::Notice>>();
        if (!readable) {
            // Not checked, so these are asked about again next time.
            emit self->problem(reply.error.isEmpty() ? QStringLiteral("Crossref couldn't be asked about retractions (%1).").arg(reply.status)
                                                     : reply.error);
            self->finish();
            return;
        }
        try {
            record(found);
        } catch (const std::exception &error) {
            emit self->problem(QString::fromUtf8(error.what()));
            self->finish();
            return;
        }
        QTimer::singleShot(self->m_intervalMs, self.data(), &NoticeChecker::next);
    });
}

} // namespace papertrail::metadata
