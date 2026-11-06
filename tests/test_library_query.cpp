#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"

#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail::core;

class LibraryQueryTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        m_publications = std::make_unique<PublicationRepository>(*m_db);
        m_tags = std::make_unique<TagRepository>(*m_db);
        m_files = std::make_unique<FileRepository>(*m_db);
        m_query = std::make_unique<LibraryQuery>(*m_db);
        buildLibrary();
    }

    void cleanup()
    {
        m_query.reset();
        m_files.reset();
        m_tags.reset();
        m_publications.reset();
        m_db.reset();
    }

    void noFilterShowsEverything()
    {
        QCOMPARE(m_query->count({}), qint64(6));
        QCOMPARE(titles({}).size(), 6);
    }

    void tabsRestrictToTheirTypes()
    {
        PublicationFilter filter;
        filter.tabTypes = typesForTab(LibraryTab::ThesesAndDissertations);
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Ceramic Thesis")}));

        filter.tabTypes = typesForTab(LibraryTab::Textbooks);
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("The Art of Bridges")}));

        filter.tabTypes = typesForTab(LibraryTab::JournalVolumes);
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Structures Volume 1")}));

        QCOMPARE(typesForTab(LibraryTab::All).size(), 0);
    }

    void everyTypeIsOnExactlyOneTab()
    {
        for (const PublicationType type : allPublicationTypes()) {
            int tabs = 0;
            for (const LibraryTab tab : {LibraryTab::JournalVolumes, LibraryTab::JournalArticles, LibraryTab::Preprints,
                                         LibraryTab::ConferencePapers, LibraryTab::Textbooks,
                                         LibraryTab::ThesesAndDissertations, LibraryTab::TechnicalReports,
                                         LibraryTab::Standards, LibraryTab::Other}) {
                if (typesForTab(tab).contains(type))
                    ++tabs;
            }
            QCOMPARE(tabs, 1);
        }
    }

    void valuesInOneFilterAreOrAndFiltersAreAnd()
    {
        PublicationFilter filter;
        filter.journals = {QStringLiteral("Journal A"), QStringLiteral("Journal B")};
        QCOMPARE(m_query->count(filter), qint64(3));

        filter.yearMin = 2015;
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Beam Fatigue"), QStringLiteral("Neural Bridges")}));

        filter.readStatuses = {ReadStatus::Read};
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Beam Fatigue")}));
    }

    void yearBoundsAreInclusiveAndSkipUnknownYears()
    {
        PublicationFilter filter;
        filter.yearMin = 2010;
        filter.yearMax = 2010;
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Old Beams")}));

        filter.yearMax.reset();
        filter.yearMin = 0;
        QCOMPARE(m_query->count(filter), qint64(5)); // One has no year.
    }

    void aTagAlsoMatchesEverythingBeneathIt()
    {
        PublicationFilter filter;
        filter.tagIds = {m_structures};
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Beam Fatigue"), QStringLiteral("Old Beams")}));

        filter.tagIds = {m_engineering};
        QCOMPARE(m_query->count(filter), qint64(3)); // Includes "The Art of Bridges", tagged directly.

        filter.tagIds = {m_fem};
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Beam Fatigue")}));

        filter.tagIds = {m_fem, m_computing}; // OR within tags.
        QCOMPARE(m_query->count(filter), qint64(2));
    }

    void authorFilterOnlyMatchesAuthors()
    {
        PublicationFilter filter;
        filter.authorIds = {personId(QStringLiteral("Knuth"))};
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Beam Fatigue"), QStringLiteral("Neural Bridges")}));

        filter.authorIds = {personId(QStringLiteral("EditorOnly"))};
        QCOMPARE(m_query->count(filter), qint64(0));
    }

    void formatFilterIncludesArticlesThroughTheirVolume()
    {
        PublicationFilter filter;
        filter.formats = {QStringLiteral("djvu")};
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Structures Volume 1"), QStringLiteral("Old Beams")}));

        filter.formats = {QStringLiteral("pdf"), QStringLiteral("epub")};
        QCOMPARE(m_query->count(filter), qint64(2));
    }

    void flagsSelectProblemRecords()
    {
        PublicationFilter filter;
        filter.needsReview = true;
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Ceramic Thesis")}));

        filter = {};
        filter.noFile = true;
        // Ceramic Thesis and Neural Bridges have none. Old Beams uses its volume's file.
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Ceramic Thesis"), QStringLiteral("Neural Bridges")}));

        filter = {};
        filter.noText = true;
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Structures Volume 1"), QStringLiteral("Old Beams")}));

        filter = {};
        filter.missingOrOffline = true;
        QCOMPARE(titleSet(filter), (QSet<QString>{QStringLiteral("Beam Fatigue")}));

        const FlagCounts counts = m_query->flagCounts({});
        QCOMPARE(counts.needsReview, qint64(1));
        QCOMPARE(counts.noFile, qint64(2));
        QCOMPARE(counts.noText, qint64(2));
        QCOMPARE(counts.missingOrOffline, qint64(1));
    }

    void searchFindsTextInEachField()
    {
        QCOMPARE(titlesFor(QStringLiteral("fatigue")), (QSet<QString>{QStringLiteral("Beam Fatigue")}));
        QCOMPARE(titlesFor(QStringLiteral("fatig")), (QSet<QString>{QStringLiteral("Beam Fatigue")})); // Prefix.
        QCOMPARE(titlesFor(QStringLiteral("cyclic")), (QSet<QString>{QStringLiteral("Beam Fatigue")})); // Abstract.
        QCOMPARE(titlesFor(QStringLiteral("author:lamport")), (QSet<QString>{QStringLiteral("Neural Bridges")}));
        QCOMPARE(titlesFor(QStringLiteral("muller")), (QSet<QString>{QStringLiteral("Old Beams")})); // Ignores accents.
        QCOMPARE(titlesFor(QStringLiteral("journal:\"Journal B\"")), (QSet<QString>{QStringLiteral("Neural Bridges")}));
        QCOMPARE(titlesFor(QStringLiteral("10.1000")).size(), 1);
        QCOMPARE(titlesFor(QStringLiteral("nothingmatchesthis")).size(), 0);
    }

    void searchWordsMustAllMatch()
    {
        QCOMPARE(titlesFor(QStringLiteral("neural bridges")), (QSet<QString>{QStringLiteral("Neural Bridges")}));
        QCOMPARE(titlesFor(QStringLiteral("neural fatigue")).size(), 0);
    }

    void searchCanUseTagsYearsTypesAndIdentifiers()
    {
        QCOMPARE(titlesFor(QStringLiteral("tag:struct")), (QSet<QString>{QStringLiteral("Beam Fatigue"), QStringLiteral("Old Beams")}));
        QCOMPARE(titlesFor(QStringLiteral("tag:engineering year:>2015")), (QSet<QString>{QStringLiteral("Beam Fatigue")}));
        QCOMPARE(titlesFor(QStringLiteral("type:thesis")), (QSet<QString>{QStringLiteral("Ceramic Thesis")}));
        QCOMPARE(titlesFor(QStringLiteral("doi:10.1000/BEAM")), (QSet<QString>{QStringLiteral("Beam Fatigue")}));
        QCOMPARE(titlesFor(QStringLiteral("doi:10.1000/")), (QSet<QString>{QStringLiteral("Beam Fatigue")})); // Prefix.
        QCOMPARE(titlesFor(QStringLiteral("isbn:978-0-306-40615-7")), (QSet<QString>{QStringLiteral("The Art of Bridges")}));
    }

    void tagPrefixTreatsWildcardsLiterally()
    {
        QCOMPARE(titlesFor(QStringLiteral("tag:%")).size(), 0);
        QCOMPARE(titlesFor(QStringLiteral("tag:_")).size(), 0);
    }

    void hostileSearchTextIsSafe_data()
    {
        QTest::addColumn<QString>("text");
        for (const char *text : {"'; DROP TABLE publication; --", "\"", "\"\"\"", "*", "NEAR(", "a OR", "(((", ":",
                                 "title:", "title:\"", "tag:'", "doi:'--", "year:>", "AND", "NOT", "^", "{a}", "a:b:c",
                                 "\\", "%", "🧪", "tag:\\", "type:'", "isbn:", "arxiv:"}) {
            QTest::newRow(text) << QString::fromUtf8(text);
        }
    }

    void hostileSearchTextIsSafe()
    {
        QFETCH(QString, text);
        PublicationFilter filter;
        filter.searchText = text;
        m_query->count(filter);
        m_query->page(filter, {SortField::Relevance, true}, 0, 10);
        m_query->facet(filter, Facet::Author);
        m_query->tagCounts(filter);
        QCOMPARE(m_publications->count(), qint64(6)); // Still intact.
    }

    void sortingOrdersEachField()
    {
        // The leading "The" is ignored, so "The Art of Bridges" comes first.
        QCOMPARE(titles({}, {SortField::Title, true}).first(), QStringLiteral("The Art of Bridges"));
        QCOMPARE(titles({}, {SortField::Title, false}).first(), QStringLiteral("Structures Volume 1"));

        // Newest first, and the record with no year goes last in both directions.
        QCOMPARE(titles({}, {SortField::Year, false}).first(), QStringLiteral("Neural Bridges"));
        QCOMPARE(titles({}, {SortField::Year, false}).last(), QStringLiteral("Structures Volume 1"));
        QCOMPARE(titles({}, {SortField::Year, true}).first(), QStringLiteral("Ceramic Thesis"));
        QCOMPARE(titles({}, {SortField::Year, true}).last(), QStringLiteral("Structures Volume 1"));

        // With no authors, a record sorts by its editor.
        const QStringList byAuthor = titles({}, {SortField::FirstAuthor, true});
        QCOMPARE(byAuthor.first(), QStringLiteral("Ceramic Thesis")); // Curie
        QCOMPARE(byAuthor.at(1), QStringLiteral("Structures Volume 1")); // EditorOnly

        // With nobody at all, it goes last when ascending.
        Publication anonymous;
        anonymous.title = QStringLiteral("Anonymous");
        m_publications->create(anonymous);
        QCOMPARE(titles({}, {SortField::FirstAuthor, true}).last(), QStringLiteral("Anonymous"));
    }

    void relevancePutsTitleMatchesFirst()
    {
        PublicationFilter filter;
        filter.searchText = QStringLiteral("bridges");
        const QStringList results = titles(filter, {SortField::Relevance, true});
        QCOMPARE(results.size(), 2);

        // Without any search text, relevance falls back to title order.
        QCOMPARE(titles({}, {SortField::Relevance, true}).first(), QStringLiteral("The Art of Bridges"));
    }

    void pagesDontRepeatOrSkipRows()
    {
        QStringList all;
        for (qint64 offset = 0;; offset += 2) {
            const auto rows = m_query->page({}, {SortField::Title, true}, offset, 2);
            if (rows.isEmpty())
                break;
            for (const PublicationSummary &row : rows)
                all << row.title;
        }
        QCOMPARE(all, titles({}, {SortField::Title, true}));
        QCOMPARE(m_query->ids({}, {SortField::Title, true}).size(), 6);
    }

    void summariesHoldWhatAListNeeds()
    {
        PublicationFilter filter;
        filter.searchText = QStringLiteral("neural");
        const auto rows = m_query->page(filter, {}, 0, 10);
        QCOMPARE(rows.size(), 1);
        const PublicationSummary &row = rows.first();
        QCOMPARE(row.title, QStringLiteral("Neural Bridges"));
        QCOMPARE(row.authors, QStringLiteral("Knuth, Donald; Lamport, Leslie"));
        QCOMPARE(row.year, 2022);
        QCOMPARE(row.containerTitle, QStringLiteral("Journal B"));
        QCOMPARE(row.type, PublicationType::JournalArticle);
    }

    void longAuthorListsAreShortenedForDisplay()
    {
        Publication p;
        p.title = QStringLiteral("Many Authors");
        p.authors = {{QStringLiteral("A"), QString()}, {QStringLiteral("B"), QString()},
                     {QStringLiteral("C"), QString()}, {QStringLiteral("D"), QString()}};
        m_publications->create(p);
        PublicationFilter filter;
        filter.searchText = QStringLiteral("title:many");
        QCOMPARE(m_query->page(filter, {}, 0, 1).first().authors, QStringLiteral("A; B; C et al."));
    }

    void facetsCountAndSortByMostCommon()
    {
        const QList<FacetValue> journals = m_query->facet({}, Facet::Journal);
        QCOMPARE(journals.size(), 2);
        QCOMPARE(journals.first().key, QStringLiteral("Journal A"));
        QCOMPARE(journals.first().count, qint64(2));

        const QList<FacetValue> types = m_query->facet({}, Facet::Type);
        qint64 total = 0;
        for (const FacetValue &value : types)
            total += value.count;
        QCOMPARE(total, qint64(6));
    }

    void facetsIgnoreTheirOwnSelectionOnly()
    {
        PublicationFilter filter;
        filter.journals = {QStringLiteral("Journal A")};

        // Choosing Journal A still shows Journal B with its count, so it can be added.
        QCOMPARE(m_query->facet(filter, Facet::Journal).size(), 2);

        // Other facets do follow the selection.
        const QList<FacetValue> authors = m_query->facet(filter, Facet::Author);
        QSet<QString> labels;
        for (const FacetValue &value : authors)
            labels << value.label;
        QVERIFY(labels.contains(QStringLiteral("Knuth, Donald")));
        QVERIFY(!labels.contains(QStringLiteral("Lamport, Leslie")));
    }

    void authorFacetKeysWorkAsFilterValues()
    {
        const QList<FacetValue> authors = m_query->facet({}, Facet::Author);
        QCOMPARE(authors.first().label, QStringLiteral("Knuth, Donald"));
        QCOMPARE(authors.first().count, qint64(2));

        PublicationFilter filter;
        filter.authorIds = {authors.first().key.toLongLong()};
        QCOMPARE(m_query->count(filter), qint64(2));
    }

    void formatFacetCountsPublicationsThroughVolumes()
    {
        QHash<QString, qint64> counts;
        for (const FacetValue &value : m_query->facet({}, Facet::Format))
            counts.insert(value.key, value.count);
        QCOMPARE(counts.value(QStringLiteral("djvu")), qint64(2)); // The volume and its article.
        QCOMPARE(counts.value(QStringLiteral("pdf")), qint64(1));
    }

    void facetLimitApplies()
    {
        QCOMPARE(m_query->facet({}, Facet::Type, 2).size(), 2);
    }

    void tagCountsRollUpAndCountEachPublicationOnce()
    {
        // Give Beam Fatigue a second tag under Engineering: it must still count once there.
        m_tags->assign(m_beamFatigue, m_structures);

        QHash<qint64, qint64> counts;
        for (const TagCount &tag : m_query->tagCounts({}))
            counts.insert(tag.tagId, tag.count);
        QCOMPARE(counts.value(m_fem), qint64(1));
        QCOMPARE(counts.value(m_structures), qint64(2));
        QCOMPARE(counts.value(m_engineering), qint64(3));
        QCOMPARE(counts.value(m_computing), qint64(1));
    }

    void tagCountsFollowOtherFiltersButNotTags()
    {
        PublicationFilter filter;
        filter.tagIds = {m_computing};
        filter.yearMin = 2019;

        QHash<qint64, qint64> counts;
        for (const TagCount &tag : m_query->tagCounts(filter))
            counts.insert(tag.tagId, tag.count);
        QCOMPARE(counts.value(m_computing), qint64(1));
        QCOMPARE(counts.value(m_fem), qint64(1)); // Still shown, despite Computing being selected.
        QCOMPARE(counts.value(m_engineering), qint64(1)); // Old Beams and Art of Bridges fall out by year.
    }

    void yearRangeFollowsFiltersButNotItself()
    {
        QCOMPARE(m_query->yearRange({}), std::make_pair(2005, 2022));

        PublicationFilter filter;
        filter.journals = {QStringLiteral("Journal B")};
        QCOMPARE(m_query->yearRange(filter), std::make_pair(2022, 2022));

        filter = {};
        filter.yearMin = 2020;
        QCOMPARE(m_query->yearRange(filter), std::make_pair(2005, 2022));

        filter = {};
        filter.searchText = QStringLiteral("nothingmatchesthis");
        QVERIFY(!m_query->yearRange(filter).has_value());
    }

    // The search index is kept up to date by database triggers, so test those directly.

    void searchFollowsEdits()
    {
        Publication p = *m_publications->get(m_beamFatigue);
        p.title = QStringLiteral("Renamed Paper");
        p.authors = {{QStringLiteral("Hopper"), QStringLiteral("Grace")}};
        m_publications->update(p);

        QCOMPARE(titlesFor(QStringLiteral("renamed")), (QSet<QString>{QStringLiteral("Renamed Paper")}));
        QCOMPARE(titlesFor(QStringLiteral("hopper")), (QSet<QString>{QStringLiteral("Renamed Paper")}));
        QCOMPARE(titlesFor(QStringLiteral("title:fatigue")).size(), 0);
    }

    void searchFollowsTagChanges()
    {
        QCOMPARE(titlesFor(QStringLiteral("computing")).size(), 1);
        m_tags->rename(m_computing, QStringLiteral("Informatics"));
        QCOMPARE(titlesFor(QStringLiteral("computing")).size(), 0);
        QCOMPARE(titlesFor(QStringLiteral("informatics")).size(), 1);

        m_tags->unassign(neuralBridges(), m_computing);
        QCOMPARE(titlesFor(QStringLiteral("informatics")).size(), 0);
    }

    void repeatedSearchSeesChangesMadeInBetween()
    {
        // The same search twice, with the library changing in between, must not reuse old matches.
        QCOMPARE(titlesFor(QStringLiteral("zebra")).size(), 0);
        Publication p;
        p.title = QStringLiteral("Zebra Stripes");
        const qint64 id = m_publications->create(p);
        QCOMPARE(titlesFor(QStringLiteral("zebra")).size(), 1);
        PublicationFilter filter;
        filter.searchText = QStringLiteral("zebra");
        QCOMPARE(m_query->count(filter), qint64(1));
        m_publications->remove(id);
        QCOMPARE(m_query->count(filter), qint64(0));
    }

    void repeatedSearchSeesChangesFromAnotherConnection()
    {
        PublicationFilter filter;
        filter.searchText = QStringLiteral("zebra");
        QCOMPARE(m_query->count(filter), qint64(0));

        Database other(m_dir->filePath(QStringLiteral("library.db")));
        PublicationRepository otherPublications(other);
        Publication p;
        p.title = QStringLiteral("Zebra Stripes");
        otherPublications.create(p);

        QCOMPARE(m_query->count(filter), qint64(1));
    }

    void searchWorksWithManyMatchesCombinedWithFilters()
    {
        // Past the point where the query walks the list in order instead of sorting matches.
        for (int i = 0; i < 2100; ++i) {
            Publication p;
            p.title = QStringLiteral("Common %1").arg(i);
            p.year = 2000 + i % 2;
            m_publications->create(p);
        }
        PublicationFilter filter;
        filter.searchText = QStringLiteral("common");
        QCOMPARE(m_query->count(filter), qint64(2100));
        filter.yearMin = 2001;
        QCOMPARE(m_query->count(filter), qint64(1050));
        const auto rows = m_query->page(filter, {SortField::Title, true}, 0, 5);
        QCOMPARE(rows.size(), 5);
        for (const PublicationSummary &row : rows)
            QCOMPARE(row.year, 2001);
    }

    void removedPublicationsLeaveTheSearchIndex()
    {
        QCOMPARE(titlesFor(QStringLiteral("neural")).size(), 1);
        m_publications->remove(neuralBridges());
        QCOMPARE(titlesFor(QStringLiteral("neural")).size(), 0);
        auto select = m_db->prepare("SELECT count(*) FROM publication_fts");
        select->step();
        QCOMPARE(select->intAt(0), 5);
    }

