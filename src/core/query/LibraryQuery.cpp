#include "core/query/LibraryQuery.h"

#include "core/db/Database.h"
#include "core/query/SearchParser.h"

#include <QHash>
#include <QSet>
#include <QStringList>

#include <memory>
#include <variant>

namespace papertrail::core {

namespace {

using Binding = std::variant<qint64, QString>;

// A SQL condition built up alongside its parameters. All input goes through bindings: nothing
// the user types ever becomes part of the SQL text.
struct Conditions {
    QByteArrayList clauses;
    QList<Binding> bindings;

    void add(const QByteArray &clause) { clauses << clause; }

    QByteArray whereClause() const
    {
        return clauses.isEmpty() ? QByteArray() : " WHERE " + clauses.join(" AND ");
    }

    QByteArray placeholders(qsizetype count) const
    {
        QByteArray text;
        for (qsizetype i = 0; i < count; ++i)
            text += i == 0 ? "?" : ", ?";
        return text;
    }

    void addIn(const QByteArray &column, const QStringList &values)
    {
        if (values.isEmpty())
            return;
        add(column + " IN (" + placeholders(values.size()) + ")");
        for (const QString &value : values)
            bindings << value;
    }

    void addInIds(const QByteArray &columnPrefix, const QByteArray &suffix, const QList<qint64> &values)
    {
        add(columnPrefix + " (" + placeholders(values.size()) + ")" + suffix);
        for (const qint64 value : values)
            bindings << value;
    }
};

void bindAll(Statement &statement, const QList<Binding> &bindings, int start = 1)
{
    int index = start;
    for (const Binding &binding : bindings) {
        if (const auto *number = std::get_if<qint64>(&binding))
            statement.bind(index++, *number);
        else
            statement.bind(index++, std::get<QString>(binding));
    }
}

QStringList typeStrings(const QList<PublicationType> &types)
{
    QStringList strings;
    for (const PublicationType type : types)
        strings << toDbString(type);
    return strings;
}

// A child article has no file link of its own: it uses its parent volume's file.
const QByteArray kOwnerOfFile = "coalesce(p.parent_id, p.id)";

QString likePrefix(const QString &text)
{
    QString escaped = text;
    escaped.replace(u'\\', QStringLiteral("\\\\"));
    escaped.replace(u'%', QStringLiteral("\\%"));
    escaped.replace(u'_', QStringLiteral("\\_"));
    return escaped + u'%';
}

// Prefix match on a column, written as a range so that it can use the column's index.
void addPrefix(Conditions &conditions, const QByteArray &column, const QString &prefix)
{
    conditions.add(column + " >= ? AND " + column + " < ?");
    conditions.bindings << prefix << prefix + QString::fromUcs4(U"\U0010FFFF");
}

// `ftsClause` is empty when there's no text to search for, or when the caller joins the FTS
// table itself to rank by relevance.
Conditions buildConditions(const PublicationFilter &filter, const QByteArray &ftsClause)
{
    Conditions c;
    const ParsedSearch search = parseSearch(filter.searchText);

    if (!ftsClause.isEmpty())
        c.add(ftsClause);

    c.addIn("p.type", typeStrings(filter.tabTypes));
    c.addIn("p.type", typeStrings(filter.types));
    c.addIn("p.type", typeStrings(search.types));

    if (!filter.tagIds.isEmpty()) {
        c.addInIds(
            "p.id IN (SELECT publication_id FROM publication_tag WHERE tag_id IN ("
            "WITH RECURSIVE down(id) AS (SELECT id FROM tag WHERE id IN",
            " UNION SELECT t.id FROM tag t JOIN down ON t.parent_id = down.id) SELECT id FROM down))",
            filter.tagIds);
    }
    for (const QString &prefix : search.tagPrefixes) {
        c.add("p.id IN (SELECT publication_id FROM publication_tag WHERE tag_id IN ("
              "WITH RECURSIVE down(id) AS (SELECT id FROM tag WHERE name LIKE ? ESCAPE '\\' "
              "UNION SELECT t.id FROM tag t JOIN down ON t.parent_id = down.id) SELECT id FROM down))");
        c.bindings << likePrefix(prefix);
    }

    if (!filter.authorIds.isEmpty()) {
        c.addInIds("p.id IN (SELECT publication_id FROM publication_person "
                   "WHERE role = 'author' AND person_id IN",
                   ")", filter.authorIds);
    }

    c.addIn("p.container_title", filter.journals);
    c.addIn("p.publisher", filter.publishers);
    c.addIn("p.conference_name", filter.conferences);
    c.addIn("p.institution", filter.institutions);
    c.addIn("p.language", filter.languages);

    if (!filter.formats.isEmpty()) {
        c.add(kOwnerOfFile + " IN (SELECT pf.publication_id FROM publication_file pf "
              "JOIN file f ON f.id = pf.file_id WHERE f.format IN ("
              + c.placeholders(filter.formats.size()) + "))");
        for (const QString &format : filter.formats)
            c.bindings << format;
    }

    if (!filter.readStatuses.isEmpty()) {
        QStringList statuses;
        for (const ReadStatus status : filter.readStatuses)
            statuses << toDbString(status);
        c.addIn("p.read_status", statuses);
    }

    if (filter.yearMin) {
        c.add("p.year >= ?");
        c.bindings << qint64(*filter.yearMin);
    }
    if (filter.yearMax) {
        c.add("p.year <= ?");
        c.bindings << qint64(*filter.yearMax);
    }
    if (search.yearMin) {
        c.add("p.year >= ?");
        c.bindings << qint64(*search.yearMin);
    }
    if (search.yearMax) {
        c.add("p.year <= ?");
        c.bindings << qint64(*search.yearMax);
    }

    for (const QString &prefix : search.doiPrefixes)
        addPrefix(c, "p.doi", prefix);
    for (const QString &prefix : search.arxivPrefixes)
        addPrefix(c, "p.arxiv_id", prefix);
    for (const QString &isbn : search.isbns) {
        c.add("(p.isbn13 = ? OR p.isbn10 = ?)");
        c.bindings << isbn << isbn;
    }
    for (const QString &issn : search.issns) {
        c.add("(p.issn = ? OR p.eissn = ?)");
        c.bindings << issn << issn;
    }

    if (filter.needsReview)
        c.add("p.needs_review = 1");
    if (filter.noText) {
        c.add(kOwnerOfFile + " IN (SELECT pf.publication_id FROM publication_file pf "
              "JOIN file f ON f.id = pf.file_id WHERE f.has_text_layer = 0)");
    }
    if (filter.noFile)
        c.add(kOwnerOfFile + " NOT IN (SELECT publication_id FROM publication_file)");
    if (filter.missingOrOffline) {
        c.add(kOwnerOfFile + " IN (SELECT pf.publication_id FROM publication_file pf "
              "JOIN file f ON f.id = pf.file_id WHERE f.status IN ('missing', 'offline'))");
    }

    return c;
}

// Ties break on id, so a page never repeats or skips a row between fetches.
QByteArray orderBy(const PublicationSort &sort)
{
    const QByteArray direction = sort.ascending ? " ASC" : " DESC";
    switch (sort.field) {
    case SortField::Relevance:
    case SortField::Title:
        return " ORDER BY p.title_sort" + direction + ", p.id" + direction;
    case SortField::FirstAuthor:
        return " ORDER BY p.first_author_sort" + direction + ", p.id" + direction;
    case SortField::Year:
        // These expressions have to match the year indexes in the schema.
        return sort.ascending ? " ORDER BY coalesce(p.year, 99999) ASC, p.title_sort ASC, p.id ASC"
                              : " ORDER BY coalesce(p.year, -1) DESC, p.title_sort ASC, p.id ASC";
    case SortField::Added:
        return " ORDER BY p.created_at" + direction + ", p.id" + direction;
    }
    return " ORDER BY p.title_sort ASC, p.id ASC";
}

// The FROM clause, conditions and ordering shared by page() and ids().
struct ListQuery {
    QByteArray sql;
    QList<Binding> bindings;
};

ListQuery buildListQuery(const PublicationFilter &filter, const PublicationSort &sort, const QByteArray &ftsClause)
{
    const QString ftsMatch = parseSearch(filter.searchText).ftsMatch;
    ListQuery query;
    if (sort.field == SortField::Relevance && !ftsMatch.isEmpty()) {
        Conditions c = buildConditions(filter, QByteArray());
        query.sql = " FROM publication_fts JOIN publication p ON p.id = publication_fts.rowid "
                    "WHERE publication_fts MATCH ?";
        query.bindings << ftsMatch;
        for (const QByteArray &clause : c.clauses)
            query.sql += " AND " + clause;
        query.bindings += c.bindings;
        // Column weights: title, abstract, authors, tags, container, publisher, identifiers.
        query.sql += " ORDER BY bm25(publication_fts, 10.0, 1.0, 5.0, 3.0, 2.0, 1.0, 8.0), p.id";
    } else {
        Conditions c = buildConditions(filter, ftsClause);
        query.sql = " FROM publication p" + c.whereClause() + orderBy(sort);
        query.bindings = c.bindings;
    }
    return query;
}

// Clears the facet's own selection, so its counts show what choosing another value would give.
PublicationFilter withoutOwnSelection(PublicationFilter filter, Facet facet)
{
    switch (facet) {
    case Facet::Type: filter.types.clear(); break;
    case Facet::Author: filter.authorIds.clear(); break;
    case Facet::Journal: filter.journals.clear(); break;
    case Facet::Publisher: filter.publishers.clear(); break;
    case Facet::Conference: filter.conferences.clear(); break;
    case Facet::Institution: filter.institutions.clear(); break;
    case Facet::Language: filter.languages.clear(); break;
    case Facet::Format: filter.formats.clear(); break;
    case Facet::ReadStatus: filter.readStatuses.clear(); break;
    }
    return filter;
}

QByteArray columnFacetSql(const QByteArray &column, const Conditions &c)
{
    QByteArray where = c.whereClause();
    where += where.isEmpty() ? " WHERE " : " AND ";
    return "SELECT " + column + ", " + column + ", count(*) FROM publication p" + where + column
        + " IS NOT NULL GROUP BY " + column + " ORDER BY count(*) DESC, " + column + " LIMIT ?";
}

} // namespace

QByteArray LibraryQuery::searchClause(const PublicationFilter &filter)
{
    // Above this many matches, it's quicker to walk the list in sort order and check each row
    // against the matches than to sort all the matches.
    constexpr qint64 kWalkInOrderAbove = 2000;

    const QString match = parseSearch(filter.searchText).ftsMatch;
    if (match.isEmpty())
        return {};

    if (match != m_searchText || m_db.changeStamp() != m_searchStamp) {
        m_db.exec("CREATE TEMP TABLE IF NOT EXISTS search_match (id INTEGER PRIMARY KEY)");
        m_db.exec("DELETE FROM temp.search_match");
        Statement &fill = m_db.cached(
            "INSERT INTO temp.search_match SELECT rowid FROM publication_fts WHERE publication_fts MATCH ?");
        fill.bind(1, match);
        fill.step();
        fill.reset();

        Statement &size = m_db.cached("SELECT count(*) FROM temp.search_match");
        size.step();
        m_searchCount = size.int64At(0);
        size.reset();

        m_searchText = match;
        m_searchStamp = m_db.changeStamp(); // Taken last, since filling the table changes it.
    }

    // A leading + stops SQLite using this condition to choose its starting point.
    return m_searchCount > kWalkInOrderAbove ? "+p.id IN temp.search_match" : "p.id IN temp.search_match";
}

qint64 LibraryQuery::count(const PublicationFilter &filter)
{
    const QByteArray fts = searchClause(filter);
    const Conditions c = buildConditions(filter, fts);
    if (!fts.isEmpty() && c.clauses.size() == 1)
        return m_searchCount;

    auto select = m_db.prepare("SELECT count(*) FROM publication p" + c.whereClause());
    bindAll(*select, c.bindings);
    select->step();
    return select->int64At(0);
}

QList<PublicationSummary> LibraryQuery::page(const PublicationFilter &filter, const PublicationSort &sort,
                                             qint64 offset, int limit)
{
    const ListQuery query = buildListQuery(filter, sort, searchClause(filter));
    auto select = m_db.prepare(
        "SELECT p.id, p.type, p.title, p.authors_display, p.year, p.container_title, p.read_status"
        + query.sql + " LIMIT ? OFFSET ?");
    bindAll(*select, query.bindings);
    select->bind(int(query.bindings.size()) + 1, limit);
    select->bind(int(query.bindings.size()) + 2, offset);

    QList<PublicationSummary> rows;
    rows.reserve(limit);
    while (select->step()) {
        PublicationSummary row;
        row.id = select->int64At(0);
        row.type = publicationTypeFromDb(select->textAt(1)).value_or(PublicationType::Other);
        row.title = select->textAt(2);
        row.authors = select->textAt(3);
        row.year = select->optIntAt(4);
        row.containerTitle = select->textAt(5);
        row.readStatus = readStatusFromDb(select->textAt(6)).value_or(ReadStatus::Unread);
        rows << row;
    }
    return rows;
}

QList<qint64> LibraryQuery::ids(const PublicationFilter &filter, const PublicationSort &sort)
{
    const ListQuery query = buildListQuery(filter, sort, searchClause(filter));
    auto select = m_db.prepare("SELECT p.id" + query.sql);
    bindAll(*select, query.bindings);
    QList<qint64> ids;
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

QList<FacetValue> LibraryQuery::facet(const PublicationFilter &filter, Facet facet, int limit)
{
    Conditions c = buildConditions(withoutOwnSelection(filter, facet), searchClause(filter));
    QByteArray sql;
    switch (facet) {
    case Facet::Type: sql = columnFacetSql("p.type", c); break;
    case Facet::Journal: sql = columnFacetSql("p.container_title", c); break;
    case Facet::Publisher: sql = columnFacetSql("p.publisher", c); break;
    case Facet::Conference: sql = columnFacetSql("p.conference_name", c); break;
    case Facet::Institution: sql = columnFacetSql("p.institution", c); break;
    case Facet::Language: sql = columnFacetSql("p.language", c); break;
    case Facet::ReadStatus: sql = columnFacetSql("p.read_status", c); break;
    case Facet::Author: {
        // With nothing selected there's no need to go through the publication table at all.
        const QByteArray scope = c.clauses.isEmpty()
            ? QByteArray()
            : " AND pp.publication_id IN (SELECT p.id FROM publication p" + c.whereClause() + ")";
        sql = "SELECT pr.id, CASE WHEN pr.given_name = '' THEN pr.family_name "
              "ELSE pr.family_name || ', ' || pr.given_name END, count(*) "
              "FROM publication_person pp JOIN person pr ON pr.id = pp.person_id "
              "WHERE pp.role = 'author'" + scope
            + " GROUP BY pr.id ORDER BY count(*) DESC, pr.name_key LIMIT ?";
        break;
    }
    case Facet::Format: {
        // Counts publications rather than files, and includes articles through their volume.
        c.add("f.format IS NOT NULL");
        sql = "SELECT f.format, f.format, count(DISTINCT p.id) FROM publication p "
              "JOIN publication_file pf ON pf.publication_id = " + kOwnerOfFile + " "
              "JOIN file f ON f.id = pf.file_id" + c.whereClause()
            + " GROUP BY f.format ORDER BY count(DISTINCT p.id) DESC LIMIT ?";
        break;
    }
    }

    auto select = m_db.prepare(sql);
    bindAll(*select, c.bindings);
    select->bind(int(c.bindings.size()) + 1, limit);

    QList<FacetValue> values;
    while (select->step())
        values << FacetValue{select->textAt(0), select->textAt(1), select->int64At(2)};
    return values;
}

QList<TagCount> LibraryQuery::tagCounts(const PublicationFilter &filter)
{
    PublicationFilter scoped = filter;
    scoped.tagIds.clear();
    const Conditions c = buildConditions(scoped, searchClause(scoped));

    // Read the tree first, then roll the matching publications up through it in memory. Each
    // publication counts once per tag, even if it has two tags under the same parent.
    QList<TagCount> tags;
    QHash<qint64, std::optional<qint64>> parents;
    {
        auto select = m_db.prepare("SELECT id, name, parent_id FROM tag ORDER BY name COLLATE NOCASE");
        while (select->step()) {
            TagCount tag;
            tag.tagId = select->int64At(0);
            tag.name = select->textAt(1);
            tag.parentId = select->optInt64At(2);
            parents.insert(tag.tagId, tag.parentId);
            tags << tag;
        }
    }

    const QByteArray scope = c.clauses.isEmpty()
        ? QByteArray()
        : " WHERE publication_id IN (SELECT p.id FROM publication p" + c.whereClause() + ")";
    auto pairs = m_db.prepare("SELECT publication_id, tag_id FROM publication_tag" + scope
                              + " ORDER BY publication_id");
    bindAll(*pairs, c.bindings);

    QHash<qint64, qint64> counts;
    qint64 current = -1;
    QSet<qint64> counted;
    while (pairs->step()) {
        const qint64 publication = pairs->int64At(0);
        if (publication != current) {
            current = publication;
            counted.clear();
        }
        std::optional<qint64> tag = pairs->int64At(1);
        while (tag && !counted.contains(*tag)) {
            counted.insert(*tag);
            ++counts[*tag];
            tag = parents.value(*tag);
        }
    }

    for (TagCount &tag : tags)
        tag.count = counts.value(tag.tagId);
    return tags;
}

FlagCounts LibraryQuery::flagCounts(const PublicationFilter &filter)
{
    const auto countWith = [&](PublicationFilter variant) { return count(variant); };

    FlagCounts counts;
    PublicationFilter needsReview = filter;
    needsReview.needsReview = true;
    counts.needsReview = countWith(needsReview);

    PublicationFilter noText = filter;
    noText.noText = true;
    counts.noText = countWith(noText);

    PublicationFilter noFile = filter;
    noFile.noFile = true;
    counts.noFile = countWith(noFile);

    PublicationFilter missing = filter;
    missing.missingOrOffline = true;
    counts.missingOrOffline = countWith(missing);
    return counts;
}

std::optional<std::pair<int, int>> LibraryQuery::yearRange(const PublicationFilter &filter)
{
    PublicationFilter scoped = filter;
    scoped.yearMin.reset();
    scoped.yearMax.reset();
    const Conditions c = buildConditions(scoped, searchClause(scoped));
    auto select = m_db.prepare("SELECT min(p.year), max(p.year) FROM publication p" + c.whereClause());
    bindAll(*select, c.bindings);
    select->step();
    if (select->isNull(0))
        return std::nullopt;
    return std::make_pair(select->intAt(0), select->intAt(1));
}

} // namespace papertrail::core
