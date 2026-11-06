#include "core/repo/PublicationRepository.h"

#include "core/db/Database.h"
#include "core/model/Identifiers.h"

#include <QDateTime>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace papertrail::core {

namespace {

struct TextColumn {
    const char *name;
    QString Publication::*member;
};

struct IntColumn {
    const char *name;
    std::optional<int> Publication::*member;
};

// Every nullable text and number column, so that insert, update and read can't drift apart.
const std::array kTextColumns = {
    TextColumn{"subtitle", &Publication::subtitle},
    TextColumn{"abstract", &Publication::abstract},
    TextColumn{"language", &Publication::language},
    TextColumn{"url", &Publication::url},
    TextColumn{"edition", &Publication::edition},
    TextColumn{"series", &Publication::series},
    TextColumn{"doi", &Publication::doi},
    TextColumn{"isbn10", &Publication::isbn10},
    TextColumn{"isbn13", &Publication::isbn13},
    TextColumn{"issn", &Publication::issn},
    TextColumn{"eissn", &Publication::eissn},
    TextColumn{"arxiv_id", &Publication::arxivId},
    TextColumn{"pmid", &Publication::pmid},
    TextColumn{"container_title", &Publication::containerTitle},
    TextColumn{"volume", &Publication::volume},
    TextColumn{"issue", &Publication::issue},
    TextColumn{"pages", &Publication::pages},
    TextColumn{"publisher", &Publication::publisher},
    TextColumn{"place", &Publication::place},
    TextColumn{"conference_name", &Publication::conferenceName},
    TextColumn{"conference_location", &Publication::conferenceLocation},
    TextColumn{"institution", &Publication::institution},
    TextColumn{"department", &Publication::department},
    TextColumn{"degree", &Publication::degree},
    TextColumn{"report_number", &Publication::reportNumber},
    TextColumn{"standard_number", &Publication::standardNumber},
    TextColumn{"standard_body", &Publication::standardBody},
    TextColumn{"standard_revision", &Publication::standardRevision},
    TextColumn{"standard_status", &Publication::standardStatus},
    TextColumn{"notes", &Publication::notes},
    TextColumn{"extra_json", &Publication::extraJson},
};

const std::array kIntColumns = {
    IntColumn{"page_start", &Publication::pageStart},
    IntColumn{"page_end", &Publication::pageEnd},
    IntColumn{"year", &Publication::year},
    IntColumn{"month", &Publication::month},
    IntColumn{"day", &Publication::day},
    IntColumn{"rating", &Publication::rating},
};

// Sorts after any real text, so records with nothing to sort on go to the end of a list.
const QString &emptySortKey()
{
    static const QString key = QString::fromUcs4(U"\U0010FFFF");
    return key;
}

QString nullIfEmpty(const QString &value)
{
    return value.isEmpty() ? QString() : value;
}

// Puts a record's values into the form they're stored in.
Publication normalised(const Publication &input)
{
    Publication p = input;
    for (const TextColumn &column : kTextColumns)
        p.*column.member = nullIfEmpty(p.*column.member);
    p.doi = normaliseDoi(p.doi);
    p.isbn10 = normaliseIsbn(p.isbn10);
    p.isbn13 = normaliseIsbn(p.isbn13);
    p.issn = normaliseIssn(p.issn);
    p.eissn = normaliseIssn(p.eissn);
    p.arxivId = normaliseArxivId(p.arxivId);
    return p;
}

QString authorsDisplay(const QList<PersonName> &people)
{
    constexpr int shown = 3;
    QStringList names;
    for (int i = 0; i < people.size() && i < shown; ++i)
        names << people.at(i).display();
    QString text = names.join(QStringLiteral("; "));
    if (people.size() > shown)
        text += QStringLiteral(" et al.");
    return text;
}

const QList<PersonName> &listedPeople(const Publication &p)
{
    return p.authors.isEmpty() ? p.editors : p.authors;
}

QString firstAuthorSortKey(const Publication &p)
{
    const QList<PersonName> &people = listedPeople(p);
    if (people.isEmpty())
        return emptySortKey();
    const QString key = nameSortKey(people.first().family + u' ' + people.first().given);
    return key.isEmpty() ? emptySortKey() : key;
}

QString titleKey(const Publication &p)
{
    const QString key = titleSortKey(p.title);
    return key.isEmpty() ? emptySortKey() : key;
}

// The value columns shared by insert and update, in a fixed order.
std::string valueColumnList()
{
    std::string columns = "type, parent_id, title, read_status, needs_review, title_sort, "
                          "first_author_sort, authors_display, updated_at";
    for (const TextColumn &column : kTextColumns)
        columns += std::string(", ") + column.name;
    for (const IntColumn &column : kIntColumns)
        columns += std::string(", ") + column.name;
    return columns;
}

int bindValues(Statement &statement, const Publication &p, qint64 now)
{
    int index = 1;
    statement.bind(index++, toDbString(p.type));
    statement.bind(index++, p.parentId);
    statement.bind(index++, p.title.isNull() ? QStringLiteral("") : p.title);
    statement.bind(index++, toDbString(p.readStatus));
    statement.bind(index++, p.needsReview ? 1 : 0);
    statement.bind(index++, titleKey(p));
    statement.bind(index++, firstAuthorSortKey(p));
    const QString display = authorsDisplay(listedPeople(p));
    statement.bind(index++, display.isNull() ? QStringLiteral("") : display);
    statement.bind(index++, now);
    for (const TextColumn &column : kTextColumns)
        statement.bind(index++, p.*column.member);
    for (const IntColumn &column : kIntColumns)
        statement.bind(index++, p.*column.member);
    return index;
}

const std::string &insertSql()
{
    static const std::string sql = [] {
        const std::string columns = valueColumnList();
        const auto values = std::count(columns.begin(), columns.end(), ',') + 1;
        std::string text = "INSERT INTO publication (" + columns + ", uuid, created_at) VALUES (";
        for (qsizetype i = 0; i < values + 2; ++i)
            text += i == 0 ? "?" : ", ?";
        return text + ")";
    }();
    return sql;
}

const std::string &updateSql()
{
    static const std::string sql = [] {
        std::string text = "UPDATE publication SET ";
        const std::string columns = valueColumnList();
        std::size_t start = 0;
        while (start < columns.size()) {
            const std::size_t comma = columns.find(", ", start);
            const std::size_t end = comma == std::string::npos ? columns.size() : comma;
            text += columns.substr(start, end - start) + " = ?";
            if (comma == std::string::npos)
                break;
            text += ", ";
            start = comma + 2;
        }
        return text + " WHERE id = ?";
    }();
    return sql;
}

const std::string &selectSql()
{
    static const std::string sql = "SELECT id, uuid, created_at, " + valueColumnList()
        + " FROM publication WHERE id = ?";
    return sql;
}

} // namespace