private:
    void buildLibrary()
    {
        m_fem = m_tags->ensurePath(QStringLiteral("Engineering > Structures > FEM"));
        m_structures = *m_tags->get(m_fem)->parentId;
        m_engineering = *m_tags->get(m_structures)->parentId;
        m_computing = m_tags->create(QStringLiteral("Computing"));

        const qint64 root = m_files->addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        int fileNumber = 0;
        const auto addFile = [&](qint64 id, const char *format, FileStatus status = FileStatus::Ok, bool text = true) {
            FileRecord file;
            file.rootId = root;
            file.relativePath = QStringLiteral("f%1").arg(fileNumber++);
            file.format = QString::fromLatin1(format);
            file.status = status;
            file.hasTextLayer = text;
            m_files->upsert(file);
            m_files->link(id, file.id, true);
        };

        Publication beam;
        beam.type = PublicationType::JournalArticle;
        beam.title = QStringLiteral("Beam Fatigue");
        beam.abstract = QStringLiteral("Cyclic loading of steel beams");
        beam.year = 2020;
        beam.containerTitle = QStringLiteral("Journal A");
        beam.readStatus = ReadStatus::Read;
        beam.doi = QStringLiteral("10.1000/beam");
        beam.authors = {{QStringLiteral("Knuth"), QStringLiteral("Donald")}};
        m_beamFatigue = m_publications->create(beam);
        m_tags->assign(m_beamFatigue, m_fem);
        addFile(m_beamFatigue, "pdf", FileStatus::Missing);

        Publication neural;
        neural.type = PublicationType::JournalArticle;
        neural.title = QStringLiteral("Neural Bridges");
        neural.year = 2022;
        neural.containerTitle = QStringLiteral("Journal B");
        neural.authors = {{QStringLiteral("Knuth"), QStringLiteral("Donald")}, {QStringLiteral("Lamport"), QStringLiteral("Leslie")}};
        const qint64 neuralId = m_publications->create(neural);
        m_tags->assign(neuralId, m_computing);

        Publication volume;
        volume.type = PublicationType::JournalVolume;
        volume.title = QStringLiteral("Structures Volume 1");
        volume.editors = {{QStringLiteral("EditorOnly"), QString()}};
        const qint64 volumeId = m_publications->create(volume);
        addFile(volumeId, "djvu", FileStatus::Ok, false);

        Publication old;
        old.type = PublicationType::JournalArticle;
        old.title = QStringLiteral("Old Beams");
        old.year = 2010;
        old.containerTitle = QStringLiteral("Journal A");
        old.parentId = volumeId;
        old.authors = {{QStringLiteral("Müller"), QStringLiteral("Anna")}};
        const qint64 oldId = m_publications->create(old);
        m_tags->assign(oldId, m_structures);

        Publication book;
        book.type = PublicationType::Textbook;
        book.title = QStringLiteral("The Art of Bridges");
        book.year = 2015;
        book.publisher = QStringLiteral("Publisher X");
        book.isbn13 = QStringLiteral("9780306406157");
        book.authors = {{QStringLiteral("Roebling"), QStringLiteral("Emily")}};
        const qint64 bookId = m_publications->create(book);
        m_tags->assign(bookId, m_engineering);
        addFile(bookId, "epub");

        Publication thesis;
        thesis.type = PublicationType::Thesis;
        thesis.title = QStringLiteral("Ceramic Thesis");
        thesis.year = 2005;
        thesis.institution = QStringLiteral("TCD");
        thesis.needsReview = true;
        thesis.authors = {{QStringLiteral("Curie"), QStringLiteral("Marie")}};
        m_publications->create(thesis);
    }

    qint64 personId(const QString &family)
    {
        auto select = m_db->prepare("SELECT id FROM person WHERE family_name = ?");
        select->bind(1, family);
        return select->step() ? select->int64At(0) : -1;
    }

    qint64 neuralBridges()
    {
        PublicationFilter filter;
        filter.searchText = QStringLiteral("title:\"Neural Bridges\"");
        return m_query->ids(filter, {}).first();
    }

    QStringList titles(const PublicationFilter &filter, const PublicationSort &sort = {})
    {
        QStringList result;
        for (const PublicationSummary &row : m_query->page(filter, sort, 0, 100))
            result << row.title;
        return result;
    }

    QSet<QString> titleSet(const PublicationFilter &filter)
    {
        const QStringList list = titles(filter);
        return QSet<QString>(list.begin(), list.end());
    }

    QSet<QString> titlesFor(const QString &search)
    {
        PublicationFilter filter;
        filter.searchText = search;
        return titleSet(filter);
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<PublicationRepository> m_publications;
    std::unique_ptr<TagRepository> m_tags;
    std::unique_ptr<FileRepository> m_files;
    std::unique_ptr<LibraryQuery> m_query;

    qint64 m_fem = 0;
    qint64 m_structures = 0;
    qint64 m_engineering = 0;
    qint64 m_computing = 0;
    qint64 m_beamFatigue = 0;
};

QTEST_GUILESS_MAIN(LibraryQueryTest)
#include "test_library_query.moc"
