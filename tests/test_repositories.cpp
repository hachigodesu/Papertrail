#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/model/Identifiers.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail::core;

class RepositoriesTest : public QObject {
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
    }

    void cleanup()
    {
        m_files.reset();
        m_tags.reset();
        m_publications.reset();
        m_db.reset();
    }

    // Publications

    void everyFieldRoundTrips()
    {
        Publication p;
        p.type = PublicationType::Dissertation;
        p.title = QStringLiteral("A Study of Thïngs");
        p.subtitle = QStringLiteral("Part One");
        p.abstract = QStringLiteral("Abstract");
        p.year = 2021;
        p.month = 3;
        p.day = 14;
        p.language = QStringLiteral("en");
        p.url = QStringLiteral("https://example.org");
        p.edition = QStringLiteral("2");
        p.series = QStringLiteral("Series");
        p.doi = QStringLiteral("10.1000/abc");
        p.isbn10 = QStringLiteral("0306406152");
        p.isbn13 = QStringLiteral("9780306406157");
        p.issn = QStringLiteral("1234-5678");
        p.eissn = QStringLiteral("8765-4321");
        p.arxivId = QStringLiteral("2101.00001");
        p.pmid = QStringLiteral("12345");
        p.containerTitle = QStringLiteral("Container");
        p.volume = QStringLiteral("12");
        p.issue = QStringLiteral("3");
        p.pages = QStringLiteral("1-10");
        p.publisher = QStringLiteral("Publisher");
        p.place = QStringLiteral("Dublin");
        p.conferenceName = QStringLiteral("Conf");
        p.conferenceLocation = QStringLiteral("Cork");
        p.institution = QStringLiteral("TCD");
        p.department = QStringLiteral("Engineering");
        p.degree = QStringLiteral("PhD");
        p.reportNumber = QStringLiteral("TR-1");
        p.standardNumber = QStringLiteral("ISO 9001");
        p.standardBody = QStringLiteral("ISO");
        p.standardRevision = QStringLiteral("2015");
        p.standardStatus = QStringLiteral("Published");
        p.readStatus = ReadStatus::Reading;
        p.rating = 4;
        p.notes = QStringLiteral("Notes");
        p.extraJson = QStringLiteral("{\"a\":1}");
        p.needsReview = true;
        p.authors = {{QStringLiteral("Knuth"), QStringLiteral("Donald")}, {QStringLiteral("Lamport"), QStringLiteral("Leslie")}};
        p.editors = {{QStringLiteral("Editor"), QStringLiteral("Ed")}};
        p.translators = {{QStringLiteral("Translator"), QString()}};
        p.advisors = {{QStringLiteral("Advisor"), QStringLiteral("Ann")}};

        const qint64 id = m_publications->create(p);
        const Publication stored = *m_publications->get(id);

        QCOMPARE(stored.id, id);
        QVERIFY(!stored.uuid.isEmpty());
        QCOMPARE(stored.type, PublicationType::Dissertation);
        QCOMPARE(stored.title, p.title);
        QCOMPARE(stored.subtitle, p.subtitle);
        QCOMPARE(stored.abstract, p.abstract);
        QCOMPARE(stored.year, p.year);
        QCOMPARE(stored.month, p.month);
        QCOMPARE(stored.day, p.day);
        QCOMPARE(stored.language, p.language);
        QCOMPARE(stored.url, p.url);
        QCOMPARE(stored.edition, p.edition);
        QCOMPARE(stored.series, p.series);
        QCOMPARE(stored.doi, p.doi);
        QCOMPARE(stored.isbn10, p.isbn10);
        QCOMPARE(stored.isbn13, p.isbn13);
        QCOMPARE(stored.issn, p.issn);
        QCOMPARE(stored.eissn, p.eissn);
        QCOMPARE(stored.arxivId, p.arxivId);
        QCOMPARE(stored.pmid, p.pmid);
        QCOMPARE(stored.containerTitle, p.containerTitle);
        QCOMPARE(stored.volume, p.volume);
        QCOMPARE(stored.issue, p.issue);
        QCOMPARE(stored.pages, p.pages);
        QCOMPARE(stored.publisher, p.publisher);
        QCOMPARE(stored.place, p.place);
        QCOMPARE(stored.conferenceName, p.conferenceName);
        QCOMPARE(stored.conferenceLocation, p.conferenceLocation);
        QCOMPARE(stored.institution, p.institution);
        QCOMPARE(stored.department, p.department);
        QCOMPARE(stored.degree, p.degree);
        QCOMPARE(stored.reportNumber, p.reportNumber);
        QCOMPARE(stored.standardNumber, p.standardNumber);
        QCOMPARE(stored.standardBody, p.standardBody);
        QCOMPARE(stored.standardRevision, p.standardRevision);
        QCOMPARE(stored.standardStatus, p.standardStatus);
        QCOMPARE(stored.readStatus, ReadStatus::Reading);
        QCOMPARE(stored.rating, p.rating);
        QCOMPARE(stored.notes, p.notes);
        QCOMPARE(stored.extraJson, p.extraJson);
        QCOMPARE(stored.needsReview, true);
        QCOMPARE(stored.authors, p.authors);
        QCOMPARE(stored.editors, p.editors);
        QCOMPARE(stored.translators.first().family, QStringLiteral("Translator"));
        QCOMPARE(stored.advisors, p.advisors);
        QVERIFY(stored.createdAt > 0);
        QVERIFY(stored.updatedAt > 0);
    }

    void unsetFieldsStayUnset()
    {
        Publication p;
        p.title = QStringLiteral("Minimal");
        p.subtitle = QStringLiteral(""); // An empty value counts as not set.
        const Publication stored = *m_publications->get(m_publications->create(p));
        QVERIFY(stored.subtitle.isNull());
        QVERIFY(stored.doi.isNull());
        QVERIFY(!stored.year.has_value());
        QVERIFY(!stored.parentId.has_value());
    }

    void identifiersAreStoredInOneForm()
    {
        Publication p;
        p.title = QStringLiteral("x");
        p.doi = QStringLiteral("https://doi.org/10.1000/ABC.def");
        p.isbn13 = QStringLiteral("978-0-306-40615-7");
        p.isbn10 = QStringLiteral("0-8044-2957-x");
        p.issn = QStringLiteral("12345678");
        p.arxivId = QStringLiteral("https://arxiv.org/abs/2101.00001");
        const Publication stored = *m_publications->get(m_publications->create(p));
        QCOMPARE(stored.doi, QStringLiteral("10.1000/abc.def"));
        QCOMPARE(stored.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(stored.isbn10, QStringLiteral("080442957X"));
        QCOMPARE(stored.issn, QStringLiteral("1234-5678"));
        QCOMPARE(stored.arxivId, QStringLiteral("2101.00001"));
    }

    void givenUuidAndTimestampsAreKept()
    {
        Publication p;
        p.uuid = QStringLiteral("11111111-2222-3333-4444-555555555555");
        p.createdAt = 1000;
        p.updatedAt = 2000;
        const qint64 id = m_publications->create(p);
        QCOMPARE(m_publications->idForUuid(p.uuid), id);
        const Publication stored = *m_publications->get(id);
        QCOMPARE(stored.createdAt, qint64(1000));
        QCOMPARE(stored.updatedAt, qint64(2000));
    }

    void updateReplacesFieldsAndPeople()
    {
        Publication p;
        p.title = QStringLiteral("Old");
        p.authors = {{QStringLiteral("Keep"), QStringLiteral("A")}, {QStringLiteral("Drop"), QStringLiteral("B")}};
        const qint64 id = m_publications->create(p);
        const qint64 created = p.updatedAt;

        p.title = QStringLiteral("New");
        p.authors = {{QStringLiteral("Keep"), QStringLiteral("A")}, {QStringLiteral("Added"), QStringLiteral("C")}};
        QTest::qSleep(2);
        m_publications->update(p);

        const Publication stored = *m_publications->get(id);
        QCOMPARE(stored.title, QStringLiteral("New"));
        QCOMPARE(stored.authors, p.authors);
        QVERIFY(stored.updatedAt > created);
        QCOMPARE(count("SELECT count(*) FROM person WHERE family_name = 'Drop'"), 0); // Not left orphaned.
    }

    void updatingAMissingPublicationThrows()
    {
        Publication p;
        p.id = 999;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_publications->update(p));
    }

    void authorOrderIsPreserved()
    {
        Publication p;
        p.authors = {{QStringLiteral("Zed"), QString()}, {QStringLiteral("Abel"), QString()}, {QStringLiteral("Mid"), QString()}};
        const Publication stored = *m_publications->get(m_publications->create(p));
        QCOMPARE(stored.authors.at(0).family, QStringLiteral("Zed"));
        QCOMPARE(stored.authors.at(2).family, QStringLiteral("Mid"));
    }

    void removeClearsLinksAndOrphanedPeople()
    {
        const qint64 tag = m_tags->create(QStringLiteral("Tag"));
        Publication shared;
        shared.authors = {{QStringLiteral("Shared"), QString()}};
        const qint64 other = m_publications->create(shared);

        Publication p;
        p.authors = {{QStringLiteral("Shared"), QString()}, {QStringLiteral("Only"), QString()}};
        const qint64 id = m_publications->create(p);
        m_tags->assign(id, tag);

        m_publications->remove(id);
        QVERIFY(!m_publications->get(id).has_value());
        QVERIFY(m_publications->get(other).has_value());
        QCOMPARE(count("SELECT count(*) FROM publication_tag"), 0);
        QCOMPARE(count("SELECT count(*) FROM person WHERE family_name = 'Only'"), 0);
        QCOMPARE(count("SELECT count(*) FROM person WHERE family_name = 'Shared'"), 1);
        QVERIFY(m_tags->get(tag).has_value()); // The tag itself stays.
    }

    void volumesHoldChildArticlesInPageOrder()
    {
        Publication volume;
        volume.type = PublicationType::JournalVolume;
        const qint64 volumeId = m_publications->create(volume);

        Publication later;
        later.parentId = volumeId;
        later.pageStart = 50;
        later.pageEnd = 60;
        const qint64 laterId = m_publications->create(later);

        Publication earlier;
        earlier.parentId = volumeId;
        earlier.pageStart = 1;
        earlier.pageEnd = 10;
        const qint64 earlierId = m_publications->create(earlier);

        QCOMPARE(m_publications->childIds(volumeId), (QList<qint64>{earlierId, laterId}));

        m_publications->remove(volumeId); // Children go with their volume.
        QVERIFY(!m_publications->get(laterId).has_value());
    }

    void invalidParentsAndRangesAreRefused()
    {
        Publication volume;
        const qint64 volumeId = m_publications->create(volume);
        Publication child;
        child.parentId = volumeId;
        const qint64 childId = m_publications->create(child);

        Publication grandchild;
        grandchild.parentId = childId;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_publications->create(grandchild));

        Publication orphan;
        orphan.parentId = 999;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_publications->create(orphan));

        volume.parentId = volumeId; // Its own parent.
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_publications->update(volume));

        Publication other;
        const qint64 otherId = m_publications->create(other);
        volume.parentId = otherId; // It already has children of its own.
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_publications->update(volume));

        Publication backwards;
        backwards.pageStart = 10;
        backwards.pageEnd = 5;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_publications->create(backwards));
    }

    void failedCreateSavesNothing()
    {
        Publication bad;
        bad.month = 13; // Rejected by a CHECK constraint.
        bad.authors = {{QStringLiteral("Ghost"), QString()}};
        QVERIFY_THROWS_EXCEPTION(DbError, m_publications->create(bad));
        QCOMPARE(m_publications->count(), qint64(0));
        QCOMPARE(count("SELECT count(*) FROM person"), 0);
        QVERIFY(!m_db->inTransaction());
    }

    void provenanceKeepsEarlierValues()
    {
        Publication p;
        const qint64 id = m_publications->create(p);
        m_publications->recordProvenance(id, {QStringLiteral("title"), QStringLiteral("crossref"), 0.9,
                                              QStringLiteral("old"), QStringLiteral("new"), 100});
        m_publications->recordProvenance(id, {QStringLiteral("title"), QStringLiteral("user"), std::nullopt,
                                              QStringLiteral("new"), QStringLiteral("newer"), 200});
        m_publications->recordProvenance(id, {QStringLiteral("year"), QStringLiteral("crossref"), 0.9,
                                              QString(), QStringLiteral("2020"), 150});

        const auto title = m_publications->provenance(id, QStringLiteral("title"));
        QCOMPARE(title.size(), 2);
        QCOMPARE(title.first().newValue, QStringLiteral("newer")); // Newest first.
        QCOMPARE(title.last().previousValue, QStringLiteral("old"));
        QCOMPARE(m_publications->provenance(id).size(), 3);
    }

    void sortKeysIgnoreCaseAccentsAndLeadingArticles()
    {
        QCOMPARE(titleSortKey(QStringLiteral("The Art of Computer Programming")), QStringLiteral("art of computer programming"));
        QCOMPARE(titleSortKey(QStringLiteral("A Study")), QStringLiteral("study"));
        QCOMPARE(titleSortKey(QStringLiteral("An Élan")), QStringLiteral("elan"));
        QCOMPARE(titleSortKey(QStringLiteral("Theory")), QStringLiteral("theory")); // Not a leading "The".
        QCOMPARE(nameSortKey(QStringLiteral("Müller")), QStringLiteral("muller"));
    }

    // Tags

    void tagPathsAreCreatedOnceAndReadBack()
    {
        const qint64 fem = m_tags->ensurePath(QStringLiteral("Engineering > Structures > FEM"));
        QCOMPARE(m_tags->path(fem), QStringLiteral("Engineering > Structures > FEM"));
        QCOMPARE(m_tags->all().size(), 3);

        // The same path, in different case, finds what's already there.
        QCOMPARE(m_tags->ensurePath(QStringLiteral("engineering>STRUCTURES>fem")), fem);
        QCOMPARE(m_tags->all().size(), 3);
    }

    void sameTagNameIsAllowedUnderDifferentParents()
    {
        const qint64 a = m_tags->ensurePath(QStringLiteral("A > Methods"));
        const qint64 b = m_tags->ensurePath(QStringLiteral("B > Methods"));
        QVERIFY(a != b);

        const qint64 parent = *m_tags->get(a)->parentId;
        QVERIFY_THROWS_EXCEPTION(DbError, m_tags->create(QStringLiteral("methods"), parent));
        m_tags->create(QStringLiteral("Root"));
        QVERIFY_THROWS_EXCEPTION(DbError, m_tags->create(QStringLiteral("root")));
    }

    void invalidTagNamesAreRefused()
    {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_tags->create(QStringLiteral("   ")));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_tags->create(QStringLiteral("a > b")));
    }

    void movingATagBeneathItselfIsRefused()
    {
        const qint64 child = m_tags->ensurePath(QStringLiteral("Top > Middle > Bottom"));
        const qint64 middle = *m_tags->get(child)->parentId;
        const qint64 top = *m_tags->get(middle)->parentId;

        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_tags->move(top, child));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, m_tags->move(top, top));

        m_tags->move(child, top);
        QCOMPARE(m_tags->path(child), QStringLiteral("Top > Bottom"));
        m_tags->move(middle, std::nullopt);
        QCOMPARE(m_tags->path(middle), QStringLiteral("Middle"));
    }

    void removingATagKeepsPublications()
    {
        const qint64 child = m_tags->ensurePath(QStringLiteral("Parent > Child"));
        const qint64 parent = *m_tags->get(child)->parentId;
        Publication p;
        const qint64 id = m_publications->create(p);
        m_tags->assign(id, child);
        m_tags->assign(id, child); // Assigning twice changes nothing.
        QCOMPARE(m_tags->tagsFor(id).size(), 1);

        m_tags->remove(parent);
        QVERIFY(!m_tags->get(child).has_value()); // Children go with their parent.
        QVERIFY(m_publications->get(id).has_value());
        QCOMPARE(m_tags->tagsFor(id).size(), 0);
    }

    void tagNamesKeepNonAsciiCharacters()
    {
        const qint64 tag = m_tags->create(QStringLiteral("Mécanique"));
        QCOMPARE(m_tags->get(tag)->name, QStringLiteral("Mécanique"));
    }

    // Files

    void filesStoreForwardSlashesAndBuildAbsolutePaths()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("Papers"), QStringLiteral("/data/papers"));
        FileRecord file;
        file.rootId = root;
        file.relativePath = QStringLiteral("2020/paper.pdf");
        file.format = QStringLiteral("pdf");
        m_files->upsert(file);
        QCOMPARE(m_files->get(file.id)->relativePath, QStringLiteral("2020/paper.pdf"));
        QCOMPARE(m_files->absolutePath(file.id), QStringLiteral("/data/papers/2020/paper.pdf"));
    }

    void upsertUpdatesTheSameFile()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        FileRecord file;
        file.rootId = root;
        file.relativePath = QStringLiteral("a.pdf");
        file.format = QStringLiteral("pdf");
        file.size = 100;
        const qint64 first = m_files->upsert(file);

        file.size = 200;
        file.sha256 = QStringLiteral("abc");
        QCOMPARE(m_files->upsert(file), first);
        const FileRecord stored = *m_files->get(first);
        QCOMPARE(stored.size, qint64(200));
        QCOMPARE(m_files->withHash(QStringLiteral("abc")).size(), 1);
    }

    void repointingARootKeepsFilesAndLinks()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("R"), QStringLiteral("/old"));
        FileRecord file;
        file.rootId = root;
        file.relativePath = QStringLiteral("a.pdf");
        file.format = QStringLiteral("pdf");
        m_files->upsert(file);
        Publication p;
        const qint64 id = m_publications->create(p);
        m_files->link(id, file.id, true);

        m_files->repointRoot(root, QStringLiteral("/new"));
        QCOMPARE(m_files->absolutePath(file.id), QStringLiteral("/new/a.pdf"));
        QCOMPARE(m_files->filesFor(id).size(), 1);
    }

    void offlineRootsOnlyAffectHealthyFiles()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        FileRecord healthy;
        healthy.rootId = root;
        healthy.relativePath = QStringLiteral("a.pdf");
        healthy.format = QStringLiteral("pdf");
        m_files->upsert(healthy);
        FileRecord broken = healthy;
        broken.relativePath = QStringLiteral("b.pdf");
        broken.status = FileStatus::Unreadable;
        m_files->upsert(broken);

        m_files->setRootOnline(root, false);
        QCOMPARE(m_files->get(healthy.id)->status, FileStatus::Offline);
        QCOMPARE(m_files->get(broken.id)->status, FileStatus::Unreadable);

        m_files->setRootOnline(root, true);
        QCOMPARE(m_files->get(healthy.id)->status, FileStatus::Ok);
        QCOMPARE(m_files->get(broken.id)->status, FileStatus::Unreadable);
    }

    void publicationsSurviveLosingTheirFiles()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        FileRecord file;
        file.rootId = root;
        file.relativePath = QStringLiteral("a.pdf");
        file.format = QStringLiteral("pdf");
        m_files->upsert(file);
        Publication p;
        const qint64 id = m_publications->create(p);
        m_files->link(id, file.id, true);

        m_files->removeRoot(root);
        QVERIFY(m_publications->get(id).has_value());
        QCOMPARE(m_files->filesFor(id).size(), 0);
    }

    void oneFileIsPrimary()
    {
        const qint64 root = m_files->addRoot(QStringLiteral("R"), QStringLiteral("/r"));
        FileRecord first;
        first.rootId = root;
        first.relativePath = QStringLiteral("a.pdf");
        first.format = QStringLiteral("pdf");
        m_files->upsert(first);
        FileRecord second = first;
        second.relativePath = QStringLiteral("b.pdf");
        m_files->upsert(second);

        Publication p;
        const qint64 id = m_publications->create(p);
        m_files->link(id, first.id, true);
        m_files->link(id, second.id, true);

        QCOMPARE(m_files->filesFor(id).first().id, second.id);
        QCOMPARE(count("SELECT count(*) FROM publication_file WHERE is_primary = 1"), 1);
    }

private:
    int count(const char *sql)
    {
        auto select = m_db->prepare(sql);
        select->step();
        return select->intAt(0);
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<PublicationRepository> m_publications;
    std::unique_ptr<TagRepository> m_tags;
    std::unique_ptr<FileRepository> m_files;
};

QTEST_GUILESS_MAIN(RepositoriesTest)
#include "test_repositories.moc"
