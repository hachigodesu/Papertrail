// Times the library's queries on a large synthetic library, against the plan's targets.
//
// Environment:
//   PAPERTRAIL_PERF_COUNT  publications to generate (default 100000)
//   PAPERTRAIL_PERF_DB     keep the library at this path and reuse it on later runs
//   PAPERTRAIL_PERF_REPORT_ONLY  print timings but never fail, for shared CI machines

#include "SyntheticLibrary.h"

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/TagRepository.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <vector>

using namespace papertrail;
using namespace papertrail::core;

namespace {

constexpr double kResultTargetMs = 100.0;

// Ranking by relevance has to score every match, and it's an optional sort rather than the
// default. This limit is the worst case: a word found in half of the library.
constexpr double kRankedLimitMs = 250.0;

constexpr int kPageSize = 100;

double medianMs(const std::function<void()> &work, int runs = 5)
{
    std::vector<double> times;
    for (int i = 0; i < runs; ++i) {
        QElapsedTimer timer;
        timer.start();
        work();
        times.push_back(timer.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    return times.at(times.size() / 2);
}

} // namespace

class QueryPerfTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const int count = qEnvironmentVariableIntValue("PAPERTRAIL_PERF_COUNT");
        m_count = count > 0 ? count : 100000;

        QString path = qEnvironmentVariable("PAPERTRAIL_PERF_DB");
        if (path.isEmpty())
            path = m_dir.filePath(QStringLiteral("perf.db"));
        const bool reuse = QFile::exists(path);

        m_db = std::make_unique<Database>(path);
        migrateLibrary(*m_db);
        if (!reuse) {
            QElapsedTimer timer;
            timer.start();
            tools::SyntheticOptions options;
            options.publications = m_count;
            tools::generateSyntheticLibrary(*m_db, options);
            m_db->exec("PRAGMA optimize");
            m_out << "Generated " << m_count << " publications in " << timer.elapsed() / 1000.0 << " s\n";
        }

        m_query = std::make_unique<LibraryQuery>(*m_db);
        m_total = m_query->count({});
        m_out << "Library holds " << m_total << " publications\n\n";
        m_out.flush();
    }