void PublicationRepository::validate(const Publication &p)
{
    if (p.pageStart && p.pageEnd && *p.pageEnd < *p.pageStart)
        throw std::invalid_argument("page range ends before it starts");

    if (!p.parentId)
        return;
    if (*p.parentId == p.id)
        throw std::invalid_argument("a publication can't be its own parent");

    // One level only: a volume holds articles, and an article doesn't hold anything.
    Statement &parent = m_db.cached("SELECT parent_id FROM publication WHERE id = ?");
    parent.bind(1, *p.parentId);
    if (!parent.step())
        throw std::invalid_argument("parent publication doesn't exist");
    const bool parentIsChild = !parent.isNull(0);
    parent.reset();
    if (parentIsChild)
        throw std::invalid_argument("a child publication can't have children");

    if (p.id != 0) {
        Statement &children = m_db.cached("SELECT 1 FROM publication WHERE parent_id = ? LIMIT 1");
        children.bind(1, p.id);
        const bool hasChildren = children.step();
        children.reset();
        if (hasChildren)
            throw std::invalid_argument("a publication with children can't become a child");
    }
}

qint64 PublicationRepository::create(Publication &publication)
{
    Publication p = normalised(publication);
    p.id = 0;
    validate(p);

    if (p.uuid.isEmpty())
        p.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 createdAt = publication.createdAt != 0 ? publication.createdAt : now;
    const qint64 updatedAt = publication.updatedAt != 0 ? publication.updatedAt : now;

    Transaction transaction(m_db);
    Statement &insert = m_db.cached(insertSql().c_str());
    int index = bindValues(insert, p, updatedAt);
    insert.bind(index++, p.uuid);
    insert.bind(index++, createdAt);
    insert.step();
    insert.reset();
    const qint64 id = m_db.lastInsertId();
    writePeople(id, p);
    transaction.commit();

    publication = p;
    publication.id = id;
    publication.createdAt = createdAt;
    publication.updatedAt = updatedAt;
    return id;
}

