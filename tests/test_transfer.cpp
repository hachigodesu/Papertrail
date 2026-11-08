#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/FileRepository.h"
#include "core/repo/RemovedContent.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/VolumeArticles.h"
#include "core/settings/SettingsStore.h"
#include "import/ImportService.h"
#include "transfer/LibraryTransfer.h"
#include "transfer/Zip.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::transfer;

namespace {

QString sample(const QString &name)
{
    return QStringLiteral(PAPERTRAIL_TEST_SAMPLES) + u'/' + name;
}

QByteArray hashOfFolder(const QString &folder)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    QStringList paths;
    while (it.hasNext())
        paths << it.next();
    paths.sort();
    for (const QString &path : std::as_const(paths)) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            hash.addData(path.toUtf8());
            hash.addData(file.readAll());
        }
    }
    return hash.result();
}

// One library: its database, a folder of documents, and the real import with its worker.
struct Library {
    QTemporaryDir dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<import::ImportService> service;
    qint64 root = 0;

    Library()
    {
        QDir(dir.path()).mkpath(QStringLiteral("docs"));
        QDir(dir.path()).mkpath(QStringLiteral("cache"));
        db = std::make_unique<Database>(dir.filePath(QStringLiteral("library.db")));
        migrateLibrary(*db);
        import::ImportOptions options;
        options.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        options.cacheDirectory = dir.filePath(QStringLiteral("cache"));
        service = std::make_unique<import::ImportService>(*db, options);
        root = FileRepository(*db).addRoot(QStringLiteral("Docs"), docs());
    }

    QString docs() const { return dir.filePath(QStringLiteral("docs")); }

    void put(const QString &sampleName, const QString &as)
    {
        const QString path = QDir(docs()).filePath(as);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QVERIFY2(QFile::copy(sample(sampleName), path), qPrintable(as));
    }

    void scan()
    {
        service->scan(root);
        service->processQueue();
    }

    qint64 count()
    {
        auto select = db->prepare("SELECT count(*) FROM publication WHERE id NOT IN (SELECT publication_id FROM hidden_publication)");
        select->step();
        return select->int64At(0);
    }

    qint64 scalar(const char *sql)
    {
        auto select = db->prepare(sql);
        return select->step() ? select->int64At(0) : -1;
    }

    QString text(const QByteArray &sql)
    {
        auto select = db->prepare(sql);
        return select->step() ? select->textAt(0) : QString();
    }

    std::optional<qint64> idOfFile(const QString &relativePath)
    {
        auto select = db->prepare("SELECT pf.publication_id FROM file f JOIN publication_file pf ON pf.file_id = f.id "
                                  "WHERE f.relative_path = ?");
        select->bind(1, relativePath);
        return select->step() ? std::optional<qint64>(select->int64At(0)) : std::nullopt;
    }

    QSet<QString> uuids()
    {
        QSet<QString> set;
        auto select = db->prepare("SELECT uuid FROM publication WHERE id NOT IN (SELECT publication_id FROM hidden_publication)");
        while (select->step())
            set.insert(select->textAt(0));
        return set;
    }
};

const QStringList kSamples = {QStringLiteral("article.pdf"), QStringLiteral("book.epub"), QStringLiteral("thesis.pdf"),
                              QStringLiteral("report.pdf"), QStringLiteral("reader_doc.pdf"), QStringLiteral("conference.pdf")};

} // namespace

class TransferTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_from = std::make_unique<Library>();
        for (const QString &name : kSamples)
            m_from->put(name, QStringLiteral("Papers/") + name);
        m_from->scan();
        QCOMPARE(m_from->count(), qint64(kSamples.size()));
        decorate(*m_from);
    }

    void cleanup()
    {
        m_from.reset();
    }

    // ---- The goal: everything comes back, with the files somewhere else entirely ----
    void aLibraryComesBackWithItsFilesSomewhereElse_data()
    {
        QTest::addColumn<bool>("scanFirst");
        QTest::newRow("files found after importing") << false;
        QTest::newRow("files found before importing") << true;
    }

    void aLibraryComesBackWithItsFilesSomewhereElse()
    {
        QFETCH(bool, scanFirst);
        ExportReport exported;
        const QByteArray data = buildBundle(*m_from->db, exported);
        QCOMPARE(exported.publications, m_from->count());
        QCOMPARE(exported.tags, qint64(2));
        QCOMPARE(exported.collections, qint64(1));
        QCOMPARE(exported.annotations, qint64(1));
        QCOMPARE(exported.summaries, qint64(1));
        // Nothing about where the files were on that computer.
        QVERIFY(!data.contains(m_from->docs().toUtf8()));

        Library to;
        // Shuffled into other folders, under other names.
        for (qsizetype i = 0; i < kSamples.size(); ++i)
            to.put(kSamples.at(i), QStringLiteral("Elsewhere/%1/renamed-%2.%3").arg(i % 2).arg(i).arg(QFileInfo(kSamples.at(i)).suffix()));
        const QByteArray before = hashOfFolder(to.docs());
        if (scanFirst) {
            to.scan();
            QCOMPARE(to.count(), qint64(kSamples.size()));
        }

        QString error;
        const std::optional<Bundle> bundle = readBundle(data, &error);
        QVERIFY2(bundle, qPrintable(error));
        ImportOptions options;
        const ImportReport report = importBundle(*to.db, *bundle, options);
        if (scanFirst) {
            QCOMPARE(report.linked, qint64(kSamples.size()));
            QCOMPARE(report.waiting, qint64(0));
        } else {
            QCOMPARE(report.added, m_from->count());
            QCOMPARE(report.waiting, qint64(kSamples.size()));
            // Out of sight until their files are found, articles inside them too.
            QCOMPARE(to.count(), qint64(0));
            QCOMPARE(FileRepository(*to.db).waitingPublicationCount(), qint64(kSamples.size()));
            QCOMPARE(FileRepository(*to.db).hiddenCount(), qint64(0)); // Not kept for a removed folder, either.
            // Exported again before then, they still go, with what they're waiting for.
            ExportReport again;
            const std::optional<Bundle> reexported = readBundle(buildBundle(*to.db, again), nullptr);
            QCOMPARE(again.publications, m_from->count());
            qsizetype withFiles = 0;
            for (const QJsonValue &value : reexported->publications)
                withFiles += value.toObject().value(QStringLiteral("files")).toArray().isEmpty() ? 0 : 1;
            QCOMPARE(withFiles, kSamples.size());
            to.scan(); // The files are found, connected to what was waiting for them, and it all appears.
        }

        // The same publications, known by the same uuids, and no more.
        QCOMPARE(to.count(), m_from->count());
        QCOMPARE(to.uuids(), m_from->uuids());
        QCOMPARE(to.scalar("SELECT count(*) FROM expected_file"), qint64(0));
        QCOMPARE(to.scalar("SELECT count(*) FROM publication WHERE coalesce(parent_id, id) NOT IN (SELECT publication_id FROM publication_file)"),
                 qint64(0));

        // Every detail came with it.
        QCOMPARE(to.text("SELECT title FROM publication WHERE title = 'A Title Corrected By Hand'"), QStringLiteral("A Title Corrected By Hand"));
        QCOMPARE(to.scalar("SELECT rating FROM publication WHERE title = 'A Title Corrected By Hand'"), qint64(4));
        QCOMPARE(to.text("SELECT notes FROM publication WHERE title = 'A Title Corrected By Hand'"), QStringLiteral("My own made-up notes"));
        QCOMPARE(to.text("SELECT p.title FROM publication p WHERE p.parent_id IS NOT NULL"), QStringLiteral("An Article Inside"));
        QCOMPARE(to.text("SELECT group_concat(name, '>') FROM (SELECT name FROM tag ORDER BY parent_id IS NOT NULL)"), QStringLiteral("Physics>Crystals"));
        QCOMPARE(to.scalar("SELECT count(*) FROM publication_tag"), qint64(2));
        QCOMPARE(to.text("SELECT name FROM collection"), QStringLiteral("Made-Up Project"));
        QCOMPARE(to.scalar("SELECT count(*) FROM collection_publication"), qint64(2));
        QCOMPARE(to.text("SELECT note FROM annotation"), QStringLiteral("A made-up note"));
        QCOMPARE(to.text("SELECT location FROM reading_state"), QStringLiteral("{\"pdf\":{\"page\":3}}"));
        QCOMPARE(to.text("SELECT summary_text FROM publication_summary"), QStringLiteral("A made-up summary."));
        QVERIFY(to.scalar("SELECT count(*) FROM metadata_provenance WHERE source = 'manual'") >= 1);
        QCOMPARE(SettingsStore(*to.db).value(QStringLiteral("citation.style")).toString(), QStringLiteral("apa"));
        // A path from the other computer is only suggested.
        QCOMPARE(SettingsStore(*to.db).value(QStringLiteral("library.last_import_folder")).toString(), QString());
        QVERIFY(report.settings.suggestedPaths.contains(QStringLiteral("library.last_import_folder")));

        // The highlight is on the same contents, so it's found on this computer's copy.
        QCOMPARE(to.text("SELECT a.content_hash FROM annotation a"),
                 to.text("SELECT f.sha256 FROM file f WHERE f.relative_path LIKE '%renamed-4.pdf'"));
        // A publication only kept for a removed folder didn't come.
        QVERIFY(!to.uuids().contains(m_hiddenUuid));
        // Its documents were only read.
        QCOMPARE(hashOfFolder(to.docs()), before);
    }

    void importingTheSameExportTwiceChangesNothing()
    {
        ExportReport exported;
        const std::optional<Bundle> bundle = readBundle(buildBundle(*m_from->db, exported), nullptr);
        QVERIFY(bundle);
        Library to;
        for (const QString &name : kSamples)
            to.put(name, QStringLiteral("Other/") + name);
        to.scan();
        importBundle(*to.db, *bundle, {});
        const qint64 publications = to.count();
        const qint64 tags = to.scalar("SELECT count(*) FROM tag");
        const qint64 links = to.scalar("SELECT count(*) FROM publication_tag");
        const qint64 history = to.scalar("SELECT count(*) FROM metadata_provenance");

        const ImportReport again = importBundle(*to.db, *bundle, {});
        QCOMPARE(again.added, qint64(0));
        QCOMPARE(again.updated, qint64(0));
        QCOMPARE(again.unchanged, publications);
        QCOMPARE(again.tagsAdded, qint64(0));
        QCOMPARE(again.annotationsAdded, qint64(0));
        QCOMPARE(to.count(), publications);
        QCOMPARE(to.scalar("SELECT count(*) FROM tag"), tags);
        QCOMPARE(to.scalar("SELECT count(*) FROM publication_tag"), links);
        QCOMPARE(to.scalar("SELECT count(*) FROM metadata_provenance"), history);
        QCOMPARE(to.scalar("SELECT count(*) FROM collection"), qint64(1));
        QCOMPARE(to.scalar("SELECT count(*) FROM annotation"), qint64(1));

        // And back again: the original library knows them all.
        ExportReport back;
        const std::optional<Bundle> returned = readBundle(buildBundle(*to.db, back), nullptr);
        const ImportReport home = importBundle(*m_from->db, *returned, {});
        QCOMPARE(home.added, qint64(0));
        QCOMPARE(m_from->count(), publications);
    }

    void whatWasRemovedStaysOutOnTheOtherComputer()
    {
        const std::optional<qint64> thesis = m_from->idOfFile(QStringLiteral("Papers/thesis.pdf"));
        QVERIFY(thesis);
        const qint64 before = m_from->count();
        RemovedContent(*m_from->db).remove({*thesis}, 1000);
        ExportReport exported;
        const std::optional<Bundle> bundle = readBundle(buildBundle(*m_from->db, exported), nullptr);
        QVERIFY(bundle);
        QCOMPARE(exported.removed, qint64(1));
        QCOMPARE(exported.publications, before - 1);

        // A new library keeps it out once its folder's scanned.
        Library to;
        for (const QString &name : kSamples)
            to.put(name, QStringLiteral("Elsewhere/") + name);
        const ImportReport report = importBundle(*to.db, *bundle, {});
        QCOMPARE(report.removedAdded, qint64(1));
        to.scan();
        QCOMPARE(to.count(), before - 1);
        QVERIFY(!to.idOfFile(QStringLiteral("Elsewhere/thesis.pdf")));
        QCOMPARE(RemovedContent(*to.db).entries().first().fileName, QStringLiteral("thesis.pdf"));

        // One that still has it keeps it: merging never takes anything away.
        Library keeping;
        for (const QString &name : kSamples)
            keeping.put(name, name);
        keeping.scan();
        QCOMPARE(importBundle(*keeping.db, *bundle, {}).removedAdded, qint64(0));
        QVERIFY(keeping.idOfFile(QStringLiteral("thesis.pdf")));
        QCOMPARE(RemovedContent(*keeping.db).count(), qint64(0));
    }

    void theMoreRecentlyEditedWins()
    {
        Library to;
        for (const QString &name : kSamples)
            to.put(name, QStringLiteral("Other/") + name);
        to.scan();
        ExportReport exported;
        importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), {});
        PublicationRepository here(*to.db);
        PublicationRepository there(*m_from->db);
        const qint64 hereId = *here.idForUuid(m_correctedUuid);
        const qint64 thereId = *there.idForUuid(m_correctedUuid);

        // Edited later here: an older export doesn't undo it.
        QTest::qWait(5);
        Publication local = *here.get(hereId);
        local.notes = QStringLiteral("Edited here later");
        local.series = QStringLiteral("A Series Set Here");
        here.update(local);
        importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), {});
        QCOMPARE(here.get(hereId)->notes, QStringLiteral("Edited here later"));

        // Edited later there: it wins, and what it replaced can be undone.
        QTest::qWait(5);
        Publication remote = *there.get(thereId);
        remote.notes = QStringLiteral("Edited there even later");
        remote.series = QStringLiteral("A Made-Up Series");
        there.update(remote);
        const ImportReport report = importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), {});
        QCOMPARE(report.updated, qint64(1));
        QCOMPARE(here.get(hereId)->notes, QStringLiteral("Edited there even later"));
        QCOMPARE(here.get(hereId)->series, QStringLiteral("A Made-Up Series"));
        // Notes are personal and keep no history, but a detail does.
        const QList<ProvenanceEntry> history = here.provenance(hereId, QStringLiteral("series"));
        QVERIFY(std::any_of(history.cbegin(), history.cend(), [](const ProvenanceEntry &entry) {
            return entry.source == QLatin1StringView("library import") && entry.previousValue == QLatin1StringView("A Series Set Here");
        }));

        // Merging only adds: something here that the export lacks stays.
        Publication extra = *here.get(hereId);
        extra.edition = QStringLiteral("2");
        here.update(extra);
        QTest::qWait(5);
        remote = *there.get(thereId);
        remote.notes = QStringLiteral("Last word from there");
        there.update(remote);
        importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), {});
        QCOMPARE(here.get(hereId)->notes, QStringLiteral("Last word from there"));
        QCOMPARE(here.get(hereId)->edition, QStringLiteral("2"));
    }

    void replacingKeepsABackupFirst()
    {
        Library to;
        for (const QString &name : kSamples)
            to.put(name, QStringLiteral("Other/") + name);
        to.put(QStringLiteral("preprint.pdf"), QStringLiteral("Other/not-in-the-export.pdf"));
        to.scan();
        to.db->exec("INSERT INTO tag (uuid, name, created_at) VALUES ('made-up-only-here', 'Only Here', 0)");
        const qint64 before = to.count();

        ImportOptions options;
        options.mode = ImportMode::Replace;
        options.backupPath = to.dir.filePath(QStringLiteral("backup.db"));
        ExportReport exported;
        const ImportReport report = importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), options);
        QCOMPARE(report.backupPath, options.backupPath);
        QVERIFY(QFile::exists(options.backupPath));
        {
            Database backup(options.backupPath);
            auto count = backup.prepare("SELECT count(*) FROM publication");
            count->step();
            QCOMPARE(count->int64At(0), before);
        }

        // Now a copy of the export: what was only here is gone, and its file is found again by the next scan.
        QCOMPARE(to.uuids(), m_from->uuids());
        QCOMPARE(to.scalar("SELECT count(*) FROM tag WHERE name = 'Only Here'"), qint64(0));
        QCOMPARE(to.scalar("SELECT count(*) FROM file WHERE relative_path LIKE '%not-in-the-export.pdf'"), qint64(0));
        to.scan();
        QCOMPARE(to.count(), m_from->count() + 1);

        // Replacing without somewhere for the backup is refused.
        options.backupPath.clear();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), options));
    }

    void onlyThePartsAskedForComeIn()
    {
        Library to;
        ImportOptions options;
        options.parts.tags = false;
        options.parts.annotations = false;
        options.parts.settings = false;
        ExportReport exported;
        importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), options);
        // Waiting for their files, out of sight.
        QCOMPARE(to.scalar("SELECT count(*) FROM publication"), m_from->count());
        QCOMPARE(to.count(), qint64(0));
        QCOMPARE(to.scalar("SELECT count(*) FROM tag"), qint64(0));
        QCOMPARE(to.scalar("SELECT count(*) FROM collection"), qint64(0));
        QCOMPARE(to.scalar("SELECT count(*) FROM annotation"), qint64(0));
        QCOMPARE(SettingsStore(*to.db).value(QStringLiteral("citation.style")).toString(), QStringLiteral("harvard"));
    }

    // ---- Files that aren't certain ----
    void anotherCopyOfTheSamePaperIsSuggested()
    {
        Library to;
        // The same paper as another download: same identifier, other contents. And a file saved again under the
        // same name, at the same size, but changed.
        to.put(QStringLiteral("preprint_stamp.pdf"), QStringLiteral("Downloads/other-copy.pdf"));
        QFile original(sample(QStringLiteral("thesis.pdf")));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QByteArray changed = original.readAll();
        changed[changed.size() - 3] = changed.at(changed.size() - 3) == 'x' ? 'y' : 'x';
        QDir().mkpath(QDir(to.docs()).filePath(QStringLiteral("Saved")));
        QFile resaved(QDir(to.docs()).filePath(QStringLiteral("Saved/thesis.pdf")));
        QVERIFY(resaved.open(QIODevice::WriteOnly));
        resaved.write(changed);
        resaved.close();
        to.scan();
        const qint64 localCopy = *to.idOfFile(QStringLiteral("Downloads/other-copy.pdf"));
        PublicationRepository here(*to.db);
        Publication withDoi = *here.get(localCopy);
        const QString doi = withDoi.doi;
        QVERIFY2(!doi.isEmpty(), "the sample should have a DOI");

        // On the other computer, the paper has that DOI too, with a highlight.
        PublicationRepository there(*m_from->db);
        Publication paper = *there.get(*m_from->idOfFile(QStringLiteral("Papers/article.pdf")));
        paper.doi = doi;
        there.update(paper);
        auto highlight = m_from->db->prepare("INSERT INTO annotation (uuid, publication_id, content_hash, location_json, type, note, "
                                             "created_at, updated_at) VALUES ('33333333-0000-0000-0000-000000000002', ?, "
                                             "(SELECT sha256 FROM file WHERE relative_path = 'Papers/article.pdf'), '{}', 'highlight', "
                                             "'On the paper', 1, 1)");
        highlight->bind(1, paper.id);
        highlight->step();

        ExportReport exported;
        const ImportReport report = importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), {});
        QCOMPARE(report.proposed, qint64(2));
        const QList<FileProposal> proposals = fileProposals(*to.db);
        QCOMPARE(proposals.size(), 2);
        QCOMPARE(fileProposalCount(*to.db), qint64(2));
        QStringList reasons;
        for (const FileProposal &proposal : proposals)
            reasons << proposal.reason;
        reasons.sort();
        QCOMPARE(reasons, (QStringList{QStringLiteral("identifier"), QStringLiteral("name_and_size")}));

        // Accepting the other copy makes the two one publication, and the highlight may not line up any more.
        const qint64 before = to.count();
        for (const FileProposal &proposal : proposals) {
            if (proposal.reason == QLatin1StringView("identifier")) {
                QVERIFY(proposal.filePath.endsWith(QStringLiteral("Downloads/other-copy.pdf")));
                acceptFileProposal(*to.db, proposal.id);
            } else {
                rejectFileProposal(*to.db, proposal.id);
            }
        }
        // The imported paper appears, in place of the copy found here, which joined it.
        QCOMPARE(to.count(), before);
        QVERIFY(!here.get(localCopy));
        QCOMPARE(to.scalar("SELECT needs_review FROM annotation WHERE note = 'On the paper'"), qint64(1));
        QCOMPARE(to.scalar("SELECT needs_review FROM annotation WHERE note = 'A made-up note'"), qint64(0));
        QCOMPARE(fileProposalCount(*to.db), qint64(0));
        QVERIFY(here.idForUuid(paper.uuid));
        QCOMPARE(to.scalar("SELECT count(*) FROM publication_file pf JOIN publication p ON p.id = pf.publication_id "
                           "WHERE p.uuid = (SELECT uuid FROM publication WHERE doi IS NOT NULL AND notes = 'My own made-up notes')"),
                 qint64(1));
    }

    void aLibraryThatImportedEarlierHidesWhatsWaiting()
    {
        // As a library that imported before waiting publications were hidden would be.
        Library to;
        ExportReport exported;
        importBundle(*to.db, *readBundle(buildBundle(*m_from->db, exported), nullptr), {});
        to.db->exec("DELETE FROM hidden_publication");
        QCOMPARE(to.count(), m_from->count());
        to.db->exec(libraryMigrations().at(11).sql); // Version 12, which hides them.
        QCOMPARE(libraryMigrations().at(11).version, 12);
        QCOMPARE(to.count(), qint64(0));
        for (const QString &name : kSamples)
            to.put(name, QStringLiteral("Later/") + name);
        to.scan();
        QCOMPARE(to.count(), m_from->count());
    }

    // ---- The file itself ----
    void bundlesAreZipsOfVersionedJson()
    {
        ExportReport exported;
        const QByteArray data = buildBundle(*m_from->db, exported);
        ZipReader zip;
        QVERIFY(zip.open(data));
        for (const QString &name : {QStringLiteral("manifest.json"), QStringLiteral("publications.json"), QStringLiteral("tags.json"),
                                    QStringLiteral("collections.json"), QStringLiteral("annotations.json"),
                                    QStringLiteral("reading.json"), QStringLiteral("provenance.json"),
                                    QStringLiteral("summaries.json"), QStringLiteral("settings.json")})
            QVERIFY2(zip.contains(name), qPrintable(name));
        const QJsonObject manifest = QJsonDocument::fromJson(*zip.read(QStringLiteral("manifest.json"))).object();
        QCOMPARE(manifest.value(QStringLiteral("version")).toInt(), kBundleVersion);
        // The API key is never there, nor anything that only means something on this computer.
        const QByteArray settings = *zip.read(QStringLiteral("settings.json"));
        QVERIFY(!settings.contains("window.geometry"));
        QVERIFY(!settings.contains("api_key"));
    }

    void whatIsntAnExportIsRefused()
    {
        QString error;
        QVERIFY(!readBundle("not a zip at all", &error));
        QVERIFY(error.contains(QStringLiteral("isn't")));

        ZipWriter other;
        other.add(QStringLiteral("manifest.json"), R"({"format": "something-else", "version": 1})");
        QVERIFY(!readBundle(other.finish(), &error));

        ZipWriter newer;
        newer.add(QStringLiteral("manifest.json"), R"({"format": "papertrail-library", "version": 99})");
        QVERIFY(!readBundle(newer.finish(), &error));
        QVERIFY(error.contains(QStringLiteral("newer version")));

        // A damaged part is noticed, rather than half read.
        ExportReport exported;
        QByteArray data = buildBundle(*m_from->db, exported);
        const qsizetype middle = data.indexOf("publications.json") + 40;
        data[middle] = char(data.at(middle) ^ 0x55);
        QVERIFY(!readBundle(data, &error));

        // One with parts missing still reads, as one from another version might.
        ZipWriter sparse;
        sparse.add(QStringLiteral("manifest.json"), R"({"format": "papertrail-library", "version": 1})");
        const std::optional<Bundle> bundle = readBundle(sparse.finish(), &error);
        QVERIFY2(bundle, qPrintable(error));
        QVERIFY(bundle->publications.isEmpty());
    }

    void zipsRoundTrip()
    {
        ZipWriter writer;
        QByteArray big;
        for (int i = 0; i < 20000; ++i)
            big += "line " + QByteArray::number(i) + '\n';
        writer.add(QStringLiteral("big.txt"), big);
        writer.add(QStringLiteral("empty.txt"), QByteArray());
        writer.add(QStringLiteral("名前.json"), "{}");
        const QByteArray archive = writer.finish();
        QVERIFY(archive.size() < big.size() / 4); // Compressed.

        ZipReader reader;
        QVERIFY(reader.open(archive));
        QCOMPARE(reader.names(), (QList<QString>{QStringLiteral("big.txt"), QStringLiteral("empty.txt"), QStringLiteral("名前.json")}));
        QCOMPARE(*reader.read(QStringLiteral("big.txt")), big);
        QCOMPARE(*reader.read(QStringLiteral("empty.txt")), QByteArray());
        QCOMPARE(*reader.read(QStringLiteral("名前.json")), QByteArray("{}"));
        QVERIFY(!reader.read(QStringLiteral("big.txt"), 100)); // Larger than allowed.
        QVERIFY(!reader.read(QStringLiteral("missing.txt")));
    }

    void anExportFromEarlierStillImports()
    {
        // Made once by this version, and kept, so later versions and other platforms must keep reading it.
        QString error;
        const std::optional<Bundle> bundle = readBundleFile(QStringLiteral(PAPERTRAIL_TEST_FIXTURES "/transfer/library-v1.papertrail"), &error);
        QVERIFY2(bundle, qPrintable(error));
        QCOMPARE(bundle->version, 1);
        Library to;
        for (qsizetype i = 0; i < kSamples.size(); ++i)
            to.put(kSamples.at(i), QStringLiteral("Anywhere/%1").arg(i) + u'.' + QFileInfo(kSamples.at(i)).suffix());
        importBundle(*to.db, *bundle, {});
        to.scan();
        QCOMPARE(to.count(), qint64(bundle->publications.size()));
        QCOMPARE(to.text("SELECT title FROM publication WHERE title = 'A Title Corrected By Hand'"), QStringLiteral("A Title Corrected By Hand"));
        QCOMPARE(to.scalar("SELECT count(*) FROM publication WHERE coalesce(parent_id, id) NOT IN (SELECT publication_id FROM publication_file)"),
                 qint64(0));
        QCOMPARE(to.scalar("SELECT count(*) FROM annotation"), qint64(1));
    }

    // Writes the fixture above, when asked. Only run by hand, when the format changes in a new version.
    void writeFixtureWhenAsked()
    {
        const QString path = qEnvironmentVariable("PAPERTRAIL_WRITE_TRANSFER_FIXTURE");
        if (path.isEmpty())
            QSKIP("Only when PAPERTRAIL_WRITE_TRANSFER_FIXTURE is set");
        // Nothing from this computer goes in a file that's kept.
        SettingsStore(*m_from->db).setValue(QStringLiteral("library.last_import_folder"), QStringLiteral("/a/made-up/folder"));
        exportLibrary(*m_from->db, path);
    }

