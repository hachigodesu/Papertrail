// Importing details from other reference managers' BibTeX and RIS exports. Every entry here is made up.

#include "citation/Importers.h"
#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"
#include "import/ImportService.h"
#include "metadata/Matching.h"
#include "transfer/ReferenceImport.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::citation;
using namespace papertrail::transfer;

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(PAPERTRAIL_TEST_FIXTURES "/references/") + name);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("missing fixture");
    return file.readAll();
}

const ImportedReference *withKey(const ParsedReferences &parsed, const QString &key)
{
    for (const ImportedReference &entry : parsed.entries) {
        if (entry.key == key)
            return &entry;
    }
    return nullptr;
}

const ImportedReference *withTitle(const ParsedReferences &parsed, const QString &start)
{
    for (const ImportedReference &entry : parsed.entries) {
        if (entry.publication.title.startsWith(start))
            return &entry;
    }
    return nullptr;
}

} // namespace

class ReferenceImportTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QDir(m_dir->path()).mkpath(QStringLiteral("docs/papers"));
        m_db = std::make_unique<Database>(m_dir->filePath(QStringLiteral("library.db")));
        migrateLibrary(*m_db);
        m_root = FileRepository(*m_db).addRoot(QStringLiteral("Docs"), docs());
    }

    void cleanup()
    {
        m_db.reset();
        m_dir.reset();
    }

    // ---- Reading ----
    void readsZoteroBibTeX()
    {
        const ParsedReferences parsed = parseReferences(fixture(QStringLiteral("zotero.bib")));
        QCOMPARE(parsed.format, ReferenceFormat::BibTeX);
        QVERIFY2(parsed.problems.isEmpty(), qPrintable(parsed.problems.join(u'\n')));
        QCOMPARE(parsed.entries.size(), 5);

        const ImportedReference *article = withKey(parsed, QStringLiteral("garner_bending_2019"));
        QVERIFY(article);
        const Publication &p = article->publication;
        QCOMPARE(p.type, PublicationType::JournalArticle);
        QCOMPARE(p.title, QStringLiteral("Bending of Example Beams under Made-Up Loads"));
        QCOMPARE(p.authors.size(), 3);
        QCOMPARE(p.authors.at(2).family, QStringLiteral("Müller"));
        QCOMPARE(p.authors.at(2).given, QStringLiteral("Jörg"));
        QCOMPARE(p.containerTitle, QStringLiteral("Journal of Imaginary Engineering"));
        QCOMPARE(p.year.value_or(0), 2019);
        QCOMPARE(p.month.value_or(0), 5);
        QCOMPARE(p.volume, QStringLiteral("12"));
        QCOMPARE(p.issue, QStringLiteral("3"));
        QCOMPARE(p.pages, QStringLiteral("101-120"));
        QCOMPARE(p.doi, QStringLiteral("10.1000/example.beams"));
        QCOMPARE(p.issn, QStringLiteral("1234-5679"));
        QCOMPARE(p.language, QStringLiteral("en"));
        QCOMPARE(article->tags, (QStringList{QStringLiteral("Beams"), QStringLiteral("Engineering > Structures"), QStringLiteral("Made-up")}));
        QCOMPARE(article->note, QStringLiteral("A made-up note about the beams."));
        QCOMPARE(article->files.size(), 2);
        QCOMPARE(article->files.first(), QStringLiteral("C:\\Users\\someone\\Zotero\\storage\\ABCD1234\\Garner - 2019 - Bending.pdf"));
        QCOMPARE(article->line, 2);

        const ImportedReference *book = withKey(parsed, QStringLiteral("lane_pretend_2020"));
        QCOMPARE(book->publication.type, PublicationType::Book);
        QCOMPARE(book->publication.title, QStringLiteral("Pretend Structures: A Guide"));
        QCOMPARE(book->publication.isbn13, QStringLiteral("9780306406157"));
        QCOMPARE(book->publication.place, QStringLiteral("London"));
        QCOMPARE(book->publication.edition, QStringLiteral("2"));

        // A preprint is only ever a hint, and a note that's only its number isn't kept as one.
        const ImportedReference *preprint = withKey(parsed, QStringLiteral("haddad_sampling_2021"));
        QCOMPARE(preprint->publication.type, PublicationType::Other);
        QVERIFY(preprint->preprintHint);
        QCOMPARE(preprint->publication.arxivId, QStringLiteral("2101.00001"));
        QVERIFY(preprint->note.isEmpty());

        const ImportedReference *thesis = withKey(parsed, QStringLiteral("sample_advanced_2018"));
        QCOMPARE(thesis->publication.type, PublicationType::Dissertation);
        QCOMPARE(thesis->publication.institution, QStringLiteral("University of Example"));
        QCOMPARE(thesis->publication.degree, QStringLiteral("PhD Thesis"));

        const ImportedReference *paper = withKey(parsed, QStringLiteral("li_graphs_2022"));
        QCOMPARE(paper->publication.type, PublicationType::ConferencePaper);
        QCOMPARE(paper->publication.containerTitle, QStringLiteral("Proceedings of the International Conference on Examples"));
        QCOMPARE(paper->publication.editors.size(), 1);
        QCOMPARE(paper->publication.pages, QStringLiteral("5-9"));
    }

    void readsMendeleyBibTeXWithMacrosAndAccents()
    {
        const ParsedReferences parsed = parseReferences(fixture(QStringLiteral("mendeley.bib")));
        QCOMPARE(parsed.entries.size(), 3);
        // The entry with a field that can't be read is reported with its line, and the rest carry on.
        QCOMPARE(parsed.problems.size(), 1);
        QVERIFY2(parsed.problems.first().startsWith(QStringLiteral("Line 28:")), qPrintable(parsed.problems.first()));

        const ImportedReference *article = withKey(parsed, QStringLiteral("Garner2019"));
        QCOMPARE(article->publication.containerTitle, QStringLiteral("Journal of Imaginary Engineering (Series A)"));
        QCOMPARE(article->publication.title, QStringLiteral("Bending of Example Beams under Made-Up Loads"));
        QCOMPARE(article->files, QStringList{QStringLiteral("C:/Users/someone/Documents/Mendeley/Garner2019.pdf")});
        QCOMPARE(article->tags, (QStringList{QStringLiteral("beams"), QStringLiteral("structures")}));

        const ImportedReference *report = withKey(parsed, QStringLiteral("Agency2020"));
        QCOMPARE(report->publication.type, PublicationType::TechnicalReport);
        QCOMPARE(report->publication.authors.size(), 1);
        QCOMPARE(report->publication.authors.first().family, QStringLiteral("Example Agency"));
        QVERIFY(report->publication.authors.first().given.isEmpty());
        QCOMPARE(report->publication.reportNumber, QStringLiteral("EA-2020-04"));

        const ImportedReference *accents = withKey(parsed, QStringLiteral("Odegaard2001"));
        QCOMPARE(accents->publication.title, QStringLiteral("Café Naïve Résumé: ßtraße & Co. — a made-up title"));
        const QList<PersonName> &people = accents->publication.authors;
        QCOMPARE(people.size(), 4);
        QCOMPARE(people.at(0).family, QStringLiteral("Ødegaard"));
        QCOMPARE(people.at(1).family, QStringLiteral("de la Fuente"));
        QCOMPARE(people.at(2).family, QStringLiteral("van Example"));
        QCOMPARE(people.at(3).family, QStringLiteral("Smith Jr."));
        QCOMPARE(people.at(3).given, QStringLiteral("John"));
    }

    void readsJabRefBibTeX()
    {
        const ParsedReferences parsed = parseReferences(fixture(QStringLiteral("jabref.bib")));
        QCOMPARE(parsed.entries.size(), 2);
        const ImportedReference *standard = withKey(parsed, QStringLiteral("ESB1234"));
        QCOMPARE(standard->publication.type, PublicationType::Standard);
        QCOMPARE(standard->publication.standardBody, QStringLiteral("Example Standards Body"));
        QCOMPARE(standard->publication.standardNumber, QStringLiteral("ESB 1234"));
        QCOMPARE(standard->tags, (QStringList{QStringLiteral("Standards"), QStringLiteral("Bolts")}));
        QCOMPARE(standard->files, QStringList{QStringLiteral("papers/esb1234.pdf")});

        const ImportedReference *judgment = withKey(parsed, QStringLiteral("Sample2021"));
        QCOMPARE(judgment->publication.type, PublicationType::CaseLaw);
        QCOMPARE(judgment->publication.court, QStringLiteral("9th Cir."));
        QCOMPARE(judgment->publication.containerTitle, QStringLiteral("F.4th"));
        QCOMPARE(judgment->publication.pages, QStringLiteral("345-360"));
        QCOMPARE(judgment->publication.day.value_or(0), 14);
    }

    void readsRisFromZoteroAndEndNote()
    {
        ParsedReferences parsed = parseReferences(fixture(QStringLiteral("zotero.ris")));
        QCOMPARE(parsed.format, ReferenceFormat::Ris);
        QVERIFY(parsed.problems.isEmpty());
        QCOMPARE(parsed.entries.size(), 3);
        const ImportedReference &article = parsed.entries.at(0);
        QCOMPARE(article.publication.type, PublicationType::JournalArticle);
        QCOMPARE(article.publication.abstract, QStringLiteral("A made-up study of beams that bend, carried onto a second line."));
        QCOMPARE(article.publication.month.value_or(0), 5);
        QCOMPARE(article.publication.day.value_or(0), 3);
        QCOMPARE(article.publication.pages, QStringLiteral("101-120"));
        QCOMPARE(article.tags, (QStringList{QStringLiteral("Beams"), QStringLiteral("Engineering > Structures")}));
        QCOMPARE(article.files, QStringList{QStringLiteral("/home/someone/Zotero/storage/ABCD1234/Garner - 2019.pdf")});
        QCOMPARE(article.note, QStringLiteral("A made-up note about the beams."));

        const ImportedReference &judgment = parsed.entries.at(1);
        QCOMPARE(judgment.publication.type, PublicationType::CaseLaw);
        QCOMPARE(judgment.publication.court, QStringLiteral("Court of Appeal (Civil Division)"));
        QCOMPARE(judgment.publication.caseNumber, QStringLiteral("A2/2020/0567"));
        QCOMPARE(judgment.publication.neutralCitation, QStringLiteral("[2021] EWCA Civ 1234"));
        QVERIFY(judgment.note.isEmpty());

        QVERIFY(parsed.entries.at(2).preprintHint);
        QCOMPARE(parsed.entries.at(2).publication.arxivId, QStringLiteral("2101.00001"));

        parsed = parseReferences(fixture(QStringLiteral("endnote.ris")));
        QCOMPARE(parsed.entries.size(), 3);
        QCOMPARE(parsed.problems.size(), 1); // The last has no ER line.
        QCOMPARE(parsed.entries.at(0).publication.type, PublicationType::Dissertation);
        QCOMPARE(parsed.entries.at(0).publication.institution, QStringLiteral("University of Example"));
        QCOMPARE(parsed.entries.at(0).key, QStringLiteral("12"));
        QCOMPARE(parsed.entries.at(1).publication.reportNumber, QStringLiteral("EA-2020-04"));
        QCOMPARE(parsed.entries.at(1).publication.institution, QStringLiteral("Example Agency"));
        const Publication &chapter = parsed.entries.at(2).publication;
        QCOMPARE(chapter.type, PublicationType::BookChapter);
        QCOMPARE(chapter.editors.size(), 1);
        QCOMPARE(chapter.containerTitle, QStringLiteral("Pretend Structures"));
        QCOMPARE(chapter.isbn13, QStringLiteral("9780306406157"));
    }

    void latexNamesAndFilesAreReadAsTheyMean()
    {
        QCOMPARE(decodeLatex(QStringLiteral("{\\'E}cole \\v{C}esk\\'{y} \\c{c}a \\aa{}ngstr\\\"om")), QStringLiteral("École Český ça ångström"));
        QCOMPARE(decodeLatex(QStringLiteral("50\\% of \\$10 \\& \\_ \\{x\\}")), QStringLiteral("50% of $10 & _ {x}"));
        QCOMPARE(decodeLatex(QStringLiteral("\\textit{Italic} and \\emph{stressed} words")), QStringLiteral("Italic and stressed words"));
        QCOMPARE(decodeLatex(QStringLiteral("$\\alpha$-helix pages 1--2 ``quoted''")), QStringLiteral("α-helix pages 1–2 “quoted”"));
        QCOMPARE(decodeLatex(QStringLiteral("A~B")), QStringLiteral("A B"));

        const QList<PersonName> names = parseBibTeXNames(QStringLiteral("Jean {de la} Example and {Example Agency} and others"));
        QCOMPARE(names.size(), 2);
        QCOMPARE(names.at(0).family, QStringLiteral("Example"));
        QCOMPARE(names.at(0).given, QStringLiteral("Jean de la"));
        QCOMPARE(names.at(1).family, QStringLiteral("Example Agency"));

        QCOMPARE(parseFileField(QStringLiteral(":/home/a/paper.pdf:PDF")), QStringList{QStringLiteral("/home/a/paper.pdf")});
        QCOMPARE(parseFileField(QStringLiteral("plain/path.pdf")), QStringList{QStringLiteral("plain/path.pdf")});
        QCOMPARE(parseFileField(QStringLiteral("Full Text:C:/Users/a/x.pdf:application/pdf")), QStringList{QStringLiteral("C:/Users/a/x.pdf")});

        // Neither a format nor anything to read.
        const ParsedReferences nothing = parseReferences("Just some text with no entries.");
        QCOMPARE(nothing.format, ReferenceFormat::Unknown);
        QVERIFY(nothing.entries.isEmpty());
        QCOMPARE(nothing.problems.size(), 1);
    }

    // ---- Matching and applying ----
    void entriesAreMatchedAndApplied()
    {
        PublicationRepository publications(*m_db);
        FileRepository files(*m_db);
        // One whose file the other manager kept, with a title only guessed from it.
        Publication guessed;
        guessed.title = QStringLiteral("Guessed From The File");
        guessed.needsReview = true;
        const qint64 byFile = publications.create(guessed);
        files.link(byFile, addFile(QStringLiteral("papers/a.pdf")), true);
        // One with a DOI, whose year was set by hand.
        Publication withDoi;
        withDoi.title = QStringLiteral("Second Made-Up Study");
        withDoi.doi = QStringLiteral("10.1000/two");
        withDoi.year = 2010;
        const qint64 byDoi = publications.create(withDoi);
        publications.recordProvenance(byDoi, ProvenanceEntry{QStringLiteral("year"), QStringLiteral("manual"), 1.0, QString(),
                                                              QStringLiteral("2010"), 1});
        Publication titled;
        titled.title = QStringLiteral("A Distinctive Made-Up Title About Beams");
        titled.year = 2020;
        titled.authors = {{QStringLiteral("Lane"), QStringLiteral("Terry")}};
        const qint64 byTitle = publications.create(titled);
        Publication shortTitle;
        shortTitle.title = QStringLiteral("Notes");
        const qint64 unsure = publications.create(shortTitle);
        Publication hidden;
        hidden.title = QStringLiteral("In A Removed Folder");
        hidden.doi = QStringLiteral("10.1000/hidden");
        const qint64 hiddenId = publications.create(hidden);
        m_db->exec(QStringLiteral("INSERT INTO hidden_publication (publication_id) VALUES (%1)").arg(hiddenId).toUtf8().constData());

        ParsedReferences parsed;
        parsed.format = ReferenceFormat::BibTeX;
        ImportedReference first = entry(QStringLiteral("The Real Title From Zotero"));
        first.files = {QDir::toNativeSeparators(docs() + QStringLiteral("/papers/a.pdf"))};
        first.publication.authors = {{QStringLiteral("Garner"), QStringLiteral("Pat")}};
        first.tags = {QStringLiteral("Beams"), QStringLiteral("Engineering > Structures")};
        first.note = QStringLiteral("A made-up note.");
        ImportedReference second = entry(QStringLiteral("Second Made-Up Study: With A Subtitle"));
        second.publication.doi = QStringLiteral("10.1000/TWO");
        second.publication.year = 2011;
        second.publication.volume = QStringLiteral("4");
        ImportedReference third = entry(QStringLiteral("A Distinctive Made-Up Title About Beams"));
        third.publication.year = 2020;
        third.publication.publisher = QStringLiteral("Example Press");
        ImportedReference fourth = entry(QStringLiteral("Notes"));
        fourth.publication.publisher = QStringLiteral("Somebody");
        ImportedReference again = entry(QStringLiteral("Second Made-Up Study"));
        again.publication.doi = QStringLiteral("10.1000/two");
        again.publication.volume = QStringLiteral("99");
        again.publication.issue = QStringLiteral("7");
        ImportedReference keptOut = entry(QStringLiteral("In A Removed Folder"));
        keptOut.publication.doi = QStringLiteral("10.1000/hidden");
        ImportedReference nothing = entry(QStringLiteral("Nothing Like It In The Library"));
        nothing.files = {QStringLiteral("/somewhere/else/entirely/x.pdf")};
        // A DOI that's another work's, as a wrong copy would be, doesn't join them.
        ImportedReference wrongDoi = entry(QStringLiteral("Something Else Altogether Entirely"));
        wrongDoi.publication.doi = QStringLiteral("10.1000/two");
        parsed.entries = {first, second, third, fourth, again, keptOut, nothing, wrongDoi};

        const ReferencePlan plan = planReferenceImport(*m_db, parsed, QStringLiteral("refs"));
        QCOMPARE(plan.entries.at(0).match, ReferenceMatch::ByFile);
        QCOMPARE(plan.entries.at(0).publicationId, byFile);
        QCOMPARE(plan.entries.at(1).match, ReferenceMatch::ByIdentifier);
        QCOMPARE(plan.entries.at(1).publicationId, byDoi);
        QCOMPARE(plan.entries.at(2).match, ReferenceMatch::ByTitle);
        QCOMPARE(plan.entries.at(2).publicationId, byTitle);
        QCOMPARE(plan.entries.at(3).match, ReferenceMatch::ByTitleUnsure);
        QCOMPARE(plan.entries.at(3).publicationId, unsure);
        QCOMPARE(plan.entries.at(4).match, ReferenceMatch::Repeated);
        QCOMPARE(plan.entries.at(5).match, ReferenceMatch::KeptOut);
        QCOMPARE(plan.entries.at(6).match, ReferenceMatch::None);
        QVERIFY(plan.entries.at(6).filesOutsideLibrary);
        QCOMPARE(plan.foldersOutside, QStringList{QStringLiteral("/somewhere/else/entirely")});
        QCOMPARE(plan.entries.at(7).match, ReferenceMatch::None);
        QCOMPARE(plan.count(ReferenceMatch::None), 2);

        // Nothing's changed by planning, and cancelling changes nothing either.
        QCOMPARE(publications.get(byFile)->title, QStringLiteral("Guessed From The File"));
        ReferenceImportReport report = applyReferenceImport(*m_db, plan, {}, [] { return true; });
        QVERIFY(report.cancelled);
        QCOMPARE(publications.get(byFile)->title, QStringLiteral("Guessed From The File"));

        ReferenceImportOptions options;
        options.intoNewCollection = true;
        report = applyReferenceImport(*m_db, plan, options);
        QVERIFY(!report.cancelled);
        QCOMPARE(report.updated, 5);
        QCOMPARE(report.keptOut, 1);
        QCOMPARE(report.notAdded, 2);
        QCOMPARE(report.added, 0);
        QCOMPARE(report.toReview, 1);

        // Its details replace guessed ones, with its tags and note.
        const Publication fromFile = *publications.get(byFile);
        QCOMPARE(fromFile.title, QStringLiteral("The Real Title From Zotero"));
        QCOMPARE(fromFile.authors.first().family, QStringLiteral("Garner"));
        QCOMPARE(fromFile.notes, QStringLiteral("A made-up note."));
        QStringList tagPaths;
        TagRepository tags(*m_db);
        for (const Tag &tag : tags.tagsFor(byFile))
            tagPaths << tags.path(tag.id);
        tagPaths.sort();
        QCOMPARE(tagPaths, (QStringList{QStringLiteral("Beams"), QStringLiteral("Engineering > Structures")}));
        // And can be undone, as a lookup can.
        QCOMPARE(metadata::undoableSource(*m_db, byFile), QStringLiteral("bibtex"));
        QCOMPARE(metadata::providerLabel(QStringLiteral("bibtex")), QStringLiteral("a BibTeX file"));

        // What was set by hand stays, and a repeat only fills gaps.
        const Publication fromDoi = *publications.get(byDoi);
        QCOMPARE(fromDoi.year.value_or(0), 2010);
        QCOMPARE(fromDoi.title, QStringLiteral("Second Made-Up Study: With A Subtitle"));
        QCOMPARE(fromDoi.volume, QStringLiteral("4"));
        QCOMPARE(fromDoi.issue, QStringLiteral("7"));

        QCOMPARE(publications.get(byTitle)->publisher, QStringLiteral("Example Press"));
        QVERIFY(publications.get(unsure)->needsReview);
        QCOMPARE(report.reviewIds, QList<qint64>{unsure});
        QVERIFY(publications.get(hiddenId)->title == QStringLiteral("In A Removed Folder"));

        // Everything it touched is in a collection named after the file.
        CollectionRepository collections(*m_db);
        QCOMPARE(collections.get(report.collectionId)->name, QStringLiteral("refs"));
        QVERIFY(collections.contains(report.collectionId, byFile));
        QVERIFY(!collections.contains(report.collectionId, hiddenId));

        // Undoing takes the import's changes back.
        QVERIFY(metadata::undoLastLookup(*m_db, byFile) > 0);
        QCOMPARE(publications.get(byFile)->title, QStringLiteral("Guessed From The File"));

        // Importing the same again changes nothing more.
        const ReferenceImportReport repeat = applyReferenceImport(*m_db, planReferenceImport(*m_db, parsed, QStringLiteral("refs")), options);
        QCOMPARE(collections.get(repeat.collectionId)->name, QStringLiteral("refs (2)"));
    }

    void entriesThatMatchNothingAreAddedOnlyWhenChosen()
    {
        ParsedReferences parsed;
        parsed.format = ReferenceFormat::Ris;
        ImportedReference preprint = entry(QStringLiteral("Sampling Made Simple"));
        preprint.publication.type = PublicationType::Preprint;
        preprint.preprintHint = true;
        preprint.note = QStringLiteral("A made-up note.");
        preprint.tags = {QStringLiteral("Sampling")};
        parsed.entries = {preprint};
        const ReferencePlan plan = planReferenceImport(*m_db, parsed, QStringLiteral("refs"));
        QCOMPARE(applyReferenceImport(*m_db, plan, {}).notAdded, 1);
        QCOMPARE(LibraryQuery(*m_db).count({}), qint64(0));

        ReferenceImportOptions options;
        options.addUnmatched = true;
        QCOMPARE(applyReferenceImport(*m_db, plan, options).added, 1);
        const QList<qint64> ids = LibraryQuery(*m_db).ids({}, {});
        QCOMPARE(ids.size(), 1);
        const Publication added = *PublicationRepository(*m_db).get(ids.first());
        QCOMPARE(added.title, QStringLiteral("Sampling Made Simple"));
        QVERIFY(added.type != PublicationType::Preprint); // Only the user makes something a preprint.
        QVERIFY(added.extraJson.contains(QStringLiteral("preprintHint")));
        QCOMPARE(added.notes, QStringLiteral("A made-up note."));
        QCOMPARE(TagRepository(*m_db).tagsFor(added.id).size(), 1);
    }

    void aFileFoundLaterJoinsItsEntry()
    {
        // An entry added without a file, which has the DOI and title of a file scanned afterwards.
        ParsedReferences parsed;
        parsed.format = ReferenceFormat::BibTeX;
        ImportedReference waiting = entry(QStringLiteral("Fatigue of Steel Beams"));
        waiting.publication.type = PublicationType::JournalArticle;
        waiting.publication.doi = QStringLiteral("10.1234/example.5678");
        waiting.publication.containerTitle = QStringLiteral("Journal of Made-Up Results");
        parsed.entries = {waiting};
        ReferenceImportOptions options;
        options.addUnmatched = true;
        applyReferenceImport(*m_db, planReferenceImport(*m_db, parsed, QStringLiteral("refs")), options);
        QCOMPARE(LibraryQuery(*m_db).count({}), qint64(1));

        QVERIFY(QFile::copy(QStringLiteral(PAPERTRAIL_TEST_SAMPLES "/article.pdf"), docs() + QStringLiteral("/article.pdf")));
        import::ImportOptions importOptions;
        importOptions.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        importOptions.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        import::ImportService service(*m_db, importOptions);
        service.scan(m_root);
        service.processQueue();

        const QList<qint64> ids = LibraryQuery(*m_db).ids({}, {});
        QCOMPARE(ids.size(), 1);
        const Publication joined = *PublicationRepository(*m_db).get(ids.first());
        QCOMPARE(FileRepository(*m_db).filesFor(joined.id).size(), 1);
        QCOMPARE(joined.containerTitle, QStringLiteral("Journal of Made-Up Results")); // The entry's details stay.
        // The file was only read.
        QFile copy(docs() + QStringLiteral("/article.pdf"));
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QFile original(QStringLiteral(PAPERTRAIL_TEST_SAMPLES "/article.pdf"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), original.readAll());
    }

private:
    QString docs() const { return QDir::cleanPath(m_dir->filePath(QStringLiteral("docs"))); }

    qint64 addFile(const QString &relative)
    {
        FileRecord file;
        file.rootId = m_root;
        file.relativePath = relative;
        file.size = 10;
        file.modifiedAt = 1;
        file.format = QFileInfo(relative).suffix();
        FileRepository(*m_db).upsert(file);
        return file.id;
    }

    static ImportedReference entry(const QString &title)
    {
        ImportedReference made;
        made.publication.title = title;
        return made;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Database> m_db;
    qint64 m_root = 0;
};

QTEST_GUILESS_MAIN(ReferenceImportTest)
#include "test_reference_import.moc"
