#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

namespace papertrail::core {

class Database;

// A notice published about a work after it came out, such as a retraction, a correction, or an expression of
// concern, as Crossref lists them.
struct Notice {
    QString kind;      // As Crossref names it, such as "retraction", "correction" or "expression_of_concern".
    QString label;     // How Crossref words it, such as "Retraction".
    QString noticeDoi; // The notice's own DOI.
    QString date;      // As yyyy, yyyy-mm or yyyy-mm-dd, or empty if not known.
    QString source;    // Where Crossref has it from, such as "publisher" or "retraction-watch".

    // Whether it's serious enough to warn about wherever the publication's shown.
    bool isWarning() const;
};

// A publication with a notice worth warning about that was found after some time.
struct NewWarning {
    qint64 publicationId = 0;
    QString title;
    Notice notice; // The newest such notice, with its kind, label and date.
    qint64 foundAt = 0;
};

// Notices on publications in the library, from checking their DOIs now and then.
class NoticeRepository {
public:
    explicit NoticeRepository(Database &db) : m_db(db) {}

    QList<Notice> forPublication(qint64 publicationId);

    // Publications with a DOI, in the library, not checked since `checkedBefore`, those never checked first.
    QList<QPair<qint64, QString>> dueForCheck(int limit, qint64 checkedBefore);

    // What a check found: for each publication checked, its notices now, replacing any found before. A notice that
    // was already known keeps the time it was first found.
    void record(const QList<qint64> &checked, const QHash<qint64, QList<Notice>> &found, qint64 when);

    // Publications in the library with a notice worth warning about first found after `since`, newest first, each
    // listed once.
    QList<NewWarning> unseenWarnings(qint64 since);

    // When the newest notice was first found, or 0 if there are none.
    qint64 newestFoundAt();

    // How many publications in the library have a notice worth warning about.
    qint64 warningCount();

private:
    Database &m_db;
};

} // namespace papertrail::core
