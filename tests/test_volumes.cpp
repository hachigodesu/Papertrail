// Tests of volumes and the articles inside them.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/query/LibraryQuery.h"
#include "core/repo/CollectionRepository.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/TagRepository.h"
#include "core/repo/VolumeArticles.h"
#include "import/ImportService.h"
#include "ui/DetailsPanel.h"
#include "ui/Labels.h"
#include "ui/ImportThread.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/PageView.h"
#include "ui/ReaderTab.h"
#include "ui/VolumeDialogs.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QScrollBar>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::ui;

namespace {

constexpr int kLongWaitMs = 60000;

QString sample(const QString &name)
{
    return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name);
}

QByteArray hashOf(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

} // namespace

class VolumesTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_unexpectedBoxes.clear();
        m_boxWatcher = std::make_unique<QTimer>();
        connect(m_boxWatcher.get(), &QTimer::timeout, this, [this] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                m_unexpectedBoxes << box->text();
                box->close();
            }
        });
        m_boxWatcher->start(100);

        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("library.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        paths.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        paths.settleMs = 0;
        m_session = std::make_unique<LibrarySession>(paths);
        m_session->settings().setValue(QStringLiteral("library.rescan_on_startup"), false);
    }

    void cleanup()
    {
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
        m_window.reset();
        m_session.reset();
        m_dir.reset();
    }

    // ---- Turning contents into articles ----

    void entriesRunUntilTheNextOne()
    {
        const QList<ContentsEntry> entries = {
            {QStringLiteral("First Article"), 0, 2},
            {QStringLiteral("Second Article"), 0, 10},
            {QStringLiteral("Third Article"), 0, 20},
        };
        const QList<ArticleDraft> drafts = draftsFromContents(entries, 0, 30);
        QCOMPARE(drafts.size(), 3);
        QCOMPARE(drafts.at(0).firstPage, 3);
        QCOMPARE(drafts.at(0).lastPage, 10);
        QCOMPARE(drafts.at(1).firstPage, 11);
        QCOMPARE(drafts.at(1).lastPage, 20);
        QCOMPARE(drafts.at(2).firstPage, 21);
        QCOMPARE(drafts.at(2).lastPage, 30); // The last one runs to the end of the file.
    }

    void anEntryEndsAtTheNextOneAtItsLevelOrAbove()
    {
        const QList<ContentsEntry> entries = {
            {QStringLiteral("Section One"), 0, 0},
            {QStringLiteral("Article A"), 1, 1},
            {QStringLiteral("A subheading inside it"), 2, 3},
            {QStringLiteral("Article B"), 1, 6},
            {QStringLiteral("Section Two"), 0, 12},
            {QStringLiteral("Article C"), 1, 13},
        };
        const QList<ArticleDraft> articles = draftsFromContents(entries, 1, 20);
        QCOMPARE(articles.size(), 3);
        QCOMPARE(articles.at(0).title, QStringLiteral("Article A"));
        QCOMPARE(articles.at(0).lastPage, 6); // The subheading is inside it.
        QCOMPARE(articles.at(1).lastPage, 12); // The next section ends it, though it's a level above.
        QCOMPARE(articles.at(2).lastPage, 20);

        const QList<ArticleDraft> sections = draftsFromContents(entries, 0, 20);
        QCOMPARE(sections.size(), 2);
        QCOMPARE(sections.at(0).lastPage, 12);
    }

    void articlesStartingOnOnePageDontGoBackwards()
    {
        const QList<ContentsEntry> entries = {
            {QStringLiteral("Short Note"), 0, 4},
            {QStringLiteral("Another Note"), 0, 4},
            {QStringLiteral("Last Note"), 0, 5},
        };
        const QList<ArticleDraft> drafts = draftsFromContents(entries, 0, 8);
        QCOMPARE(drafts.at(0).firstPage, 5);
        QCOMPARE(drafts.at(0).lastPage, 5);
        QCOMPARE(drafts.at(1).lastPage, 5);
        QCOMPARE(drafts.at(2).lastPage, 8);
    }

    void entriesLeadingNowhereAreLeftOut()
    {
        const QList<ContentsEntry> entries = {
            {QStringLiteral("Goes Nowhere"), 0, -1},
            {QStringLiteral("  "), 0, 1},
            {QStringLiteral("Past The End"), 0, 99},
            {QStringLiteral("  Spread \n over   lines "), 0, 2},
        };
        const QList<ArticleDraft> drafts = draftsFromContents(entries, 0, 10);
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(drafts.first().title, QStringLiteral("Spread over lines"));
    }

    void theMostCommonLevelIsTheLikelyOneForArticles()
    {
        const QList<ContentsEntry> nested = {
            {QStringLiteral("Part"), 0, 0},
            {QStringLiteral("A"), 1, 1},
            {QStringLiteral("B"), 1, 2},
            {QStringLiteral("C"), 2, 2},
        };
        QCOMPARE(likelyArticleDepth(nested), 1);
        QCOMPARE(likelyArticleDepth({{QStringLiteral("A"), 0, 0}, {QStringLiteral("B"), 1, 1}}), 0); // A tie.
        QCOMPARE(likelyArticleDepth({}), 0);
    }

    void partsOfAVolumeThatArentArticlesAreNoticed_data()
    {
        QTest::addColumn<QString>("title");
        QTest::addColumn<bool>("notAnArticle");
        QTest::newRow("contents") << QStringLiteral("Contents") << true;
        QTest::newRow("table of contents") << QStringLiteral("Table of Contents") << true;
        QTest::newRow("front matter") << QStringLiteral("Front matter") << true;
        QTest::newRow("back cover") << QStringLiteral("Back Cover") << true;
        QTest::newRow("index") << QStringLiteral("Author Index") << true;
        QTest::newRow("masthead") << QStringLiteral("Masthead") << true;
        QTest::newRow("an article") << QStringLiteral("Fatigue of Steel Beams") << false;
        QTest::newRow("mentions an index") << QStringLiteral("A New Index of Fatigue") << false;
        QTest::newRow("mentions a cover") << QStringLiteral("Soil Cover and Erosion") << false;
    }

    void partsOfAVolumeThatArentArticlesAreNoticed()
    {
        QFETCH(QString, title);
        QFETCH(bool, notAnArticle);
        const QList<ArticleDraft> drafts = draftsFromContents({{title, 0, 0}}, 0, 5);
        QCOMPARE(drafts.first().likelyNotAnArticle, notAnArticle);
    }

    // ---- Creating them ----

    void articlesCarryOverWhatTheyShareWithTheVolume()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("Journal of Made-up Results, Volume 12");
        volume.containerTitle = QStringLiteral("Journal of Made-up Results");
        volume.volume = QStringLiteral("12");
        volume.issue = QStringLiteral("3");
        volume.year = 2019;
        volume.publisher = QStringLiteral("Example Press");
        volume.issn = QStringLiteral("1234-5679");
        const qint64 volumeId = m_session->publications().create(volume);

        ArticleDraft draft;
        draft.title = QStringLiteral("Fatigue of Steel Beams");
        draft.firstPage = 5;
        draft.lastPage = 9;
        const qint64 id = createArticles(m_session->database(), volumeId, {draft}, true).first();

        const Publication article = *m_session->publications().get(id);
        QCOMPARE(article.type, PublicationType::JournalArticle);
        QCOMPARE(article.parentId.value(), volumeId);
        QCOMPARE(article.containerTitle, QStringLiteral("Journal of Made-up Results"));
        QCOMPARE(article.volume, QStringLiteral("12"));
        QCOMPARE(article.issue, QStringLiteral("3"));
        QCOMPARE(article.year.value(), 2019);
        QCOMPARE(article.publisher, QStringLiteral("Example Press"));
        QCOMPARE(article.issn, QStringLiteral("1234-5679"));
        QCOMPARE(article.pageStart.value(), 5);
        QCOMPARE(article.pageEnd.value(), 9);
        QVERIFY(article.needsReview);
        // Those are positions in the file, and a citation wants the printed page numbers.
        QCOMPARE(article.pages, QString());
    }

    void eachKindOfVolumeHasItsOwnKindOfArticle_data()
    {
        QTest::addColumn<PublicationType>("volume");
        QTest::addColumn<PublicationType>("article");
        QTest::newRow("journal") << PublicationType::JournalIssue << PublicationType::JournalArticle;
        QTest::newRow("proceedings") << PublicationType::ConferenceProceedings << PublicationType::ConferencePaper;
        QTest::newRow("book") << PublicationType::Book << PublicationType::BookChapter;
        QTest::newRow("textbook") << PublicationType::Textbook << PublicationType::BookChapter;
    }

    void eachKindOfVolumeHasItsOwnKindOfArticle()
    {
        QFETCH(PublicationType, volume);
        QFETCH(PublicationType, article);
        QVERIFY(canHoldArticles(volume));
        QCOMPARE(articleTypeFor(volume), article);
        QVERIFY(!canHoldArticles(PublicationType::JournalArticle));
        QVERIFY(!canHoldArticles(PublicationType::Thesis));
    }

    void aVolumeWithoutItsOwnJournalNameLendsItsTitle()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("Annals of Examples");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        const qint64 id = createArticles(m_session->database(), volumeId, {draft}, false).first();
        QCOMPARE(m_session->publications().get(id)->containerTitle, QStringLiteral("Annals of Examples"));
        QVERIFY(!m_session->publications().get(id)->needsReview);
    }

    void anArticleCantHaveArticles()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("A Volume");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        const qint64 article = createArticles(m_session->database(), volumeId, {draft}, false).first();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, createArticles(m_session->database(), article, {draft}, false));
        draft.title = QStringLiteral("   ");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, createArticles(m_session->database(), volumeId, {draft}, false));
    }

    void numberedEntriesAreNoticed_data()
    {
        QTest::addColumn<QString>("title");
        QTest::addColumn<bool>("placeholder");
        QTest::newRow("two digits") << QStringLiteral("01") << true;
        QTest::newRow("with a dot") << QStringLiteral(" 12. ") << true;
        QTest::newRow("roman") << QStringLiteral("xiv") << true;
        QTest::newRow("article") << QStringLiteral("Article 3") << true;
        QTest::newRow("chapter") << QStringLiteral("Chapter 7") << true;
        QTest::newRow("no.") << QStringLiteral("No. 4") << true;
        QTest::newRow("untitled") << QStringLiteral("Untitled") << true;
        QTest::newRow("empty") << QString() << true;
        QTest::newRow("a title") << QStringLiteral("Fatigue of Steel Beams") << false;
        QTest::newRow("starts with a number") << QStringLiteral("3D Printing of Beams") << false;
        QTest::newRow("a title with a number") << QStringLiteral("Chapter 7: Beams") << false;
        QTest::newRow("a word made of numerals") << QStringLiteral("Civil") << false;
    }

    void numberedEntriesAreNoticed()
    {
        QFETCH(QString, title);
        QFETCH(bool, placeholder);
        QCOMPARE(isPlaceholderTitle(title), placeholder);
    }

    void aNumberedEntryTakesItsTitleFromItsPage()
    {
        ArticleDraft draft;
        draft.title = QStringLiteral("02");
        QVERIFY(adoptPageHeading(draft, QStringLiteral("  Fatigue of   Steel Beams "), QStringLiteral("10.5555/beams")));
        QCOMPARE(draft.title, QStringLiteral("Fatigue of Steel Beams"));
        QVERIFY(draft.titleFromPage);
        QCOMPARE(draft.doi, QStringLiteral("10.5555/beams"));
        QVERIFY(!draft.likelyNotAnArticle);

        // A named entry keeps its name, though it still takes a DOI it lacks.
        ArticleDraft named;
        named.title = QStringLiteral("What the Contents Say");
        QVERIFY(adoptPageHeading(named, QStringLiteral("Something Else Entirely"), QStringLiteral("10.5555/named")));
        QCOMPARE(named.title, QStringLiteral("What the Contents Say"));
        QVERIFY(!named.titleFromPage);
        QCOMPARE(named.doi, QStringLiteral("10.5555/named"));

        // A DOI it already has isn't replaced.
        QVERIFY(!adoptPageHeading(named, QString(), QStringLiteral("10.5555/other")));
        QCOMPARE(named.doi, QStringLiteral("10.5555/named"));

        // Nothing useful on the page leaves the number, for someone to type over.
        ArticleDraft blank;
        blank.title = QStringLiteral("03");
        QVERIFY(!adoptPageHeading(blank, QStringLiteral("7"), QString()));
        QVERIFY(!adoptPageHeading(blank, QStringLiteral("  "), QString()));
        QCOMPARE(blank.title, QStringLiteral("03"));

        // A page that's really an index says so.
        ArticleDraft index;
        index.title = QStringLiteral("49");
        QVERIFY(adoptPageHeading(index, QStringLiteral("Author Index"), QString()));
        QVERIFY(index.likelyNotAnArticle);
    }

    void articlesMadeWithOnlyNumbersAreNamedRatherThanRepeated()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("An Issue");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft numbered;
        numbered.title = QStringLiteral("02");
        numbered.firstPage = 2;
        numbered.lastPage = 3;
        ArticleDraft named;
        named.title = QStringLiteral("Already Named");
        named.firstPage = 4;
        named.lastPage = 4;
        const QList<qint64> made = createArticles(m_session->database(), volumeId, {numbered, named}, false);

        // Someone writes notes on the numbered one before its title is fixed.
        Publication tagged = *m_session->publications().get(made.at(0));
        tagged.notes = QStringLiteral("Keep these notes");
        m_session->publications().update(tagged);

        QList<ArticleDraft> drafts = {numbered, named};
        markExistingArticles(m_session->database(), volumeId, drafts);
        QCOMPARE(drafts.at(0).renames, std::optional<qint64>(made.at(0)));
        QVERIFY(!drafts.at(0).alreadyAdded);
        QVERIFY(!drafts.at(1).renames);
        QVERIFY(drafts.at(1).alreadyAdded);

        QVERIFY(adoptPageHeading(drafts[0], QStringLiteral("The Real Title"), QStringLiteral("10.5555/real")));
        const QList<qint64> ids = createArticles(m_session->database(), volumeId, {drafts.at(0)}, true);
        QCOMPARE(ids, QList<qint64>{made.at(0)});
        QCOMPARE(m_session->publications().childIds(volumeId).size(), 2);
        const Publication renamed = *m_session->publications().get(made.at(0));
        QCOMPARE(renamed.title, QStringLiteral("The Real Title"));
        QCOMPARE(renamed.doi, QStringLiteral("10.5555/real"));
        QCOMPARE(renamed.notes, QStringLiteral("Keep these notes"));
        QCOMPARE(renamed.pageStart.value(), 2);
        QVERIFY(renamed.needsReview);

        // Once it has a title, it's only already there, and a title typed by hand is never replaced.
        drafts = {numbered};
        markExistingArticles(m_session->database(), volumeId, drafts);
        QVERIFY(!drafts.at(0).renames);
        ArticleDraft stale = numbered;
        stale.title = QStringLiteral("Another Guess");
        stale.renames = made.at(0);
        createArticles(m_session->database(), volumeId, {stale}, true);
        QCOMPARE(m_session->publications().get(made.at(0))->title, QStringLiteral("The Real Title"));

        // Only its own articles can be named.
        Publication other;
        other.type = PublicationType::JournalIssue;
        other.title = QStringLiteral("Another Issue");
        const qint64 otherId = m_session->publications().create(other);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, createArticles(m_session->database(), otherId, {stale}, true));
    }

    void anIssuesNumbersFillOnlyWhatsMissing()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("An Issue");
        volume.issue = QStringLiteral("Typed By Hand");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        const qint64 article = createArticles(m_session->database(), volumeId, {draft}, false).first();

        QVERIFY(fillVolumeNumbering(m_session->database(), volumeId, QStringLiteral("7"), QStringLiteral("11"), 11, 2026));
        const Publication filled = *m_session->publications().get(volumeId);
        QCOMPARE(filled.volume, QStringLiteral("7"));
        QCOMPARE(filled.issue, QStringLiteral("Typed By Hand"));
        QCOMPARE(filled.year.value(), 2026);
        QCOMPARE(filled.month.value(), 11);
        const Publication articleFilled = *m_session->publications().get(article);
        QCOMPARE(articleFilled.volume, QStringLiteral("7"));
        QCOMPARE(articleFilled.issue, QStringLiteral("Typed By Hand")); // It took the issue's when it was made.

        // A month never goes with someone else's year.
        Publication dated;
        dated.type = PublicationType::JournalIssue;
        dated.title = QStringLiteral("Dated");
        dated.year = 2025;
        const qint64 datedId = m_session->publications().create(dated);
        fillVolumeNumbering(m_session->database(), datedId, QStringLiteral("6"), QStringLiteral("12"), 12, 2026);
        QCOMPARE(m_session->publications().get(datedId)->year.value(), 2025);
        QVERIFY(!m_session->publications().get(datedId)->month);

        // Nothing left to fill changes nothing.
        QVERIFY(!fillVolumeNumbering(m_session->database(), volumeId, QStringLiteral("8"), QStringLiteral("1"), 1, 2027));
        QCOMPARE(m_session->publications().get(volumeId)->volume, QStringLiteral("7"));

        // How they're shown, to tell issues of one journal apart.
        QCOMPARE(issueNumbering(QStringLiteral("7"), QStringLiteral("11"), std::nullopt, 2026), QStringLiteral("Vol. 7, No. 11, 2026"));
        QVERIFY(issueNumbering(QStringLiteral("7"), QStringLiteral("11"), 11, 2026).startsWith(QStringLiteral("Vol. 7, No. 11, ")));
        QVERIFY(issueNumbering(QString(), QString(), std::nullopt, std::nullopt).isEmpty());
    }

    void whatsAlreadyThereIsNoticed()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("A Volume");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        draft.firstPage = 3;
        draft.lastPage = 4;
        createArticles(m_session->database(), volumeId, {draft}, false);

        QList<ArticleDraft> drafts;
        ArticleDraft same = draft;
        same.title = QStringLiteral("  an   ARTICLE ");
        ArticleDraft otherPage = draft;
        otherPage.firstPage = 8;
        drafts << same << otherPage;
        markExistingArticles(m_session->database(), volumeId, drafts);
        QVERIFY(drafts.at(0).alreadyAdded);
        QVERIFY(!drafts.at(1).alreadyAdded);
    }

    // ---- In the library ----

    void articlesCanBeFoundTaggedAndCollectedOnTheirOwn()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("A Volume");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft draft;
        draft.title = QStringLiteral("Findable Article");
        const qint64 article = createArticles(m_session->database(), volumeId, {draft}, false).first();

        PublicationFilter search;
        search.searchText = QStringLiteral("Findable");
        QCOMPARE(m_session->query().ids(search, {}), QList<qint64>{article});

        const qint64 tag = m_session->tags().ensurePath(QStringLiteral("Only The Article"));
        m_session->tags().assign(article, tag);
        QVERIFY(m_session->tags().tagsFor(volumeId).isEmpty());
        PublicationFilter tagged;
        tagged.tagIds = {tag};
        QCOMPARE(m_session->query().ids(tagged, {}), QList<qint64>{article});

        // Adding a volume to a collection adds only the volume.
        const qint64 collection = m_session->collections().create(QStringLiteral("Project"));
        m_session->collections().add(collection, {volumeId});
        QVERIFY(!m_session->collections().contains(collection, article));
    }

    void deletingAVolumesDetailsTakesItsArticlesToo()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("A Volume");
        const qint64 volumeId = m_session->publications().create(volume);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        const qint64 article = createArticles(m_session->database(), volumeId, {draft}, false).first();
        m_session->publications().remove(volumeId);
        QVERIFY(!m_session->publications().get(article).has_value());
    }

    // ---- The contents dialog, with a real file and a real worker ----

    void theContentsOfARealFileAreOffered()
    {
        const qint64 volume = importVolume();
        const QByteArray before = hashOf(volumePath());
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy ready(&dialog, &ContentsDialog::contentsReady);
        QVERIFY2(ready.wait(kLongWaitMs), qPrintable(dialog.messageLabel()->text()));

        // reader_doc.pdf has two chapters, and the first has a section inside it.
        QCOMPARE(dialog.levelBox()->count(), 2);
        QCOMPARE(dialog.levelBox()->currentData().toInt(), 0);
        QCOMPARE(dialog.table()->rowCount(), 2);
        QCOMPARE(dialog.table()->item(0, 0)->text(), QStringLiteral("Chapter One"));
        QCOMPARE(dialog.table()->item(1, 0)->text(), QStringLiteral("Chapter Two"));
        QCOMPARE(dialog.table()->item(0, 0)->checkState(), Qt::Checked);

        dialog.levelBox()->setCurrentIndex(1);
        QCOMPARE(dialog.table()->rowCount(), 1);
        QCOMPARE(dialog.table()->item(0, 0)->text(), QStringLiteral("A Section"));
        dialog.levelBox()->setCurrentIndex(0);

        // Nothing has been created by looking.
        QVERIFY(m_session->publications().childIds(volume).isEmpty());

        // Only what's ticked, with any corrections.
        dialog.table()->item(1, 0)->setCheckState(Qt::Unchecked);
        dialog.table()->item(0, 0)->setText(QStringLiteral("A Corrected Title"));
        QVERIFY(dialog.createChosen());
        const QList<qint64> created = m_session->publications().childIds(volume);
        QCOMPARE(created.size(), 1);
        const Publication article = *m_session->publications().get(created.first());
        QCOMPARE(article.title, QStringLiteral("A Corrected Title"));
        QCOMPARE(article.pageStart.value(), 1);
        QVERIFY(article.needsReview);

        // Asking again notices what's there, and doesn't offer it by default.
        ContentsDialog again(*m_session, volume);
        QSignalSpy readyAgain(&again, &ContentsDialog::contentsReady);
        QVERIFY(readyAgain.wait(kLongWaitMs));
        QCOMPARE(again.table()->item(1, 0)->checkState(), Qt::Checked);

        QCOMPARE(hashOf(volumePath()), before); // Its file is only ever read.
    }

    void titlesTheContentsOnlyNumberAreReadFromTheirPages()
    {
        const qint64 volume = importVolume(QStringLiteral("numbered_issue.pdf"));
        const QByteArray before = hashOf(volumePath());
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy ready(&dialog, &ContentsDialog::contentsReady);
        QSignalSpy read(&dialog, &ContentsDialog::headingsRead);
        QVERIFY2(ready.wait(kLongWaitMs), qPrintable(dialog.messageLabel()->text()));
        QCOMPARE(dialog.table()->rowCount(), 3);
        if (dialog.headingsPending() > 0)
            QVERIFY(read.wait(kLongWaitMs));
        QCOMPARE(dialog.headingsPending(), 0);
        QVERIFY(!dialog.messageLabel()->text().contains(QStringLiteral("left")));

        // The cover's largest text is the journal's name, and the articles' are their titles, across two lines.
        QCOMPARE(dialog.table()->item(0, 0)->text(), QStringLiteral("Journal of Imaginary Findings"));
        QCOMPARE(dialog.table()->item(1, 0)->text(), QStringLiteral("A Made-Up Study of Lattice Vibrations in Pretend Crystals"));
        QCOMPARE(dialog.table()->item(2, 0)->text(), QStringLiteral("Another Invented Article on Fluid Flow"));
        QVERIFY(dialog.table()->item(1, 3)->text().contains(QStringLiteral("first page")));

        // Only the second has one DOI before its references. The third prints two, so neither is taken.
        const QList<ArticleDraft> drafts = dialog.chosenDrafts();
        QCOMPARE(drafts.size(), 3);
        QCOMPARE(drafts.at(1).doi, QStringLiteral("10.5555/imaginary.2026.0002"));
        QVERIFY(drafts.at(2).doi.isEmpty());

        // A title typed over is kept, and a DOI only goes with the page it was found on.
        dialog.table()->item(0, 0)->setCheckState(Qt::Unchecked);
        dialog.table()->item(2, 0)->setText(QStringLiteral("Typed By Hand"));
        QVERIFY(dialog.createChosen());
        QList<qint64> ids = m_session->publications().childIds(volume);
        QCOMPARE(ids.size(), 2);
        QStringList titles;
        for (const qint64 id : ids) {
            const Publication article = *m_session->publications().get(id);
            titles << article.title;
            if (article.pageStart == 2)
                QCOMPARE(article.doi, QStringLiteral("10.5555/imaginary.2026.0002"));
        }
        titles.sort();
        QCOMPARE(titles, (QStringList{QStringLiteral("A Made-Up Study of Lattice Vibrations in Pretend Crystals"),
                                      QStringLiteral("Typed By Hand")}));
        QCOMPARE(hashOf(volumePath()), before);
    }

    void anIssuesNumbersAreReadFromItsPages()
    {
        const qint64 volume = importVolume(QStringLiteral("numbered_issue.pdf"));
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy ready(&dialog, &ContentsDialog::contentsReady);
        QSignalSpy read(&dialog, &ContentsDialog::headingsRead);
        QVERIFY(ready.wait(kLongWaitMs));
        if (dialog.headingsPending() > 0)
            QVERIFY(read.wait(kLongWaitMs));
        QVERIFY(dialog.filledNumbering());
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("filled in")));
        const Publication filled = *m_session->publications().get(volume);
        QCOMPARE(filled.volume, QStringLiteral("3"));
        QCOMPARE(filled.issue, QStringLiteral("2"));

        // Articles made now take them from the issue.
        dialog.table()->item(0, 0)->setCheckState(Qt::Unchecked);
        QVERIFY(dialog.createChosen());
        for (const qint64 id : m_session->publications().childIds(volume))
            QCOMPARE(m_session->publications().get(id)->volume, QStringLiteral("3"));
    }

    void articlesMadeBeforeTitlesWereReadAreNamed()
    {
        const qint64 volume = importVolume(QStringLiteral("numbered_issue.pdf"));
        // As an earlier version made them, with the contents' numbers for titles.
        ArticleDraft numbered;
        numbered.title = QStringLiteral("02");
        numbered.firstPage = 2;
        numbered.lastPage = 3;
        const qint64 madeEarlier = createArticles(m_session->database(), volume, {numbered}, true).first();

        ContentsDialog dialog(*m_session, volume);
        QSignalSpy ready(&dialog, &ContentsDialog::contentsReady);
        QSignalSpy read(&dialog, &ContentsDialog::headingsRead);
        QVERIFY(ready.wait(kLongWaitMs));
        if (dialog.headingsPending() > 0)
            QVERIFY(read.wait(kLongWaitMs));
        QCOMPARE(dialog.table()->item(1, 0)->checkState(), Qt::Checked);
        QVERIFY(dialog.table()->item(1, 3)->text().contains(QStringLiteral("already made")));

        dialog.table()->item(0, 0)->setCheckState(Qt::Unchecked);
        dialog.table()->item(2, 0)->setCheckState(Qt::Unchecked);
        QVERIFY(dialog.createChosen());
        QCOMPARE(m_session->publications().childIds(volume), QList<qint64>{madeEarlier});
        const Publication renamed = *m_session->publications().get(madeEarlier);
        QCOMPARE(renamed.title, QStringLiteral("A Made-Up Study of Lattice Vibrations in Pretend Crystals"));
        QCOMPARE(renamed.doi, QStringLiteral("10.5555/imaginary.2026.0002"));
    }

    void pagesThatDontMakeSenseAreRefused()
    {
        const qint64 volume = importVolume();
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy ready(&dialog, &ContentsDialog::contentsReady);
        QVERIFY(ready.wait(kLongWaitMs));

        dialog.table()->item(0, 2)->setData(Qt::EditRole, 0);
        QVERIFY(!dialog.createChosen());
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("row 1")));
        dialog.table()->item(0, 2)->setData(Qt::EditRole, 1);
        dialog.table()->item(1, 2)->setData(Qt::EditRole, 99999);
        QVERIFY(!dialog.createChosen());
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("row 2")));
        dialog.table()->item(1, 0)->setText(QStringLiteral("   "));
        dialog.table()->item(1, 2)->setData(Qt::EditRole, 1);
        QVERIFY(!dialog.createChosen());
        QVERIFY(m_session->publications().childIds(volume).isEmpty());
    }

    void aFileWithNoContentsSaysSo()
    {
        const qint64 volume = importVolume(QStringLiteral("article.pdf"));
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy unavailable(&dialog, &ContentsDialog::contentsUnavailable);
        QVERIFY(unavailable.wait(kLongWaitMs));
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("no table of contents")));
    }

    void aFormatWithNoFixedPagesSaysSo()
    {
        const qint64 volume = importVolume(QStringLiteral("reader_nav.epub"));
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy unavailable(&dialog, &ContentsDialog::contentsUnavailable);
        QVERIFY(unavailable.wait(kLongWaitMs));
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("no fixed pages")));
    }

    void aVolumeWithNoFileSaysSo()
    {
        Publication volume;
        volume.type = PublicationType::JournalIssue;
        volume.title = QStringLiteral("A Volume");
        const qint64 id = m_session->publications().create(volume);
        ContentsDialog dialog(*m_session, id);
        QSignalSpy unavailable(&dialog, &ContentsDialog::contentsUnavailable);
        QVERIFY(unavailable.wait(kLongWaitMs));
    }

    void aCrashReadingTheFileIsReportedAndContained()
    {
        qputenv("PAPERTRAIL_WORKER_TEST_FAULTS", "1");
        const qint64 volume = importVolume(QStringLiteral("reader_doc.pdf"), QStringLiteral("__crash___volume.pdf"), false);
        ContentsDialog dialog(*m_session, volume);
        QSignalSpy unavailable(&dialog, &ContentsDialog::contentsUnavailable);
        const bool reported = unavailable.wait(kLongWaitMs);
        qunsetenv("PAPERTRAIL_WORKER_TEST_FAULTS");
        QVERIFY(reported);
    }

    void addingAnArticleByHandChecksItsPages()
    {
        AddArticleDialog dialog(40);
        dialog.titleEdit()->setText(QStringLiteral("  A Hand-Made   Article "));
        dialog.authorsEdit()->setText(QStringLiteral("Example, Ada; Sample, Omar"));
        dialog.doiEdit()->setText(QStringLiteral(" 10.1000/hand "));
        dialog.firstPageBox()->setValue(12);
        QCOMPARE(dialog.lastPageBox()->value(), 12); // It can't end before it starts.
        dialog.lastPageBox()->setValue(15);
        dialog.firstPageBox()->setValue(20);
        QCOMPARE(dialog.lastPageBox()->value(), 20);
        dialog.lastPageBox()->setValue(25);
        QCOMPARE(dialog.lastPageBox()->maximum(), 40);

        const ArticleDraft draft = dialog.draft();
        QCOMPARE(draft.title, QStringLiteral("A Hand-Made Article"));
        QCOMPARE(draft.firstPage, 20);
        QCOMPARE(draft.lastPage, 25);
        QCOMPARE(draft.authors.size(), 2);
        QCOMPARE(draft.authors.at(1).family, QStringLiteral("Sample"));
        QCOMPARE(draft.doi, QStringLiteral("10.1000/hand"));
    }

    // ---- The window ----

    void anArticleOpensAtItsFirstPage()
    {
        const qint64 volume = importVolume();
        ArticleDraft draft;
        draft.title = QStringLiteral("On The Third Page");
        draft.firstPage = 3;
        draft.lastPage = 3;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();

        ReaderTab *reader = m_window->openPublication(article);
        QVERIFY(reader != nullptr);
        QCOMPARE(reader->publicationId(), article);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->currentPage(), 2, kLongWaitMs);

        // The volume itself opens from the start, in a tab of its own.
        ReaderTab *volumeReader = m_window->openPublication(volume);
        QVERIFY(volumeReader != reader);
        QTRY_VERIFY_WITH_TIMEOUT(volumeReader->pageView()->pageCount() > 0, kLongWaitMs);
        QTest::qWait(300);
        QCOMPARE(volumeReader->pageView()->currentPage(), 0);
    }

    void theDetailsListAVolumesArticlesAndLeadBackFromThem()
    {
        const qint64 volume = importVolume();
        ArticleDraft first;
        first.title = QStringLiteral("Later Article");
        first.firstPage = 3;
        first.lastPage = 3;
        ArticleDraft second;
        second.title = QStringLiteral("Earlier Article");
        second.firstPage = 1;
        second.lastPage = 2;
        const QList<qint64> ids = createArticles(m_session->database(), volume, {first, second}, false);

        DetailsPanel *details = m_window->details();
        details->showPublication(volume);
        QVERIFY(details->articlesSection()->isVisibleTo(details));
        QListWidget *list = details->articleList();
        QCOMPARE(list->count(), 2);
        QVERIFY(list->item(0)->text().startsWith(QStringLiteral("Earlier Article"))); // In page order.
        QVERIFY(list->item(0)->text().contains(QStringLiteral("1")));
        QCOMPARE(list->item(0)->data(Qt::UserRole).toLongLong(), ids.at(1));
        QVERIFY(!details->partOfLabel()->isVisibleTo(details));

        // Opening one from the list.
        emit list->itemActivated(list->item(0));
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<ReaderTab *>(m_window->documentTabs()->currentWidget()) != nullptr, kLongWaitMs);
        QCOMPARE(qobject_cast<ReaderTab *>(m_window->documentTabs()->currentWidget())->publicationId(), ids.at(1));
        m_window->documentTabs()->setCurrentIndex(0);

        // From an article, its volume is a click away.
        details->showPublication(ids.at(0));
        QVERIFY(!details->articlesSection()->isVisibleTo(details));
        QVERIFY(details->partOfLabel()->isVisibleTo(details));
        QVERIFY(details->partOfLabel()->text().contains(m_session->publications().get(volume)->title));
        QCOMPARE(details->partOfLabel()->textFormat(), Qt::PlainText);
        details->showVolumeButton()->click();
        QCOMPARE(details->currentId().value(), volume);
    }

    void onlyThingsThatCanHoldArticlesOfferThem()
    {
        Publication thesis;
        thesis.type = PublicationType::Thesis;
        thesis.title = QStringLiteral("A Thesis");
        const qint64 id = m_session->publications().create(thesis);
        openWindow();
        m_window->details()->showPublication(id);
        QVERIFY(!m_window->details()->articlesSection()->isVisibleTo(m_window->details()));
    }

    void anArticleOnItsOwnShowsOnlyItsPages()
    {
        const qint64 volume = importVolume();
        ArticleDraft draft;
        draft.title = QStringLiteral("Pages Three And Four");
        draft.firstPage = 3;
        draft.lastPage = 4;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();

        ReaderTab *reader = m_window->openPublication(article);
        QVERIFY(reader != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(reader->isReady(), kLongWaitMs);
        PageView *view = reader->pageView();
        QTRY_VERIFY_WITH_TIMEOUT(view->hasPageRange(), kLongWaitMs);
        QCOMPARE(view->firstShownPage(), 2);
        QCOMPARE(view->lastShownPage(), 3);
        QCOMPARE(view->currentPage(), 2);
        QCOMPARE(reader->pageBox()->text(), QStringLiteral("1"));
        QVERIFY(view->pageRect(1).isNull());
        QVERIFY(view->pageRect(4).isNull());

        // It can't be scrolled or paged out of.
        view->goToPage(0);
        QCOMPARE(view->currentPage(), 2);
        view->goToPage(5);
        QCOMPARE(view->topPage(), 3);
        view->nextPage();
        QCOMPARE(view->topPage(), 3);

        // Its contents and thumbnails only show its own pages.
        QTRY_VERIFY_WITH_TIMEOUT(reader->contents()->topLevelItemCount() > 0, kLongWaitMs);
        QVERIFY(reader->thumbnails()->isRowHidden(0));
        QVERIFY(!reader->thumbnails()->isRowHidden(2));

        // The whole issue is a click away, and back.
        QToolButton *whole = reader->wholeFileButton();
        QVERIFY(whole->isVisibleTo(reader));
        QCOMPARE(whole->text(), QStringLiteral("Whole issue"));
        whole->click();
        QVERIFY(!reader->isArticleOnly());
        QVERIFY(!view->hasPageRange());
        QVERIFY(!reader->thumbnails()->isRowHidden(0));
        view->goToPage(0);
        QCOMPARE(view->currentPage(), 0);
        whole->click();
        QVERIFY(view->hasPageRange());
        QCOMPARE(view->currentPage(), 2);

        // The issue itself has no such button.
        ReaderTab *volumeReader = m_window->openPublication(volume);
        QTRY_VERIFY_WITH_TIMEOUT(volumeReader->isReady(), kLongWaitMs);
        QVERIFY(!volumeReader->wholeFileButton()->isVisibleTo(volumeReader));
        QVERIFY(!volumeReader->pageView()->hasPageRange());
    }

    void anArticleFromItsIssuesListShowsTheWholeIssue()
    {
        const qint64 volume = importVolume();
        ArticleDraft draft;
        draft.title = QStringLiteral("Page Three");
        draft.firstPage = 3;
        draft.lastPage = 3;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();

        DetailsPanel *details = m_window->details();
        details->showPublication(volume);
        emit details->articleList()->itemActivated(details->articleList()->item(0));
        ReaderTab *reader = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((reader = qobject_cast<ReaderTab *>(m_window->documentTabs()->currentWidget())) != nullptr,
                                 kLongWaitMs);
        QCOMPARE(reader->publicationId(), article);
        QTRY_VERIFY_WITH_TIMEOUT(reader->isReady(), kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->currentPage(), 2, kLongWaitMs);
        QVERIFY(!reader->isArticleOnly());
        QVERIFY(!reader->pageView()->hasPageRange());
        QVERIFY(reader->wholeFileButton()->isChecked());

        // Opened on its own from the library, the same tab then shows only the article.
        QCOMPARE(m_window->openPublication(article), reader);
        QVERIFY(reader->isArticleOnly());
        QVERIFY(reader->pageView()->hasPageRange());
    }

    void numberedContentsShowTheirArticlesTitles()
    {
        const qint64 volume = importVolume(QStringLiteral("numbered_issue.pdf"));
        ArticleDraft draft;
        draft.title = QStringLiteral("Named In The Library");
        draft.firstPage = 2;
        draft.lastPage = 3;
        createArticles(m_session->database(), volume, {draft}, false);

        ReaderTab *reader = m_window->openPublication(volume);
        QTRY_COMPARE_WITH_TIMEOUT(reader->contents()->topLevelItemCount(), 3, kLongWaitMs);
        QCOMPARE(reader->contents()->topLevelItem(0)->text(0), QStringLiteral("01"));
        QCOMPARE(reader->contents()->topLevelItem(1)->text(0), QStringLiteral("Named In The Library"));
        QCOMPARE(reader->contents()->topLevelItem(2)->text(0), QStringLiteral("03"));
    }

    void longContentsTitlesWrapToThePanelsWidth()
    {
        const qint64 volume = importVolume(QStringLiteral("numbered_issue.pdf"));
        ArticleDraft draft;
        draft.title = QStringLiteral("A Deliberately Long Made-Up Title About Lattice Vibrations in Pretend Crystals "
                                     "That Cannot Possibly Fit on One Line of a Narrow Panel");
        draft.firstPage = 2;
        draft.lastPage = 3;
        createArticles(m_session->database(), volume, {draft}, false);

        m_window->resize(1200, 800);
        ReaderTab *reader = m_window->openPublication(volume);
        QTreeWidget *contents = reader->contents();
        QTRY_COMPARE_WITH_TIMEOUT(contents->topLevelItemCount(), 3, kLongWaitMs);
        reader->setSidePanelVisible(true);
        QTRY_VERIFY(contents->isVisible());
        QTest::qWait(100);

        // The long title takes several lines, and nothing needs scrolling sideways to read.
        const QRect shortRow = contents->visualItemRect(contents->topLevelItem(0));
        QRect longRow = contents->visualItemRect(contents->topLevelItem(1));
        QVERIFY2(longRow.height() >= 2 * shortRow.height() - 8, qPrintable(QStringLiteral("%1 and %2").arg(longRow.height()).arg(shortRow.height())));
        QVERIFY(longRow.right() <= contents->viewport()->width());
        QVERIFY(!contents->horizontalScrollBar()->isVisible());

        // Made wider, it takes fewer lines.
        const int narrowHeight = longRow.height();
        contents->parentWidget()->parentWidget()->findChild<QSplitter *>();
        if (auto *splitter = qobject_cast<QSplitter *>(contents->parentWidget()->parentWidget())) {
            splitter->setSizes({700, 500});
            QTRY_VERIFY(contents->visualItemRect(contents->topLevelItem(1)).height() < narrowHeight);
        }

        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty()) {
            if (auto *splitter = qobject_cast<QSplitter *>(contents->parentWidget()->parentWidget()))
                splitter->setSizes({230, 970});
            QTest::qWait(200);
            reader->grab().save(QDir(pictures).filePath(QStringLiteral("contents-wrapping.png")));
        }
    }

    void anArticleHasACoverOfItsOwnFirstPage()
    {
        const qint64 volume = importVolume();
        ArticleDraft draft;
        draft.title = QStringLiteral("Page Four");
        draft.firstPage = 4;
        draft.lastPage = 4;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();
        const QString sha = m_session->files().filesFor(volume).first().sha256;
        QVERIFY(!sha.isEmpty());

        // Listed, an article's cover is its own first page, and the issue's is still the issue's.
        PublicationFilter filter;
        const QList<PublicationSummary> rows = m_session->query().page(filter, {}, 0, 10);
        QCOMPARE(rows.size(), 2);
        for (const PublicationSummary &row : rows)
            QCOMPARE(row.coverSha, row.id == article ? articleCoverKey(sha, 4) : sha);
        QCOMPARE(coverKeyHash(articleCoverKey(sha, 4)), sha);
        QCOMPARE(coverKeyPage(articleCoverKey(sha, 4)), 4);
        QCOMPARE(coverKeyPage(sha), 0);

        // It's made when first shown.
        const QString issueCover = import::coverPathFor(m_session->paths().cacheDirectory, sha);
        const QString articleCover = import::coverPathFor(m_session->paths().cacheDirectory, articleCoverKey(sha, 4));
        QVERIFY(articleCover != issueCover);
        QVERIFY(QFile::exists(issueCover));
        const QByteArray issueBefore = hashOf(issueCover);
        const QByteArray fileBefore = hashOf(volumePath());
        m_window->details()->showPublication(article);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(articleCover), kLongWaitMs);
        QCOMPARE(hashOf(issueCover), issueBefore);
        QVERIFY(hashOf(articleCover) != issueBefore);
        QCOMPARE(hashOf(volumePath()), fileBefore);
    }

    void typingASearchWhileReadingShowsTheResults()
    {
        const qint64 volume = importVolume();
        QVERIFY(m_window->openPublication(volume) != nullptr);
        QVERIFY(m_window->documentTabs()->currentIndex() != 0);

        // Only typing does it, not a search set some other way.
        m_window->searchBox()->setText(QStringLiteral("set"));
        QVERIFY(m_window->documentTabs()->currentIndex() != 0);
        m_window->searchBox()->clear();
        QTest::keyClicks(m_window->searchBox(), QStringLiteral("chapter"));
        QCOMPARE(m_window->documentTabs()->currentIndex(), 0);
    }

    void documentTabsCanBeReorderedButTheLibraryStaysFirst()
    {
        const qint64 volume = importVolume();
        ArticleDraft first;
        first.title = QStringLiteral("First Tab");
        first.firstPage = 1;
        first.lastPage = 1;
        ArticleDraft second;
        second.title = QStringLiteral("Second Tab");
        second.firstPage = 2;
        second.lastPage = 2;
        const QList<qint64> ids = createArticles(m_session->database(), volume, {first, second}, false);
        QVERIFY(m_window->openPublication(ids.at(0)) != nullptr);
        QVERIFY(m_window->openPublication(ids.at(1)) != nullptr);

        QTabWidget *tabs = m_window->documentTabs();
        QVERIFY(tabs->isMovable());
        QWidget *library = tabs->widget(0);
        tabs->tabBar()->moveTab(2, 1);
        QTest::qWait(50);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Second Tab"));
        QCOMPARE(tabs->widget(0), library);

        // Dragging anything in front of the library puts the library back first.
        tabs->tabBar()->moveTab(2, 0);
        QTRY_COMPARE(tabs->widget(0), library);
        QCOMPARE(tabs->tabText(1), QStringLiteral("First Tab"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Second Tab"));
    }

    void deletingAnArticleLeavesTheVolumeAndAsksFirst()
    {
        const qint64 volume = importVolume();
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        draft.firstPage = 2;
        draft.lastPage = 2;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();
        QVERIFY(m_window->openPublication(article) != nullptr);
        m_window->details()->showPublication(article);

        m_boxWatcher->stop();
        QTimer::singleShot(100, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Cancel)->click();
        });
        m_window->askToDeleteArticle(article);
        QVERIFY(m_session->publications().get(article).has_value());

        QTimer::singleShot(100, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Yes)->click();
        });
        m_window->askToDeleteArticle(article);
        m_boxWatcher->start(100);

        QVERIFY(!m_session->publications().get(article).has_value());
        QVERIFY(m_session->publications().get(volume).has_value());
        QCOMPARE(m_window->documentTabs()->count(), 1); // Its reader closed, and saved nothing.
        QVERIFY(!m_window->details()->currentId().has_value());
        QVERIFY(QFile::exists(volumePath())); // Its file is the volume's, and isn't touched.
    }

    void removingAVolumesFolderTakesItsArticlesToo()
    {
        const qint64 volume = importVolume();
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();
        const qint64 root = m_session->files().roots().first().id;
        m_window->removeRootKeepingDetails(root);
        QVERIFY(m_session->files().hiddenPublications().contains(article));
        QCOMPARE(m_session->query().count({}), qint64(0));
    }

