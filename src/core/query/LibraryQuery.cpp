#include "core/query/LibraryQuery.h"

#include "core/db/Database.h"
#include "core/repo/FullTextIndex.h"
#include "core/repo/ReadingStateRepository.h"
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
Conditions buildConditions(const PublicationFilter &filter, const QByteArray &ftsClause, bool anyHidden)
{
    Conditions c;
    // Left out if there's nothing to hide, because some queries take a quicker route with no conditions.
    if (anyHidden)
        c.add("p.id NOT IN (SELECT publication_id FROM hidden_publication)");

    const ParsedSearch search = parseSearch(filter.searchText);

    if (!ftsClause.isEmpty())
        c.add(ftsClause);

    if (!filter.collectionIds.isEmpty()) {
        c.addInIds("p.id IN (SELECT publication_id FROM collection_publication WHERE collection_id IN ("
                   "WITH RECURSIVE down(id) AS (SELECT id FROM collection WHERE id IN",
                   " UNION SELECT cp.collection_id FROM collection_parent cp JOIN down ON cp.parent_id = down.id) SELECT id FROM down))",
                   filter.collectionIds);
    }

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
    for (const QString &notes : search.notesMatches) {
        c.add("p.id IN (SELECT publication_id FROM annotation WHERE id IN "
              "(SELECT rowid FROM annotation_fts WHERE annotation_fts MATCH ?) "
              "UNION SELECT rowid FROM publication_note_fts WHERE publication_note_fts MATCH ?)");
        c.bindings << notes << notes;
    }
    if (filter.parentId) {
        c.add("p.parent_id = ?");
        c.bindings << *filter.parentId;
    }
    for (const QString &holder : search.inMatches) {
        c.add("p.parent_id IN (SELECT rowid FROM publication_fts WHERE publication_fts MATCH ?)");
        c.bindings << holder;
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
    c.addIn("p.court", filter.courts);
    c.addIn("p.jurisdiction", filter.jurisdictions);
    c.addIn("p.language", filter.languages);

    if (!filter.formats.isEmpty()) {
        c.add(kOwnerOfFile + " IN (SELECT pf.publication_id FROM publication_file pf "
              "JOIN file f ON f.id = pf.file_id WHERE f.format IN ("
              + c.placeholders(filter.formats.size()) + "))");
        for (const QString &format : filter.formats)
            c.bindings << format;
    }

    // Read statuses and being opened recently are offered together, so either will do.
    if (!filter.readStatuses.isEmpty() || filter.openedSince) {
        QByteArrayList either;
        if (!filter.readStatuses.isEmpty()) {
            either << "p.read_status IN (" + c.placeholders(filter.readStatuses.size()) + ")";
            for (const ReadStatus status : filter.readStatuses)
                c.bindings << toDbString(status);
        }
        if (filter.openedSince) {
            either << "p.id IN (SELECT publication_id FROM reading_state WHERE last_opened >= ?)";
            c.bindings << *filter.openedSince;
        }
        c.add("(" + either.join(" OR ") + ")");
    }

    if (!filter.ratings.isEmpty()) {
        QList<qint64> given;
        for (const int rating : filter.ratings) {
            if (rating >= 1 && rating <= 5)
                given << rating;
        }
        QByteArray clause = given.isEmpty() ? QByteArray("0") : "p.rating IN (" + c.placeholders(given.size()) + ")";
        if (filter.ratings.contains(0))
            clause = "(" + clause + " OR p.rating IS NULL)";
        c.add(clause);
        for (const qint64 rating : given)
            c.bindings << rating;
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
    if (filter.unreadable) {
        c.add(kOwnerOfFile + " IN (SELECT pf.publication_id FROM publication_file pf "
              "JOIN file f ON f.id = pf.file_id WHERE f.status = 'unreadable')");
    }
    if (filter.retracted) {
        c.add("p.id IN (SELECT publication_id FROM publication_notice WHERE kind IN "
              "('retraction', 'partial_retraction', 'withdrawal', 'removal', 'expression_of_concern'))");
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
    case SortField::Rating:
        // As for years, these have to match the rating indexes, and keep anything unrated last.
        return sort.ascending ? " ORDER BY coalesce(p.rating, 6) ASC, p.title_sort ASC, p.id ASC"
                              : " ORDER BY coalesce(p.rating, 0) DESC, p.title_sort ASC, p.id ASC";
    case SortField::LastOpened:
        // Anything never opened comes last either way.
        return sort.ascending
            ? " ORDER BY coalesce((SELECT last_opened FROM reading_state WHERE publication_id = p.id), 9223372036854775807) ASC, "
              "p.title_sort ASC, p.id ASC"
            : " ORDER BY coalesce((SELECT last_opened FROM reading_state WHERE publication_id = p.id), 0) DESC, "
              "p.title_sort ASC, p.id ASC";
    }
    return " ORDER BY p.title_sort ASC, p.id ASC";
}

// The FROM clause, conditions and ordering shared by page() and ids().
struct ListQuery {
    QByteArray sql;
    QList<Binding> bindings;
};

ListQuery buildListQuery(const PublicationFilter &filter, const PublicationSort &sort, const QByteArray &ftsClause, bool anyHidden,
                         bool matchedBeyondDetails)
{
    const QString ftsMatch = parseSearch(filter.searchText).ftsMatch;
    ListQuery query;
    // How well words match inside documents can't be compared with how well they match a title, so a search
    // that includes them falls through to the ordinary order, which treats best match as by title.
    // So do matches found only in highlights and notes.
    if (sort.field == SortField::Relevance && !ftsMatch.isEmpty() && !filter.searchContents && !matchedBeyondDetails) {
        Conditions c = buildConditions(filter, QByteArray(), anyHidden);
        query.sql = " FROM publication_fts JOIN publication p ON p.id = publication_fts.rowid "
                    "WHERE publication_fts MATCH ?";
        query.bindings << ftsMatch;
        for (const QByteArray &clause : c.clauses)
            query.sql += " AND " + clause;
        query.bindings += c.bindings;
        // Column weights: title, abstract, authors, tags, container, publisher, identifiers.
        query.sql += " ORDER BY bm25(publication_fts, 10.0, 1.0, 5.0, 3.0, 2.0, 1.0, 8.0), p.id";
    } else {
        Conditions c = buildConditions(filter, ftsClause, anyHidden);
        // The best match for the articles of an issue is the order they're in it.
        const bool pageOrder = sort.field == SortField::Relevance
            && (filter.parentId || !parseSearch(filter.searchText).inMatches.isEmpty());
        query.sql = " FROM publication p" + c.whereClause()
            + (pageOrder ? QByteArray(" ORDER BY p.parent_id, coalesce(p.page_start, 2147483647), p.title_sort, p.id") : orderBy(sort));
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
    case Facet::Court: filter.courts.clear(); break;
    case Facet::Jurisdiction: filter.jurisdictions.clear(); break;
    case Facet::Language: filter.languages.clear(); break;
    case Facet::Format: filter.formats.clear(); break;
    case Facet::ReadStatus:
        filter.readStatuses.clear();
        filter.openedSince.reset();
        break;
    case Facet::Rating: filter.ratings.clear(); break;
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

    const ParsedSearch parsed = parseSearch(filter.searchText);
    const bool contents = filter.searchContents && !parsed.contentsMatch.isEmpty() && hasFullText(m_db);
    QByteArray stamp = m_db.changeStamp();
    if (contents) {
        // Indexing happens on another connection, and in another file from the one the stamp looks at.
        Statement &version = m_db.cached("PRAGMA fulltext.data_version");
        version.step();
        stamp += '/' + QByteArray::number(version.int64At(0));
        version.reset();
    }

    if (match != m_searchText || contents != m_searchedContents || stamp != m_searchStamp) {
        m_db.exec("CREATE TEMP TABLE IF NOT EXISTS search_match (id INTEGER PRIMARY KEY)");
        m_db.exec("DELETE FROM temp.search_match");
        Statement &fill = m_db.cached(
            "INSERT INTO temp.search_match SELECT rowid FROM publication_fts WHERE publication_fts MATCH ?");
        fill.bind(1, match);
        fill.step();
        fill.reset();
        // Whether any were found only by what's in highlights and notes, which can't be ranked with the details.
        m_matchesBeyondDetails = false;
        if (!parsed.contentsMatch.isEmpty()) {
            addNoteMatches(parsed.contentsMatch, parsed.fieldMatch);
            m_matchesBeyondDetails = m_db.changes() > 0;
        }
        if (contents)
            addContentMatches(parsed.contentsMatch, parsed.fieldMatch);

        Statement &size = m_db.cached("SELECT count(*) FROM temp.search_match");
        size.step();
        m_searchCount = size.int64At(0);
        size.reset();

        m_searchText = match;
        m_searchedContents = contents;
        m_searchStamp = m_db.changeStamp(); // Taken last, since filling the table changes it.
        if (contents) {
            Statement &version = m_db.cached("PRAGMA fulltext.data_version");
            version.step();
            m_searchStamp += '/' + QByteArray::number(version.int64At(0));
            version.reset();
        }
    }

    // A leading + stops SQLite using this condition to choose its starting point.
    return m_searchCount > kWalkInOrderAbove ? "+p.id IN temp.search_match" : "p.id IN temp.search_match";
}

bool LibraryQuery::anyHidden()
{
    Statement &select = m_db.cached("SELECT EXISTS (SELECT 1 FROM hidden_publication)");
    select.step();
    const bool any = select.intAt(0) != 0;
    select.reset();
    return any;
}

void LibraryQuery::addNoteMatches(const QString &contentsMatch, const QString &fieldMatch)
{
    // As with documents, a field search can only be answered from the details, so it has to agree too.
    const QByteArray agreeing = fieldMatch.isEmpty()
        ? QByteArray()
        : " AND id IN (SELECT rowid FROM publication_fts WHERE publication_fts MATCH ?2)";
    auto add = m_db.prepare(
        "INSERT OR IGNORE INTO temp.search_match SELECT id FROM publication WHERE id IN ("
        "SELECT publication_id FROM annotation WHERE id IN (SELECT rowid FROM annotation_fts WHERE annotation_fts MATCH ?1) "
        "UNION SELECT rowid FROM publication_note_fts WHERE publication_note_fts MATCH ?1)" + agreeing);
    add->bind(1, contentsMatch);
    if (!fieldMatch.isEmpty())
        add->bind(2, fieldMatch);
    add->step();
}

void LibraryQuery::addContentMatches(const QString &contentsMatch, const QString &fieldMatch)
{
    m_db.exec("CREATE TEMP TABLE IF NOT EXISTS content_hit (document INTEGER, page INTEGER)");
    m_db.exec("DELETE FROM temp.content_hit");
    {
        auto hits = m_db.prepare("INSERT INTO temp.content_hit SELECT rowid / ?1, rowid % ?1 FROM fulltext.page_text WHERE page_text MATCH ?2");
        hits->bind(1, kFullTextPagesPerDocument);
        hits->bind(2, contentsMatch);
        hits->step();
    }

    // Field searches such as author: can only be answered from the metadata, so they still have to agree.
    const QByteArray agreeing = fieldMatch.isEmpty()
        ? QByteArray()
        : " WHERE pf.publication_id IN (SELECT rowid FROM publication_fts WHERE publication_fts MATCH ?)";
    {
        auto whole = m_db.prepare(
            "INSERT OR IGNORE INTO temp.search_match "
            "SELECT DISTINCT pf.publication_id FROM (SELECT DISTINCT document FROM temp.content_hit) h "
            "JOIN fulltext.document d ON d.id = h.document JOIN file f ON f.sha256 = d.sha256 "
            "JOIN publication_file pf ON pf.file_id = f.id" + agreeing);
        if (!fieldMatch.isEmpty())
            whole->bind(1, fieldMatch);
        whole->step();
    }
    {
        // An article is found by what's on its own pages of its volume's file.
        const QByteArray articleAgreeing = fieldMatch.isEmpty()
            ? QByteArray()
            : " AND c.id IN (SELECT rowid FROM publication_fts WHERE publication_fts MATCH ?)";
        auto articles = m_db.prepare(
            "INSERT OR IGNORE INTO temp.search_match "
            "SELECT DISTINCT c.id FROM publication c "
            "JOIN publication_file pf ON pf.publication_id = c.parent_id JOIN file f ON f.id = pf.file_id "
            "JOIN fulltext.document d ON d.sha256 = f.sha256 "
            "JOIN temp.content_hit h ON h.document = d.id AND h.page + 1 BETWEEN c.page_start AND c.page_end "
            "WHERE c.parent_id IS NOT NULL" + articleAgreeing);
        if (!fieldMatch.isEmpty())
            articles->bind(1, fieldMatch);
        articles->step();
    }
    m_db.exec("DELETE FROM temp.content_hit");
}

qint64 LibraryQuery::count(const PublicationFilter &filter)
{
    const QByteArray fts = searchClause(filter);
    const Conditions c = buildConditions(filter, fts, anyHidden());
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
    const QByteArray clause = searchClause(filter);
    const QByteArray columns =
        "SELECT p.id, p.type, p.title, p.authors_display, p.year, p.container_title, p.read_status, "
        "p.needs_review, coalesce(p.parent_id, p.id), CASE WHEN p.parent_id IS NULL THEN NULL ELSE p.page_start END, "
        "p.volume, p.issue, p.month, p.rating, CASE WHEN p.parent_id IS NULL THEN NULL ELSE p.page_end END, "
        "(SELECT CASE WHEN max(kind = 'retraction') THEN 'retraction' WHEN max(kind = 'partial_retraction') THEN 'partial_retraction' "
        "WHEN max(kind IN ('withdrawal', 'removal')) THEN 'withdrawal' END FROM publication_notice WHERE publication_id = p.id)";

    // Each part of the page, as SQL from FROM on, its bindings, and how many rows from where.
    struct Part {
        QByteArray sql;
        QList<Binding> bindings;
        qint64 offset = 0;
        int limit = 0;
    };
    QList<Part> parts;
    if (sort.field == SortField::LastOpened) {
        // Sorting everything by when it was opened means looking each one up, which is slow at 100,000. Only those
        // opened have a time, so they come first through the index on it, and then everything else by title.
        const Conditions c = buildConditions(filter, clause, anyHidden());
        QByteArray where = c.whereClause();
        where += where.isEmpty() ? " WHERE " : " AND ";
        const QByteArray opened = " FROM reading_state r JOIN publication p ON p.id = r.publication_id" + where
            + "r.last_opened IS NOT NULL";
        auto count = m_db.prepare("SELECT count(*)" + opened);
        bindAll(*count, c.bindings);
        count->step();
        const qint64 openedCount = count->int64At(0);
        const QByteArray direction = sort.ascending ? " ASC" : " DESC";
        if (offset < openedCount)
            parts << Part{opened + " ORDER BY r.last_opened" + direction + ", p.title_sort ASC, p.id ASC", c.bindings, offset,
                          int(qMin<qint64>(limit, openedCount - offset))};
        const int rest = limit - (parts.isEmpty() ? 0 : parts.first().limit);
        if (rest > 0) {
            parts << Part{" FROM publication p" + where
                              + "p.id NOT IN (SELECT publication_id FROM reading_state WHERE last_opened IS NOT NULL)"
                              " ORDER BY p.title_sort ASC, p.id ASC",
                          c.bindings, qMax<qint64>(0, offset - openedCount), rest};
        }
    } else {
        const ListQuery query = buildListQuery(filter, sort, clause, anyHidden(), m_matchesBeyondDetails);
        parts << Part{query.sql, query.bindings, offset, limit};
    }

    QList<PublicationSummary> rows;
    QList<qint64> fileOwners;
    QList<int> firstPages; // An article's, for its cover. 0 for anything else.
    QList<int> lastPages;
    rows.reserve(limit);
    fileOwners.reserve(limit);
    firstPages.reserve(limit);
    for (const Part &part : std::as_const(parts)) {
        auto select = m_db.prepare(columns + part.sql + " LIMIT ? OFFSET ?");
        bindAll(*select, part.bindings);
        select->bind(int(part.bindings.size()) + 1, part.limit);
        select->bind(int(part.bindings.size()) + 2, part.offset);
        while (select->step()) {
            PublicationSummary row;
            row.id = select->int64At(0);
            row.type = publicationTypeFromDb(select->textAt(1)).value_or(PublicationType::Other);
            row.title = select->textAt(2);
            row.authors = select->textAt(3);
            row.year = select->optIntAt(4);
            row.containerTitle = select->textAt(5);
            row.readStatus = readStatusFromDb(select->textAt(6)).value_or(ReadStatus::Unread);
            row.needsReview = select->intAt(7) != 0;
            fileOwners << select->int64At(8);
            firstPages << select->optIntAt(9).value_or(0);
            row.volume = select->textAt(10);
            row.issue = select->textAt(11);
            row.month = select->optIntAt(12);
            row.rating = select->optIntAt(13);
            lastPages << select->optIntAt(14).value_or(0);
            row.coverNotice = select->textAt(15);
            rows << row;
        }
    }

    // Read afterwards rather than joined, so it only ever costs one small lookup per row shown.
    Statement &file = m_db.cached(
        "SELECT f.sha256, f.format, f.status, f.page_count, "
        "(SELECT group_concat(DISTINCT f2.format) FROM publication_file pf2 JOIN file f2 ON f2.id = pf2.file_id "
        " WHERE pf2.publication_id = pf.publication_id) "
        "FROM publication_file pf JOIN file f ON f.id = pf.file_id "
        "WHERE pf.publication_id = ? ORDER BY pf.is_primary DESC, f.id LIMIT 1");
    Statement &reading = m_db.cached("SELECT location, last_opened FROM reading_state WHERE publication_id = ?");
    for (qsizetype i = 0; i < rows.size(); ++i) {
        std::optional<int> pageCount;
        file.bind(1, fileOwners.at(i));
        if (file.step()) {
            rows[i].coverSha = articleCoverKey(file.textAt(0), firstPages.at(i));
            rows[i].format = file.textAt(1);
            rows[i].fileStatus = file.textAt(2);
            pageCount = file.optIntAt(3);
            rows[i].formats = file.textAt(4).split(u',', Qt::SkipEmptyParts);
            // The main file's format comes first, wherever SQLite happened to put it.
            if (rows[i].formats.removeOne(rows[i].format))
                rows[i].formats.prepend(rows[i].format);
        }
        file.reset();

        reading.bind(1, rows.at(i).id);
        if (reading.step()) {
            rows[i].lastOpened = reading.optInt64At(1).value_or(0);
            rows[i].progress = readingProgress(reading.textAt(0), firstPages.at(i), lastPages.at(i), pageCount);
        }
        reading.reset();
    }
    return rows;
}

QList<qint64> LibraryQuery::ids(const PublicationFilter &filter, const PublicationSort &sort)
{
    const QByteArray clause = searchClause(filter);
    const ListQuery query = buildListQuery(filter, sort, clause, anyHidden(), m_matchesBeyondDetails);
    auto select = m_db.prepare("SELECT p.id" + query.sql);
    bindAll(*select, query.bindings);
    QList<qint64> ids;
    while (select->step())
        ids << select->int64At(0);
    return ids;
}

QList<FacetValue> LibraryQuery::facet(const PublicationFilter &filter, Facet facet, int limit)
{
    Conditions c = buildConditions(withoutOwnSelection(filter, facet), searchClause(filter), anyHidden());
    QByteArray sql;
    switch (facet) {
    case Facet::Type: sql = columnFacetSql("p.type", c); break;
    case Facet::Journal: sql = columnFacetSql("p.container_title", c); break;
    case Facet::Publisher: sql = columnFacetSql("p.publisher", c); break;
    case Facet::Conference: sql = columnFacetSql("p.conference_name", c); break;
    case Facet::Institution: sql = columnFacetSql("p.institution", c); break;
    case Facet::Court: sql = columnFacetSql("p.court", c); break;
    case Facet::Jurisdiction: sql = columnFacetSql("p.jurisdiction", c); break;
    case Facet::Language: sql = columnFacetSql("p.language", c); break;
    case Facet::ReadStatus: sql = columnFacetSql("p.read_status", c); break;
    case Facet::Rating:
        // Unrated too, as "0", after the rest.
        sql = "SELECT coalesce(p.rating, 0), coalesce(p.rating, 0), count(*) FROM publication p" + c.whereClause()
            + " GROUP BY 1 ORDER BY 1 DESC LIMIT ?";
        break;
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
    const Conditions c = buildConditions(scoped, searchClause(scoped), anyHidden());

    // Read the tree first, then roll the matching publications up through it in memory. Each
    // publication counts once per tag, even if it has two tags under the same parent.
    QList<TagCount> tags;
    QHash<qint64, std::optional<qint64>> parents;
    {
        auto select = m_db.prepare("SELECT id, name, parent_id, colour FROM tag ORDER BY name COLLATE NOCASE");
        while (select->step()) {
            TagCount tag;
            tag.tagId = select->int64At(0);
            tag.name = select->textAt(1);
            tag.parentId = select->optInt64At(2);
            tag.colour = select->textAt(3);
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

QList<CollectionCount> LibraryQuery::collectionCounts(const PublicationFilter &filter)
{
    PublicationFilter others = filter;
    others.collectionIds.clear();
    const Conditions c = buildConditions(others, searchClause(others), anyHidden());
    const QByteArray scope = c.clauses.isEmpty()
        ? QByteArray()
        : " WHERE m.publication_id IN (SELECT p.id FROM publication p" + c.whereClause() + ")";
    // Each collection with every one inside it, so a publication in two of them is counted once.
    auto select = m_db.prepare(
        "WITH RECURSIVE tree(top, id) AS (SELECT id, id FROM collection "
        "UNION SELECT tree.top, cp.collection_id FROM collection_parent cp JOIN tree ON cp.parent_id = tree.id), "
        "counted(top, n) AS (SELECT tree.top, count(DISTINCT m.publication_id) FROM tree "
        "JOIN collection_publication m ON m.collection_id = tree.id" + scope + " GROUP BY tree.top) "
        "SELECT c.id, c.name, coalesce(counted.n, 0), cp.parent_id FROM collection c LEFT JOIN counted ON counted.top = c.id "
        "LEFT JOIN collection_parent cp ON cp.collection_id = c.id "
        "ORDER BY c.name");
    bindAll(*select, c.bindings);

    QList<CollectionCount> values;
    while (select->step())
        values << CollectionCount{select->textAt(0), select->textAt(1), select->int64At(2), select->optInt64At(3)};
    return values;
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

    PublicationFilter unreadable = filter;
    unreadable.unreadable = true;
    counts.unreadable = countWith(unreadable);

    PublicationFilter retracted = filter;
    retracted.retracted = true;
    counts.retracted = countWith(retracted);
    return counts;
}

qint64 LibraryQuery::openedSinceCount(const PublicationFilter &filter, qint64 since)
{
    PublicationFilter opened = withoutOwnSelection(filter, Facet::ReadStatus);
    opened.openedSince = since;
    return count(opened);
}

std::optional<std::pair<int, int>> LibraryQuery::yearRange(const PublicationFilter &filter)
{
    PublicationFilter scoped = filter;
    scoped.yearMin.reset();
    scoped.yearMax.reset();
    const Conditions c = buildConditions(scoped, searchClause(scoped), anyHidden());
    auto select = m_db.prepare("SELECT min(p.year), max(p.year) FROM publication p" + c.whereClause());
    bindAll(*select, c.bindings);
    select->step();
    if (select->isNull(0))
        return std::nullopt;
    return std::make_pair(select->intAt(0), select->intAt(1));
}

} // namespace papertrail::core