void PublicationRepository::update(Publication &publication)
{
    Publication p = normalised(publication);
    validate(p);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction transaction(m_db);
    Statement &update = m_db.cached(updateSql().c_str());
    const int index = bindValues(update, p, now);
    update.bind(index, p.id);
    update.step();
    const int changed = m_db.changes();
    update.reset();
    if (changed == 0)
        throw std::invalid_argument("publication doesn't exist");
    writePeople(p.id, p);
    transaction.commit();

    publication = p;
    publication.updatedAt = now;
}

namespace {

QString peopleText(const QList<PersonName> &people)
{
    QStringList names;
    for (const PersonName &person : people)
        names << person.display();
    return names.join(QStringLiteral("; "));
}

QString numberText(const std::optional<int> &value)
{
    return value ? QString::number(*value) : QString();
}

} // namespace

void PublicationRepository::fillGaps(Publication &into, const Publication &from)
{
    for (const TextColumn &column : kTextColumns) {
        // Notes are the reader's own, and merging decides what to do with them.
        if (qstrcmp(column.name, "notes") != 0 && qstrcmp(column.name, "extra_json") != 0
            && (into.*column.member).isEmpty()) {
            into.*column.member = from.*column.member;
        }
    }
    for (const IntColumn &column : kIntColumns) {
        // A page range belongs to one particular file's place in a volume.
        if (qstrcmp(column.name, "page_start") != 0 && qstrcmp(column.name, "page_end") != 0
            && !(into.*column.member)) {
            into.*column.member = from.*column.member;
        }
    }
    if (into.authors.isEmpty())
        into.authors = from.authors;
    if (into.editors.isEmpty())
        into.editors = from.editors;
    if (into.translators.isEmpty())
        into.translators = from.translators;
    if (into.advisors.isEmpty())
        into.advisors = from.advisors;
}

void PublicationRepository::updateWithProvenance(Publication &publication, const QString &source)
{
    Transaction transaction(m_db);
    const std::optional<Publication> before = get(publication.id);
    if (!before)
        throw std::invalid_argument("publication doesn't exist");
    update(publication);

    const auto record = [&](const char *field, const QString &oldValue, const QString &newValue) {
        if (oldValue != newValue && !(oldValue.isEmpty() && newValue.isEmpty())) {
            recordProvenance(publication.id, {QString::fromLatin1(field), source, std::nullopt, oldValue, newValue, 0});
        }
    };
    record("type", toDbString(before->type), toDbString(publication.type));
    record("title", before->title, publication.title);
    for (const TextColumn &column : kTextColumns) {
        // Personal notes aren't metadata, and have no earlier values to go back to.
        if (qstrcmp(column.name, "notes") != 0)
            record(column.name, (*before).*column.member, publication.*column.member);
    }
    for (const IntColumn &column : kIntColumns) {
        if (qstrcmp(column.name, "rating") != 0)
            record(column.name, numberText((*before).*column.member), numberText(publication.*column.member));
    }
    record("authors", peopleText(before->authors), peopleText(publication.authors));
    record("editors", peopleText(before->editors), peopleText(publication.editors));
    record("translators", peopleText(before->translators), peopleText(publication.translators));
    record("advisors", peopleText(before->advisors), peopleText(publication.advisors));
    transaction.commit();
}

void PublicationRepository::setReadStatus(const QList<qint64> &ids, ReadStatus status)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction transaction(m_db);
    Statement &update = m_db.cached("UPDATE publication SET read_status = ?, updated_at = ? WHERE id = ?");
    for (const qint64 id : ids) {
        update.bind(1, toDbString(status));
        update.bind(2, now);
        update.bind(3, id);
        update.step();
        update.reset();
    }
    transaction.commit();
}