    // What the list needs when a filter or search changes: the first page and the total.
    void resultsMeetTheTarget()
    {
        TagRepository tags(*m_db);
        const QList<Tag> allTags = tags.all();
        qint64 rootTag = 0;
        for (const Tag &tag : allTags) {
            if (tag.name == QStringLiteral("Engineering"))
                rootTag = tag.id;
        }
        const qint64 topAuthor = m_query->facet({}, Facet::Author, 1).first().key.toLongLong();
        const QString topJournal = m_query->facet({}, Facet::Journal, 1).first().key;

        struct Case {
            const char *name;
            PublicationFilter filter;
            PublicationSort sort;
        };
        std::vector<Case> cases;
        const auto add = [&](const char *name, PublicationFilter filter, PublicationSort sort = {}) {
            cases.push_back({name, std::move(filter), sort});
        };
        const auto search = [](const char *text) {
            PublicationFilter filter;
            filter.searchText = QString::fromUtf8(text);
            return filter;
        };

        add("everything, by title", {});
        add("everything, by author", {}, {SortField::FirstAuthor, true});
        add("everything, newest first", {}, {SortField::Year, false});
        add("everything, oldest first", {}, {SortField::Year, true});
        add("everything, recently added", {}, {SortField::Added, false});

        PublicationFilter articles;
        articles.tabTypes = typesForTab(LibraryTab::JournalArticles);
        add("articles tab", articles);
        add("articles tab, newest first", articles, {SortField::Year, false});

        PublicationFilter standards;
        standards.tabTypes = typesForTab(LibraryTab::Standards);
        add("standards tab (small)", standards);

        PublicationFilter tag;
        tag.tagIds = {rootTag};
        add("top-level tag and everything under it", tag);

        PublicationFilter author;
        author.authorIds = {topAuthor};
        add("most common author", author);

        PublicationFilter journal;
        journal.journals = {topJournal};
        add("most common journal", journal);

        PublicationFilter years;
        years.yearMin = 2015;
        years.yearMax = 2020;
        add("year range", years);

        PublicationFilter format;
        format.formats = {QStringLiteral("pdf")};
        add("format: pdf", format);

        PublicationFilter noFile;
        noFile.noFile = true;
        add("no file linked", noFile);

        PublicationFilter noText;
        noText.noText = true;
        add("no text layer", noText);

        PublicationFilter combined;
        combined.tabTypes = typesForTab(LibraryTab::JournalArticles);
        combined.tagIds = {rootTag};
        combined.yearMin = 2010;
        combined.readStatuses = {ReadStatus::Unread};
        combined.languages = {QStringLiteral("en")};
        add("combined: tab, tag, year, status, language", combined);

        add("search: common word", search("analysis"));
        add("search: common word, ranked", search("analysis"), {SortField::Relevance, true});
        add("search: two words", search("finite element"));
        add("search: exact phrase", search("\"finite element\""));
        add("search: two words, ranked", search("finite element"), {SortField::Relevance, true});
        add("search: short prefix", search("a"));
        add("search: author field", search("author:smith1"));
        add("search: doi prefix", search("doi:10.1234/"));
        add("search: tag name", search("tag:engineering"));
        add("search: year and word", search("year:>2015 turbulent"));

        m_out << "Results: first page and total, median of 5 (target " << kResultTargetMs
              << " ms, or " << kRankedLimitMs << " ms when ranked)\n";
        QStringList failures;
        for (const Case &c : cases) {
            const double ms = medianMs([&] {
                // A new LibraryQuery each time, so every search is timed as a first search
                // rather than reusing the matches from the previous run.
                LibraryQuery query(*m_db);
                query.page(c.filter, c.sort, 0, kPageSize);
                query.count(c.filter);
            });
            const double target = c.sort.field == SortField::Relevance ? kRankedLimitMs : kResultTargetMs;
            report(c.name, ms, target);
            if (ms > target)
                failures << QString::fromUtf8(c.name);
        }
        m_out << '\n';
        m_out.flush();
        if (!reportOnly())
            QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral(", "))));
    }

    void aCollectionMeetsTheTarget()
    {
        CollectionRepository collections(*m_db);
        const qint64 id = collections.create(QStringLiteral("Performance test"));
        m_db->exec("INSERT INTO collection_publication (collection_id, publication_id, added_at) "
                   "SELECT (SELECT max(id) FROM collection), id, 0 FROM publication WHERE id % 20 = 0");
        PublicationFilter filter;
        filter.collectionIds = {id};
        PublicationFilter word = filter;
        word.searchText = QStringLiteral("analysis");

        m_out << "With a collection of a twentieth of the library (target " << kResultTargetMs << " ms)\n";
        QStringList failures;
        const auto time = [&](const char *name, const PublicationFilter &f, const PublicationSort &sort) {
            const double ms = medianMs([&] {
                LibraryQuery query(*m_db);
                query.page(f, sort, 0, kPageSize);
                query.count(f);
            });
            report(name, ms, kResultTargetMs);
            if (ms > kResultTargetMs)
                failures << QString::fromUtf8(name);
        };
        time("collection, by title", filter, {});
        time("collection, newest first", filter, {SortField::Year, false});
        time("collection, search", word, {});
        m_out << '\n';
        m_out.flush();
        collections.remove(id);
        if (!reportOnly())
            QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral(", "))));
    }

    void hiddenPublicationsDontSlowResults()
    {
        // A tenth of the library hidden, which is the slower path for these queries.
        m_db->exec("INSERT INTO hidden_publication (publication_id) SELECT id FROM publication WHERE id % 10 = 0");
        PublicationFilter articles;
        articles.tabTypes = typesForTab(LibraryTab::JournalArticles);
        PublicationFilter word;
        word.searchText = QStringLiteral("analysis");

        m_out << "With a tenth of the library hidden (target " << kResultTargetMs << " ms)\n";
        QStringList failures;
        const auto time = [&](const char *name, const PublicationFilter &filter) {
            const double ms = medianMs([&] {
                LibraryQuery query(*m_db);
                query.page(filter, {}, 0, kPageSize);
                query.count(filter);
            });
            report(name, ms, kResultTargetMs);
            if (ms > kResultTargetMs)
                failures << QString::fromUtf8(name);
        };
        time("everything, by title", {});
        time("articles tab", articles);
        time("search: common word", word);
        m_out << '\n';
        m_out.flush();
        m_db->exec("DELETE FROM hidden_publication");
        if (!reportOnly())
            QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral(", "))));
    }

    void scrollingStaysFastDeepIntoTheList()
    {
        const PublicationSort sort{SortField::Title, true};
        m_out << "Scrolling: one page at a position in the full list\n";
        double worst = 0;
        for (const double position : {0.0, 0.5, 0.9}) {
            const qint64 offset = qint64(m_total * position);
            const double ms = medianMs([&] { m_query->page({}, sort, offset, kPageSize); });
            report(qPrintable(QStringLiteral("offset %1").arg(offset)), ms, kResultTargetMs);
            worst = std::max(worst, ms);
        }
        m_out << '\n';
        m_out.flush();
        if (!reportOnly())
            QVERIFY(worst <= kResultTargetMs);
    }

    // The sidebar. Measured and reported here, since it updates after the list and in the background.
    void reportSidebarTimes()
    {
        PublicationFilter none;
        PublicationFilter articles;
        articles.tabTypes = typesForTab(LibraryTab::JournalArticles);
        PublicationFilter searching;
        searching.searchText = QStringLiteral("analysis");

        for (const auto &[label, filter] : {std::pair<const char *, const PublicationFilter &>{"nothing selected", none},
                                            {"articles tab", articles},
                                            {"searching a common word", searching}}) {
            m_out << "Sidebar, " << label << " (no target yet: reported only)\n";
            double total = 0;
            const auto measure = [&](const char *name, const std::function<void()> &work) {
                const double ms = medianMs(work, 3);
                total += ms;
                report(name, ms, 0);
            };
            measure("type", [&] { m_query->facet(filter, Facet::Type); });
            measure("tags, rolled up", [&] { m_query->tagCounts(filter); });
            measure("author", [&] { m_query->facet(filter, Facet::Author); });
            measure("journal", [&] { m_query->facet(filter, Facet::Journal); });
            measure("publisher", [&] { m_query->facet(filter, Facet::Publisher); });
            measure("conference", [&] { m_query->facet(filter, Facet::Conference); });
            measure("institution", [&] { m_query->facet(filter, Facet::Institution); });
            measure("language", [&] { m_query->facet(filter, Facet::Language); });
            measure("format", [&] { m_query->facet(filter, Facet::Format); });
            measure("read status", [&] { m_query->facet(filter, Facet::ReadStatus); });
            measure("flags", [&] { m_query->flagCounts(filter); });
            measure("year range", [&] { m_query->yearRange(filter); });
            report("whole sidebar", total, 0);
            m_out << '\n';
        }
        m_out.flush();
    }

    void cleanupTestCase()
    {
        m_query.reset();
        m_db.reset();
    }

private:
    static bool reportOnly() { return qEnvironmentVariableIsSet("PAPERTRAIL_PERF_REPORT_ONLY"); }

    void report(const char *name, double ms, double target)
    {
        m_out << "  " << QString::fromUtf8(name).leftJustified(46)
              << QString::number(ms, 'f', 1).rightJustified(8) << " ms";
        if (target > 0 && ms > target)
            m_out << "  OVER";
        m_out << '\n';
    }

    QTemporaryDir m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<LibraryQuery> m_query;
    QTextStream m_out{stdout};
    int m_count = 0;
    qint64 m_total = 0;
};

QTEST_GUILESS_MAIN(QueryPerfTest)
#include "test_query_perf.moc"