private:
    QString volumePath() const { return QDir(m_dir->filePath(QStringLiteral("docs"))).filePath(m_fileName); }

    // Imports a sample through the real import and the window, and makes it a journal volume.
    qint64 importVolume(const QString &name = QStringLiteral("reader_doc.pdf"), const QString &as = QString(), bool expectReadable = true)
    {
        m_fileName = as.isEmpty() ? name : as;
        QDir().mkpath(m_dir->filePath(QStringLiteral("docs")));
        QFile::copy(sample(name), volumePath());
        openWindow();
        m_window->addFolder(m_dir->filePath(QStringLiteral("docs")));
        QTest::qWait(100);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kLongWaitMs && (m_session->query().count({}) < 1 || m_window->importThread()->isBusy()))
            QTest::qWait(50);
        Q_UNUSED(expectReadable)
        const QList<qint64> ids = m_session->query().ids({}, {});
        if (ids.isEmpty())
            return 0;
        Publication volume = *m_session->publications().get(ids.first());
        volume.type = PublicationType::JournalIssue;
        m_session->publications().update(volume);
        return volume.id;
    }

    void openWindow()
    {
        if (m_window)
            return;
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->show();
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
    QString m_fileName;
};

QTEST_MAIN(VolumesTest)
#include "test_volumes.moc"