void PublicationRepository::setNeedsReview(const QList<qint64> &ids, bool needsReview)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Transaction transaction(m_db);
    Statement &update = m_db.cached("UPDATE publication SET needs_review = ?, updated_at = ? WHERE id = ?");
    for (const qint64 id : ids) {
        update.bind(1, needsReview ? 1 : 0);
        update.bind(2, now);
        update.bind(3, id);
        update.step();
        update.reset();
    }
    transaction.commit();
}

void PublicationRepository::remove(qint64 id)
{
    Transaction transaction(m_db);
    QList<qint64> people;
    {
        auto select = m_db.prepare(
            "SELECT DISTINCT person_id FROM publication_person pp JOIN publication p "
            "ON p.id = pp.publication_id WHERE p.id = ?1 OR p.parent_id = ?1");
        select->bind(1, id);
        while (select->step())
            people << select->int64At(0);
    }

    auto remove = m_db.prepare("DELETE FROM publication WHERE id = ?");
    remove->bind(1, id);
    remove->step();

    auto orphan = m_db.prepare(
        "DELETE FROM person WHERE id = ?1 "
        "AND NOT EXISTS (SELECT 1 FROM publication_person WHERE person_id = ?1)");
    for (const qint64 person : people) {
        orphan->reset();
        orphan->bind(1, person);
        orphan->step();
    }
    transaction.commit();
}

std::optional<Publication> PublicationRepository::get(qint64 id)
{
    Publication p;
    {
        auto select = m_db.prepare(selectSql().c_str());
        select->bind(1, id);
        if (!select->step())
            return std::nullopt;

        int column = 0;
        p.id = select->int64At(column++);
        p.uuid = select->textAt(column++);
        p.createdAt = select->int64At(column++);
        p.type = publicationTypeFromDb(select->textAt(column++)).value_or(PublicationType::Other);
        p.parentId = select->optInt64At(column++);
        p.title = select->textAt(column++);
        p.readStatus = readStatusFromDb(select->textAt(column++)).value_or(ReadStatus::Unread);
        p.needsReview = select->intAt(column++) != 0;
        column += 3; // title_sort, first_author_sort and authors_display are derived.
        p.updatedAt = select->int64At(column++);
        for (const TextColumn &text : kTextColumns)
            p.*text.member = select->textAt(column++);
        for (const IntColumn &number : kIntColumns)
            p.*number.member = select->optIntAt(column++);
    }

    p.authors = readPeople(id, PersonRole::Author);
    p.editors = readPeople(id, PersonRole::Editor);
    p.translators = readPeople(id, PersonRole::Translator);
    p.advisors = readPeople(id, PersonRole::Advisor);
    return p;
}

std::optional<qint64> PublicationRepository::idForUuid(const QString &uuid)
{
    auto select = m_db.prepare("SELECT id FROM publication WHERE uuid = ?");
    select->bind(1, uuid);
    if (!select->step())
        return std::nullopt;
    return select->int64At(0);
}