private:
    // Things a person adds: corrections, tags, a collection, a highlight, a place, a summary, settings, an article,
    // and a folder they removed.
    void decorate(Library &library)
    {
        Database &db = *library.db;
        PublicationRepository publications(db);
        const qint64 article = *library.idOfFile(QStringLiteral("Papers/article.pdf"));
        Publication corrected = *publications.get(article);
        corrected.title = QStringLiteral("A Title Corrected By Hand");
        corrected.rating = 4;
        corrected.notes = QStringLiteral("My own made-up notes");
        publications.updateWithProvenance(corrected, QStringLiteral("manual"));
        m_correctedUuid = corrected.uuid;

        db.exec("INSERT INTO tag (uuid, name, created_at) VALUES ('11111111-0000-0000-0000-000000000001', 'Physics', 1)");
        db.exec("INSERT INTO tag (uuid, name, parent_id, colour, created_at) VALUES ('11111111-0000-0000-0000-000000000002', 'Crystals', "
                "(SELECT id FROM tag WHERE name = 'Physics'), '#00ff00', 2)");
        const qint64 book = *library.idOfFile(QStringLiteral("Papers/book.epub"));
        auto tag = db.prepare("INSERT INTO publication_tag (publication_id, tag_id) VALUES (?, (SELECT id FROM tag WHERE name = 'Crystals'))");
        for (const qint64 id : {article, book}) {
            tag->bind(1, id);
            tag->step();
            tag->reset();
        }
        db.exec("INSERT INTO collection (uuid, name, created_at, updated_at) VALUES ('22222222-0000-0000-0000-000000000001', 'Made-Up Project', 1, 1)");
        auto member = db.prepare("INSERT INTO collection_publication (collection_id, publication_id, added_at) VALUES (1, ?, 1)");
        for (const qint64 id : {article, book}) {
            member->bind(1, id);
            member->step();
            member->reset();
        }

        const qint64 volume = *library.idOfFile(QStringLiteral("Papers/reader_doc.pdf"));
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article Inside");
        draft.firstPage = 2;
        draft.lastPage = 3;
        createArticles(db, volume, {draft}, false);
        const QString volumeHash = library.text("SELECT sha256 FROM file WHERE relative_path = 'Papers/reader_doc.pdf'");
        auto annotation = db.prepare("INSERT INTO annotation (uuid, publication_id, content_hash, location_json, type, colour, "
                                     "selected_text, note, created_at, updated_at) VALUES ('33333333-0000-0000-0000-000000000001', "
                                     "?, ?, '{\"parts\":[]}', 'highlight', '#ffff00', 'findme', 'A made-up note', 1, 1)");
        annotation->bind(1, volume);
        annotation->bind(2, volumeHash);
        annotation->step();
        auto place = db.prepare("INSERT INTO reading_state (publication_id, location, zoom, last_opened) VALUES (?, '{\"pdf\":{\"page\":3}}', 1.5, 5)");
        place->bind(1, volume);
        place->step();
        auto summary = db.prepare("INSERT INTO publication_summary (uuid, publication_id, model, effort, basis, summary_text, sources_json, "
                                  "created_at) VALUES ('44444444-0000-0000-0000-000000000001', ?, 'made-up-model', 'high', 'abstract', "
                                  "'A made-up summary.', '[]', 1)");
        summary->bind(1, book);
        summary->step();

        SettingsStore settings(db);
        settings.setValue(QStringLiteral("citation.style"), QStringLiteral("apa"));
        settings.setValue(QStringLiteral("library.last_import_folder"), library.docs());

        // A removed folder's publication, kept only in case the folder comes back.
        const QString removed = library.dir.filePath(QStringLiteral("removed"));
        QDir().mkpath(removed);
        QVERIFY(QFile::copy(sample(QStringLiteral("standard.pdf")), removed + QStringLiteral("/standard.pdf")));
        FileRepository files(db);
        const qint64 removedRoot = files.addRoot(QStringLiteral("Removed"), removed);
        library.service->scan(removedRoot);
        library.service->processQueue();
        m_hiddenUuid = library.text("SELECT p.uuid FROM publication p JOIN publication_file pf ON pf.publication_id = p.id "
                                    "JOIN file f ON f.id = pf.file_id WHERE f.relative_path = 'standard.pdf'");
        QVERIFY(!m_hiddenUuid.isEmpty());
        files.hideRoot(removedRoot);
    }

    std::unique_ptr<Library> m_from;
    QString m_correctedUuid;
    QString m_hiddenUuid;
};

QTEST_GUILESS_MAIN(TransferTest)
#include "test_transfer.moc"