QList<qint64> PublicationRepository::childIds(qint64 parentId)
{
    QList<qint64> ids;
    auto select = m_db.prepare(
        "SELECT id FROM publication WHERE parent_id = ? ORDER BY page_start, title_sort");
    select->bind(1, parentId);
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

qint64 PublicationRepository::count()
{
    auto select = m_db.prepare("SELECT count(*) FROM publication");
    select->step();
    return select->int64At(0);
}

void PublicationRepository::recordProvenance(qint64 publicationId, const ProvenanceEntry &entry)
{
    auto insert = m_db.prepare(
        "INSERT INTO metadata_provenance "
        "(publication_id, field, source, confidence, previous_value, new_value, recorded_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");
    insert->bind(1, publicationId);
    insert->bind(2, entry.field);
    insert->bind(3, entry.source);
    insert->bind(4, entry.confidence);
    insert->bind(5, entry.previousValue);
    insert->bind(6, entry.newValue);
    insert->bind(7, entry.recordedAt != 0 ? entry.recordedAt : QDateTime::currentMSecsSinceEpoch());
    insert->step();
}

QList<ProvenanceEntry> PublicationRepository::provenance(qint64 publicationId, const QString &field)
{
    auto select = m_db.prepare(
        "SELECT field, source, confidence, previous_value, new_value, recorded_at "
        "FROM metadata_provenance WHERE publication_id = ?1 AND (?2 IS NULL OR field = ?2) "
        "ORDER BY recorded_at DESC, id DESC");
    select->bind(1, publicationId);
    select->bind(2, field);

    QList<ProvenanceEntry> entries;
    while (select->step()) {
        ProvenanceEntry entry;
        entry.field = select->textAt(0);
        entry.source = select->textAt(1);
        if (!select->isNull(2))
            entry.confidence = select->doubleAt(2);
        entry.previousValue = select->textAt(3);
        entry.newValue = select->textAt(4);
        entry.recordedAt = select->int64At(5);
        entries << entry;
    }
    return entries;
}

qint64 PublicationRepository::personId(const PersonName &name)
{
    const QString key = nameSortKey(name.family + QChar(0x1F) + name.given);

    Statement &select = m_db.cached("SELECT id FROM person WHERE name_key = ?");
    select.bind(1, key);
    if (select.step()) {
        const qint64 id = select.int64At(0);
        select.reset();
        return id;
    }
    select.reset();

    Statement &insert = m_db.cached(
        "INSERT INTO person (name_key, family_name, given_name) VALUES (?, ?, ?)");
    insert.bind(1, key);
    insert.bind(2, name.family);
    insert.bind(3, name.given.isNull() ? QStringLiteral("") : name.given);
    insert.step();
    insert.reset();
    return m_db.lastInsertId();
}

void PublicationRepository::writePeople(qint64 id, const Publication &p)
{
    QSet<qint64> previous;
    {
        Statement &select = m_db.cached(
            "SELECT person_id FROM publication_person WHERE publication_id = ?");
        select.bind(1, id);
        while (select.step())
            previous.insert(select.int64At(0));
        select.reset();
    }

    if (!previous.isEmpty()) {
        Statement &clear = m_db.cached("DELETE FROM publication_person WHERE publication_id = ?");
        clear.bind(1, id);
        clear.step();
        clear.reset();
    }

    const std::array<std::pair<PersonRole, const QList<PersonName> *>, 4> groups = {{
        {PersonRole::Author, &p.authors},
        {PersonRole::Editor, &p.editors},
        {PersonRole::Translator, &p.translators},
        {PersonRole::Advisor, &p.advisors},
    }};

    for (const auto &[role, people] : groups) {
        int position = 0;
        for (const PersonName &person : *people) {
            if (person.family.isEmpty() && person.given.isEmpty())
                continue;
            const qint64 personKey = personId(person);
            previous.remove(personKey);
            Statement &insert = m_db.cached(
                "INSERT INTO publication_person (publication_id, person_id, role, position) "
                "VALUES (?, ?, ?, ?)");
            insert.bind(1, id);
            insert.bind(2, personKey);
            insert.bind(3, toDbString(role));
            insert.bind(4, position++);
            insert.step();
            insert.reset();
        }
    }

    // Anyone no longer on this publication may now be on none at all.
    for (const qint64 person : previous) {
        Statement &orphan = m_db.cached(
            "DELETE FROM person WHERE id = ?1 "
            "AND NOT EXISTS (SELECT 1 FROM publication_person WHERE person_id = ?1)");
        orphan.bind(1, person);
        orphan.step();
        orphan.reset();
    }
}

QList<PersonName> PublicationRepository::readPeople(qint64 id, PersonRole role)
{
    QList<PersonName> people;
    auto select = m_db.prepare(
        "SELECT pr.family_name, pr.given_name FROM publication_person pp "
        "JOIN person pr ON pr.id = pp.person_id "
        "WHERE pp.publication_id = ? AND pp.role = ? ORDER BY pp.position");
    select->bind(1, id);
    select->bind(2, toDbString(role));
    while (select->step())
        people << PersonName{select->textAt(0), select->textAt(1)};
    return people;
}

} // namespace papertrail::core
