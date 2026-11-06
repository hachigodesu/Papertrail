// Tests of the library window, working on real libraries and the real worker, with no screen.

#include "core/model/Publication.h"
#include "ui/BulkChanges.h"
#include "ui/DetailsPanel.h"
#include "ui/FilterSidebar.h"
#include "ui/FolderWatcher.h"
#include "ui/ImportThread.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/MetadataEditor.h"
#include "ui/PublicationListModel.h"
#include "import/ImportService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

using namespace papertrail;
using namespace papertrail::core;
using namespace papertrail::ui;

namespace {

constexpr int kLongWaitMs = 60000;

QMap<QString, QByteArray> hashTree(const QString &folder)
{
    QMap<QString, QByteArray> hashes;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        hashes.insert(path, hash.result());
    }
    return hashes;
}

} // namespace

class LibraryUiTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("library.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        paths.workerPath = QStringLiteral(PAPERTRAIL_WORKER_PATH);
        paths.settleMs = 0;
        m_session = std::make_unique<LibrarySession>(paths);
    }

    void cleanup()
    {
        m_window.reset();
        m_session.reset();
        m_dir.reset();
    }

    // ---- The list ----

    void modelFetchesPagesOnlyWhenAsked()
    {
        addMany(1000);
        PublicationListModel model(m_session->paths().database);
        QSignalSpy results(&model, &PublicationListModel::resultsChanged);
        model.setQuery({}, {});
        QVERIFY(results.wait(kLongWaitMs));
        QCOMPARE(model.rowCount(), 1000);
        QVERIFY(model.idAt(0).has_value());

        // Far from the first page, it isn't known until it's been fetched.
        QVERIFY(!model.idAt(950).has_value());
        QVERIFY(!model.data(model.index(950, 0), PublicationListModel::IsLoadedRole).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(model.idAt(950).has_value(), kLongWaitMs);
        QCOMPARE(model.data(model.index(950, 0), PublicationListModel::TitleRole).toString(),
                 QStringLiteral("Paper %1").arg(950, 5, 10, QLatin1Char('0')));
    }

    void refreshKeepsThePlaceAndAddsRows()
    {
        addMany(150);
        PublicationListModel model(m_session->paths().database);
        QSignalSpy results(&model, &PublicationListModel::resultsChanged);
        model.setQuery({}, {});
        QVERIFY(results.wait(kLongWaitMs));

        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        addMany(10, 500);
        model.refresh();
        QVERIFY(results.wait(kLongWaitMs));
        QCOMPARE(model.rowCount(), 160);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(inserted.size(), 1);

        // A new filter is a new set of results, and does start again.
        PublicationFilter filter;
        filter.searchText = QStringLiteral("00500");
        model.setQuery(filter, {});
        QVERIFY(results.wait(kLongWaitMs));
        QCOMPARE(resets.size(), 1);
        QCOMPARE(model.rowCount(), 1);
    }

    void onlyTheNewestQueryCounts()
    {
        addMany(50);
        PublicationListModel model(m_session->paths().database);
        PublicationFilter noResults;
        noResults.searchText = QStringLiteral("nothingmatchesthisword");
        model.setQuery(noResults, {});
        model.setQuery({}, {});
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 50, kLongWaitMs);
        QTest::qWait(200);
        QCOMPARE(model.rowCount(), 50);
    }

    // ---- Tabs, search and filters ----

    void tabsShowTheirTypes()
    {
        addPublication(QStringLiteral("An Article"), PublicationType::JournalArticle);
        addPublication(QStringLiteral("A Thesis"), PublicationType::Thesis);
        addPublication(QStringLiteral("A Dissertation"), PublicationType::Dissertation);
        addPublication(QStringLiteral("A Textbook"), PublicationType::Textbook);
        addPublication(QStringLiteral("A Preprint"), PublicationType::Preprint);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 5, kLongWaitMs);

        // A preprint of an article is kept apart from the published articles.
        m_window->setTab(LibraryTab::JournalArticles);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QCOMPARE(title(0), QStringLiteral("An Article"));
        m_window->setTab(LibraryTab::Preprints);
        QTRY_COMPARE_WITH_TIMEOUT(title_(0), QStringLiteral("A Preprint"), kLongWaitMs);
        QCOMPARE(rows(), 1);
        m_window->setTab(LibraryTab::Other);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);

        m_window->setTab(LibraryTab::ThesesAndDissertations);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        m_window->setTab(LibraryTab::Textbooks);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QCOMPARE(title(0), QStringLiteral("A Textbook"));
        m_window->setTab(LibraryTab::Standards);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        m_window->setTab(LibraryTab::All);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 5, kLongWaitMs);
    }

    void searchBoxFiltersAndSortsByBestMatch()
    {
        addPublication(QStringLiteral("Beams in Tension"), PublicationType::JournalArticle, QStringLiteral("Hooke"));
        addPublication(QStringLiteral("Columns in Compression"), PublicationType::JournalArticle, QStringLiteral("Euler"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QCOMPARE(m_window->sortBox()->currentData().toInt(), int(SortField::Title));

        m_window->searchBox()->setText(QStringLiteral("author:euler"));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QCOMPARE(title(0), QStringLiteral("Columns in Compression"));
        QCOMPARE(m_window->sortBox()->currentData().toInt(), int(SortField::Relevance));

        m_window->searchBox()->clear();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QCOMPARE(m_window->sortBox()->currentData().toInt(), int(SortField::Title));
    }

    void sidebarCountsAndNarrows()
    {
        addPublication(QStringLiteral("One"), PublicationType::JournalArticle, QStringLiteral("Hooke"), 2001);
        addPublication(QStringLiteral("Two"), PublicationType::JournalArticle, QStringLiteral("Hooke"), 2010);
        addPublication(QStringLiteral("Three"), PublicationType::JournalArticle, QStringLiteral("Euler"), 2020);
        openWindow();
        QTRY_VERIFY_WITH_TIMEOUT(authorItem(QStringLiteral("Hooke")) != nullptr, kLongWaitMs);

        QTreeWidgetItem *hooke = authorItem(QStringLiteral("Hooke"));
        QCOMPARE(hooke->text(1), QStringLiteral("2"));

        hooke->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);

        // Another author's count still shows what choosing them as well would add.
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(authorItem(QStringLiteral("Euler")) != nullptr, kLongWaitMs);
        QCOMPARE(authorItem(QStringLiteral("Euler"))->text(1), QStringLiteral("1"));

        m_window->sidebar()->clearSelection();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, kLongWaitMs);

        // The year range.
        auto *from = m_window->sidebar()->findChild<QLineEdit *>();
        QVERIFY(from != nullptr);
        from->setText(QStringLiteral("2010"));
        from->setModified(true);
        emit from->editingFinished();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QVERIFY(m_window->sidebar()->hasSelection());
    }

    void chosenValueStaysOnTheListWhenNothingMatchesIt()
    {
        addPublication(QStringLiteral("One"), PublicationType::JournalArticle, QStringLiteral("Hooke"));
        addPublication(QStringLiteral("Two"), PublicationType::Thesis, QStringLiteral("Euler"));
        openWindow();
        QTRY_VERIFY_WITH_TIMEOUT(authorItem(QStringLiteral("Hooke")) != nullptr, kLongWaitMs);

        authorItem(QStringLiteral("Hooke"))->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->setTab(LibraryTab::ThesesAndDissertations);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);

        // Hooke has no theses, but it's still ticked and can be unticked.
        QTRY_COMPARE_WITH_TIMEOUT(authorItem(QStringLiteral("Hooke"))->text(1), QStringLiteral("0"), kLongWaitMs);
        QCOMPARE(authorItem(QStringLiteral("Hooke"))->checkState(0), Qt::Checked);
    }

    void emptyLibraryAndNoMatchesExplainThemselves()
    {
        openWindow();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->model()->hasResults(), kLongWaitMs);
        QVERIFY(!qobject_cast<QAbstractItemView *>(m_window->viewStack()->currentWidget()));

        addPublication(QStringLiteral("One"), PublicationType::JournalArticle);
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QVERIFY(qobject_cast<QAbstractItemView *>(m_window->viewStack()->currentWidget()));

        m_window->searchBox()->setText(QStringLiteral("nothingmatchesthisword"));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        QVERIFY(!qobject_cast<QAbstractItemView *>(m_window->viewStack()->currentWidget()));
    }

    // ---- Selection and changes ----

    void selectingShowsDetails()
    {
        const qint64 id = addPublication(QStringLiteral("Beams"), PublicationType::JournalArticle, QStringLiteral("Hooke"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);

        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));
        QCOMPARE(m_window->details()->currentId().value_or(0), id);
        QCOMPARE(m_window->details()->titleLabel()->text(), QStringLiteral("Beams"));
        QCOMPARE(m_window->details()->authorsLabel()->text(), QStringLiteral("Hooke"));
    }

    void selectionCarriesAcrossViews()
    {
        addPublication(QStringLiteral("Beams"), PublicationType::JournalArticle);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);

        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));
        QCOMPARE(m_window->selectedCount(), 1);
        m_window->setViewMode(MainWindow::ViewMode::List);
        QCOMPARE(m_window->selectedCount(), 1);
        QCOMPARE(m_window->viewStack()->currentWidget(), static_cast<QWidget *>(m_window->listView()));
        QCOMPARE(m_session->settings().value(QStringLiteral("library.view")).toString(), QStringLiteral("list"));
    }

    void changesApplyToEverythingSelected()
    {
        addMany(350); // More than the pages the model has fetched so far.
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 350, kLongWaitMs);

        m_window->gridView()->selectAll();
        QCOMPARE(m_window->selectedCount(), 350);
        QVERIFY(!m_window->details()->currentId().has_value());

        const QList<qint64> ids = m_window->selectedIds();
        QCOMPARE(ids.size(), 350);
        QSignalSpy finished(m_window->bulkChanges(), &BulkChanges::finished);
        m_window->tagSelected(QStringLiteral("Reading > Soon"));
        m_window->setSelectedReadStatus(ReadStatus::Reading);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, kLongWaitMs);

        const QList<Tag> tags = m_session->tags().tagsFor(ids.last());
        QCOMPARE(tags.size(), 1);
        QCOMPARE(m_session->tags().path(tags.first().id), QStringLiteral("Reading > Soon"));
        QCOMPARE(m_session->publications().get(ids.last())->readStatus, ReadStatus::Reading);
        QCOMPARE(m_session->publications().get(ids.first())->readStatus, ReadStatus::Reading);
    }

    void aRangeOfRowsIsTheRightPublications()
    {
        addMany(20);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 20, kLongWaitMs);

        QItemSelectionModel *selection = m_window->gridView()->selectionModel();
        selection->select(QItemSelection(m_window->model()->index(3, 0), m_window->model()->index(5, 0)),
                          QItemSelectionModel::ClearAndSelect);
        selection->select(QItemSelection(m_window->model()->index(10, 0), m_window->model()->index(10, 0)),
                          QItemSelectionModel::Select);
        QCOMPARE(m_window->selectedCount(), 4);

        const QList<qint64> ids = m_window->selectedIds();
        QCOMPARE(ids.size(), 4);
        QCOMPARE(m_session->publications().get(ids.at(0))->title, QStringLiteral("Paper 00003"));
        QCOMPARE(m_session->publications().get(ids.at(3))->title, QStringLiteral("Paper 00010"));
    }

    void selectionIsClearedRatherThanPointingAtTheWrongPublication()
    {
        addPublication(QStringLiteral("M"), PublicationType::JournalArticle);
        addPublication(QStringLiteral("N"), PublicationType::JournalArticle);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);

        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));
        const qint64 id = m_window->details()->currentId().value_or(0);
        QCOMPARE(m_session->publications().get(id)->title, QStringLiteral("M"));

        // Something new sorts in front, so the selected row now holds a different publication.
        addPublication(QStringLiteral("A"), PublicationType::JournalArticle);
        QSignalSpy results(m_window->model(), &PublicationListModel::resultsChanged);
        m_window->model()->refresh();
        QVERIFY(results.wait(kLongWaitMs));
        QCOMPARE(m_window->selectedCount(), 0);
        QCOMPARE(m_window->details()->currentId().value_or(0), id);
    }

    void tagsCanBeAddedAndRemoved()
    {
        const qint64 id = addPublication(QStringLiteral("Beams"), PublicationType::JournalArticle);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));

        m_window->details()->addTag(QStringLiteral("Engineering > Structures"));
        const QList<Tag> tags = m_session->tags().tagsFor(id);
        QCOMPARE(tags.size(), 1);
        QCOMPARE(m_session->tags().path(tags.first().id), QStringLiteral("Engineering > Structures"));

        // It reaches the sidebar, under its parent, with counts for both.
        const QString structures = QString::number(tags.first().id);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->sidebar()->findValue(FilterSidebar::Tags, structures) != nullptr, kLongWaitMs);
        QTreeWidgetItem *item = m_window->sidebar()->findValue(FilterSidebar::Tags, structures);
        QCOMPARE(item->text(0), QStringLiteral("Structures"));
        QCOMPARE(item->text(1), QStringLiteral("1"));
        QCOMPARE(item->parent()->text(0), QStringLiteral("Engineering"));
        QCOMPARE(item->parent()->text(1), QStringLiteral("1"));

        // Choosing the parent finds what's tagged beneath it.
        item->parent()->setCheckState(0, Qt::Checked);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->model()->filter().tagIds.size() == 1, kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);

        m_window->details()->removeTag(tags.first().id);
        QVERIFY(m_session->tags().tagsFor(id).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
    }

    void aDeletedTagStopsFiltering()
    {
        const qint64 id = addPublication(QStringLiteral("Beams"), PublicationType::JournalArticle);
        const qint64 tag = m_session->tags().ensurePath(QStringLiteral("Old"));
        m_session->tags().assign(id, tag);
        openWindow();
        const QString key = QString::number(tag);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->sidebar()->findValue(FilterSidebar::Tags, key) != nullptr, kLongWaitMs);
        m_window->sidebar()->setValueChosen(FilterSidebar::Tags, key, true);
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->filter().tagIds.size(), 1, kLongWaitMs);

        m_session->tags().remove(tag);
        emit m_window->sidebar()->tagsChanged();
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->filter().tagIds.size(), 0, kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
    }

    void statusRatingAndNotesAreSaved()
    {
        const qint64 id = addPublication(QStringLiteral("Beams"), PublicationType::JournalArticle);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));

        QComboBox *status = m_window->details()->readStatusBox();
        status->setCurrentIndex(status->findData(int(ReadStatus::Read)));
        QCOMPARE(m_session->publications().get(id)->readStatus, ReadStatus::Read);

        m_window->details()->notesEdit()->setPlainText(QStringLiteral("Check section 3."));
        m_window->details()->saveNotes();
        QCOMPARE(m_session->publications().get(id)->notes, QStringLiteral("Check section 3."));
    }

    void theCheckboxMarksAPreprint()
    {
        const qint64 id = addPublication(QStringLiteral("Beams"), PublicationType::JournalArticle);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));

        QCheckBox *box = m_window->details()->preprintBox();
        QVERIFY(box->isEnabled());
        QVERIFY(!box->isChecked());
        QVERIFY(box->mapTo(m_window.get(), QPoint()).y() < m_window->details()->notesEdit()->mapTo(m_window.get(), QPoint()).y());

        // Ticking it moves it from the articles to the preprints.
        box->setChecked(true);
        QCOMPARE(m_session->publications().get(id)->type, PublicationType::Preprint);
        QCOMPARE(m_session->publications().provenance(id, QStringLiteral("type")).first().source, QStringLiteral("manual"));
        m_window->setTab(LibraryTab::JournalArticles);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        m_window->setTab(LibraryTab::Preprints);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->gridView()->setCurrentIndex(m_window->model()->index(0, 0));
        QVERIFY(m_window->details()->preprintBox()->isChecked());

        // Unticking puts back what it was.
        m_window->details()->preprintBox()->setChecked(false);
        QCOMPARE(m_session->publications().get(id)->type, PublicationType::JournalArticle);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
    }

    void theCheckboxFollowsThePublicationShownAndChangesNothingByItself()
    {
        const qint64 article = addPublication(QStringLiteral("A"), PublicationType::JournalArticle);
        const qint64 preprint = addPublication(QStringLiteral("B"), PublicationType::Preprint);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);

        m_window->details()->showPublication(preprint);
        QVERIFY(m_window->details()->preprintBox()->isChecked());
        m_window->details()->showPublication(article);
        QVERIFY(!m_window->details()->preprintBox()->isChecked());
        QCOMPARE(m_session->publications().get(preprint)->type, PublicationType::Preprint);
        QCOMPARE(m_session->publications().get(article)->type, PublicationType::JournalArticle);
        QVERIFY(m_session->publications().provenance(article, QStringLiteral("type")).isEmpty());

        // With no earlier type on record, unticking leaves it as Other.
        m_window->details()->showPublication(preprint);
        m_window->details()->preprintBox()->setChecked(false);
        QCOMPARE(m_session->publications().get(preprint)->type, PublicationType::Other);
    }

    void notesAreSavedWhenMovingToAnotherPublication()
    {
        const qint64 first = addPublication(QStringLiteral("A"), PublicationType::JournalArticle);
        const qint64 second = addPublication(QStringLiteral("B"), PublicationType::JournalArticle);
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);

        m_window->details()->showPublication(first);
        m_window->details()->notesEdit()->setPlainText(QStringLiteral("About A."));
        m_window->details()->showPublication(second);
        QCOMPARE(m_session->publications().get(first)->notes, QStringLiteral("About A."));
        QVERIFY(m_session->publications().get(second)->notes.isEmpty());
        QVERIFY(m_window->details()->notesEdit()->toPlainText().isEmpty());
    }

    // ---- Text and links from documents ----

    void metadataIsOnlyEverShownAsText()
    {
        const QString nasty = QStringLiteral("<img src=\"file:///x.png\"><b>Bold</b>");
        Publication p;
        p.type = PublicationType::JournalArticle;
        p.title = nasty;
        p.publisher = nasty;
        p.authors = {{nasty, QString()}};
        p.url = QStringLiteral("file:///etc/passwd");
        const qint64 id = m_session->publications().create(p);

        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->details()->showPublication(id);

        QCOMPARE(m_window->details()->titleLabel()->textFormat(), Qt::PlainText);
        QCOMPARE(m_window->details()->titleLabel()->text(), nasty);
        QCOMPARE(m_window->details()->authorsLabel()->textFormat(), Qt::PlainText);

        // Every value on show is either plain text, or a link whose text was escaped.
        QFormLayout *facts = m_window->details()->facts();
        for (int row = 0; row < facts->rowCount(); ++row) {
            auto *value = qobject_cast<QLabel *>(facts->itemAt(row, QFormLayout::FieldRole)->widget());
            QVERIFY(value != nullptr);
            QCOMPARE(value->textFormat(), Qt::PlainText);
        }
    }

    void onlyWebLinksCanBeFollowed()
    {
        QVERIFY(safeWebLink(QStringLiteral("file:///etc/passwd")).isEmpty());
        QVERIFY(safeWebLink(QStringLiteral("javascript:alert(1)")).isEmpty());
        QVERIFY(safeWebLink(QStringLiteral("ms-msdt:/id")).isEmpty());
        QVERIFY(safeWebLink(QStringLiteral("smb://server/share")).isEmpty());
        QVERIFY(safeWebLink(QStringLiteral("not a link")).isEmpty());
        QVERIFY(safeWebLink(QStringLiteral("https://")).isEmpty());
        QCOMPARE(safeWebLink(QStringLiteral(" https://example.org/a ")).toString(), QStringLiteral("https://example.org/a"));
        QVERIFY(!safeWebLink(QStringLiteral("HTTP://example.org")).isEmpty());

        // A DOI can't move the link off doi.org, or add a query to it.
        const QUrl doi = doiLink(QStringLiteral("10.1000/a?b=c#d"));
        QCOMPARE(doi.host(), QStringLiteral("doi.org"));
        QVERIFY(!doi.hasQuery());
        QVERIFY(!doi.hasFragment());
        QCOMPARE(doiLink(QStringLiteral("@evil.example/x")).host(), QStringLiteral("doi.org"));

        const QString html = linkHtml(QUrl(QStringLiteral("https://example.org/")), QStringLiteral("<b>x</b>"));
        QVERIFY(html.contains(QStringLiteral("&lt;b&gt;x&lt;/b&gt;")));
    }

    // ---- Editing ----

    void editorOffersFieldsThatSuitTheType()
    {
        Publication p;
        p.type = PublicationType::Thesis;
        p.title = QStringLiteral("A Thesis");
        p.reportNumber = QStringLiteral("R-1"); // Not usual for a thesis, but it has a value.

        MetadataEditor editor(p);
        QVERIFY(editor.isFieldShown(QStringLiteral("institution")));
        QVERIFY(editor.isFieldShown(QStringLiteral("advisors")));
        QVERIFY(!editor.isFieldShown(QStringLiteral("standard_number")));
        QVERIFY(editor.isFieldShown(QStringLiteral("report_number")));

        editor.typeBox()->setCurrentIndex(editor.typeBox()->findData(int(PublicationType::Standard)));
        QVERIFY(editor.isFieldShown(QStringLiteral("standard_number")));
        QVERIFY(!editor.isFieldShown(QStringLiteral("institution")));
        QVERIFY(!editor.isFieldShown(QStringLiteral("advisors")));
    }

    void editorKeepsValuesInFieldsThatAreHidden()
    {
        Publication p;
        p.type = PublicationType::Thesis;
        p.title = QStringLiteral("A Thesis");
        MetadataEditor editor(p);
        editor.field(QStringLiteral("institution"))->setText(QStringLiteral("Example University"));
        editor.typeBox()->setCurrentIndex(editor.typeBox()->findData(int(PublicationType::Standard)));
        QCOMPARE(editor.result().institution, QStringLiteral("Example University"));
    }

    void editorNeedsATitle()
    {
        Publication p;
        p.title = QStringLiteral("A Title");
        MetadataEditor editor(p);
        QVERIFY(editor.validate());
        editor.field(QStringLiteral("title"))->setText(QStringLiteral("   "));
        QVERIFY(!editor.validate());
    }

    void editorReadsPeopleAndClearsReview()
    {
        Publication p;
        p.title = QStringLiteral("A Title");
        p.needsReview = true;
        p.year = 2001;
        p.month = 3;
        MetadataEditor editor(p);
        editor.peopleField(QStringLiteral("authors"))->setPlainText(QStringLiteral("Hooke, Robert\n\nLeonhard Euler\n"));

        Publication same = editor.result();
        QCOMPARE(same.authors.size(), 2);
        QCOMPARE(same.authors.at(0).family, QStringLiteral("Hooke"));
        QCOMPARE(same.authors.at(0).given, QStringLiteral("Robert"));
        QCOMPARE(same.authors.at(1).family, QStringLiteral("Euler"));
        QVERIFY(!same.needsReview);
        QCOMPARE(same.month.value_or(0), 3);
    }

    void changingTheYearDropsItsMonth()
    {
        Publication p;
        p.title = QStringLiteral("A Title");
        p.year = 2001;
        p.month = 3;
        MetadataEditor editor(p);
        // Year is the one field without a name to look it up by.
        for (QLineEdit *edit : editor.findChildren<QLineEdit *>()) {
            if (edit->text() == QStringLiteral("2001"))
                edit->setText(QStringLiteral("2002"));
        }
        QCOMPARE(editor.result().year.value_or(0), 2002);
        QVERIFY(!editor.result().month.has_value());
    }

    void savedEditsCanBeTracedBack()
    {
        const qint64 id = addPublication(QStringLiteral("Old Title"), PublicationType::JournalArticle, QStringLiteral("Hooke"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);

        Publication p = *m_session->publications().get(id);
        p.title = QStringLiteral("New Title");
        p.authors = {{QStringLiteral("Euler"), QString()}};
        m_window->saveEdit(p);

        QCOMPARE(m_session->publications().get(id)->title, QStringLiteral("New Title"));
        const QList<ProvenanceEntry> title = m_session->publications().provenance(id, QStringLiteral("title"));
        QCOMPARE(title.size(), 1);
        QCOMPARE(title.first().source, QStringLiteral("manual"));
        QCOMPARE(title.first().previousValue, QStringLiteral("Old Title"));
        QCOMPARE(m_session->publications().provenance(id, QStringLiteral("authors")).first().previousValue,
                 QStringLiteral("Hooke"));
        QVERIFY(m_session->publications().provenance(id, QStringLiteral("doi")).isEmpty());

        // The new title reaches the list, and a search can find it.
        QTRY_COMPARE_WITH_TIMEOUT(title_(0), QStringLiteral("New Title"), kLongWaitMs);
        m_window->searchBox()->setText(QStringLiteral("author:euler"));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
    }

    // ---- Importing ----

    void importingFromTheWindowLeavesFilesAlone()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf"), QStringLiteral("book.epub"),
                                            QStringLiteral("scanned.djvu"), QStringLiteral("garbage.pdf"),
                                            QStringLiteral("ignored_notes.txt")});
        const auto before = hashTree(folder);

        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 4, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        QCOMPARE(hashTree(folder), before);
        QCOMPARE(m_session->files().roots().size(), 1);

        // Unreadable, but listed, and easy to find.
        QTRY_VERIFY_WITH_TIMEOUT(m_window->sidebar()->findValue(FilterSidebar::Status, QStringLiteral("unreadable")) != nullptr,
                                 kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            m_window->sidebar()->findValue(FilterSidebar::Status, QStringLiteral("unreadable"))->text(1), QStringLiteral("1"),
            kLongWaitMs);

        // Covers are on disk for the files that have them.
        bool foundCover = false;
        for (int row = 0; row < rows(); ++row) {
            const QString sha = m_window->model()->data(m_window->model()->index(row, 0),
                                                        PublicationListModel::CoverShaRole).toString();
            foundCover = foundCover || QFile::exists(import::coverPathFor(m_session->paths().cacheDirectory, sha));
        }
        QVERIFY(foundCover);
    }

    void aFolderInsideTheLibraryIsNotAddedTwice()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        QDir(folder).mkpath(QStringLiteral("inner"));
        QFile::copy(sample(QStringLiteral("book.epub")), QDir(folder).filePath(QStringLiteral("inner/book.epub")));

        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        m_window->addFolder(QDir(folder).filePath(QStringLiteral("inner")));
        QCOMPARE(m_session->files().roots().size(), 1);
    }

    void aParentOfALibraryFolderDoesNotSeeItsFilesTwice()
    {
        const QString parent = m_dir->filePath(QStringLiteral("parent"));
        QDir().mkpath(QDir(parent).filePath(QStringLiteral("inner")));
        QFile::copy(sample(QStringLiteral("article.pdf")), QDir(parent).filePath(QStringLiteral("inner/article.pdf")));
        QFile::copy(sample(QStringLiteral("book.epub")), QDir(parent).filePath(QStringLiteral("book.epub")));

        openWindow();
        m_window->addFolder(QDir(parent).filePath(QStringLiteral("inner")));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        m_window->addFolder(parent);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        QCOMPARE(m_session->files().roots().size(), 2);
        const QHash<qint64, qint64> counts = m_session->files().fileCountsByRoot();
        for (const qint64 count : counts)
            QCOMPARE(count, 1);
    }

    void aWatchedFolderPicksUpNewAndRemovedFiles()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->folderWatcher()->setDelays(200, 500);
        m_window->folderWatcher()->setPollDelays(300, 0);
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->folderWatcher()->watchedCount() == 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        QVERIFY(QFile::copy(sample(QStringLiteral("book.epub")), QDir(folder).filePath(QStringLiteral("book.epub"))));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        // A new subfolder gets watched too, and what's put in it is found.
        QVERIFY(QDir(folder).mkpath(QStringLiteral("sub")));
        QTRY_COMPARE_WITH_TIMEOUT(m_window->folderWatcher()->watchedCount(), 2, kLongWaitMs);
        QVERIFY(QFile::copy(sample(QStringLiteral("report.pdf")), QDir(folder).filePath(QStringLiteral("sub/report.pdf"))));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        // A removed file keeps its publication, and shows as missing.
        QVERIFY(QFile::remove(QDir(folder).filePath(QStringLiteral("book.epub"))));
        QTRY_VERIFY_WITH_TIMEOUT(m_window->sidebar()->findValue(FilterSidebar::Status, QStringLiteral("missing")) != nullptr
                                     && !m_window->sidebar()->findValue(FilterSidebar::Status, QStringLiteral("missing"))->isHidden(),
                                 kLongWaitMs);
        QCOMPARE(rows(), 3);
    }

    void turningOffWatchingStopsIt()
    {
        FolderWatcher watcher;
        const QString folder = m_dir->path();
        watcher.setFolders(1, {folder});
        QCOMPARE(watcher.watchedCount(), 1);
        watcher.setEnabled(false);
        QCOMPARE(watcher.watchedCount(), 0);
        watcher.setEnabled(true);
        QCOMPARE(watcher.watchedCount(), 1);
        watcher.removeRoot(1);
        QCOMPARE(watcher.watchedCount(), 0);
    }

    void stoppingAnImportCarriesOnNextTime()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf"), QStringLiteral("book.epub"),
                                            QStringLiteral("report.pdf"), QStringLiteral("thesis.pdf"),
                                            QStringLiteral("standard.pdf"), QStringLiteral("preprint.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTest::qWait(50);
        m_window->importThread()->requestStop();
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        m_window.reset();

        // Whatever happened before the stop, the next start gets through all of it.
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 6, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        // And nothing was marked as unreadable because of being interrupted.
        QVERIFY(!m_session->query().flagCounts({}).unreadable);
    }

    void aChangeInAFolderDoesNotUndoStopping()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->folderWatcher()->setDelays(100, 100);
        m_window->folderWatcher()->setPollDelays(100, 0);
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->folderWatcher()->watchedCount() == 1, kLongWaitMs);

        m_window->stopImport();
        QVERIFY(QFile::copy(sample(QStringLiteral("book.epub")), QDir(folder).filePath(QStringLiteral("book.epub"))));
        QTest::qWait(1000);
        QCOMPARE(rows(), 1);

        // Asking for it by hand starts things again.
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
    }

    void aWatchedFolderCanStillBeRenamed()
    {
        const QString parent = m_dir->filePath(QStringLiteral("watched"));
        const QString child = QDir(parent).filePath(QStringLiteral("sub"));
        QVERIFY(QDir().mkpath(child));

        FolderWatcher watcher;
        watcher.setFolders(1, {parent, child});
        QCOMPARE(watcher.watchedCount(), 2);

        // Watching must never get in the way of people managing their own folders.
        QVERIFY(QDir(parent).rename(QStringLiteral("sub"), QStringLiteral("renamed")));
        QVERIFY(QDir(m_dir->path()).rename(QStringLiteral("watched"), QStringLiteral("watched2")));
        QVERIFY(QDir(m_dir->filePath(QStringLiteral("watched2"))).removeRecursively());
    }

    void comingBackToTheWindowLooksForChanges()
    {
        FolderWatcher watcher;
        watcher.setFolders(1, {m_dir->path()});
        QSignalSpy due(&watcher, &FolderWatcher::pollDue);
        watcher.setPollDelays(60000, 0);
        watcher.windowActivated();
        QCOMPARE(due.size(), watcher.isPolling() ? 1 : 0);

        // Not over and over, though.
        watcher.setPollDelays(60000, 60000);
        watcher.windowActivated();
        QCOMPARE(due.size(), watcher.isPolling() ? 1 : 0);
    }

    void aMissingWorkerDoesNotMakeFilesLookUnreadable()
    {
        m_session.reset();
        LibraryPaths paths;
        paths.database = m_dir->filePath(QStringLiteral("library.db"));
        paths.cacheDirectory = m_dir->filePath(QStringLiteral("cache"));
        paths.workerPath = m_dir->filePath(QStringLiteral("no-such-worker"));
        paths.settleMs = 0;
        m_session = std::make_unique<LibrarySession>(paths);

        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        QSignalSpy failed(m_window->importThread(), &ImportThread::failed);
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        QCOMPARE(m_session->query().flagCounts({}).unreadable, 0);
        QCOMPARE(m_session->query().count({}), 0);
    }

    // ---- The library as a whole ----

    void removingAFolderKeepsItsPublicationsAndTheirTags()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        const qint64 id = m_window->model()->idAt(0).value();
        m_session->tags().assign(id, m_session->tags().ensurePath(QStringLiteral("Keep")));
        m_session->files().removeRoot(m_session->files().roots().first().id);

        QCOMPARE(m_session->query().count({}), 1);
        QCOMPARE(m_session->tags().tagsFor(id).size(), 1);
        QCOMPARE(m_session->query().flagCounts({}).noFile, 1);
        QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("article.pdf"))));
    }

    void onlyOneCopyCanHaveALibraryOpen()
    {
        bool refused = false;
        try {
            LibrarySession second(m_session->paths());
        } catch (const LibraryInUse &) {
            refused = true;
        }
        QVERIFY(refused);
    }

    void windowLayoutIsRememberedButNotExported()
    {
        openWindow();
        m_window->close();
        QVERIFY(!m_session->settings().value(QStringLiteral("window.geometry")).toString().isEmpty());
        QVERIFY(!m_session->settings().exportAll().contains(QStringLiteral("window.geometry")));
    }

private:
    int rows() const { return m_window->model()->rowCount(); }

    QString title(int row) const
    {
        return m_window->model()->data(m_window->model()->index(row, 0), PublicationListModel::TitleRole).toString();
    }

    // For QTRY comparisons, where a row may not have been fetched yet.
    QString title_(int row) const { return rows() > row ? title(row) : QString(); }

    QTreeWidgetItem *authorItem(const QString &name) const
    {
        QTreeWidgetItem *section = m_window->sidebar()->sectionItem(FilterSidebar::Authors);
        for (int i = 0; i < section->childCount(); ++i) {
            if (section->child(i)->text(0) == name)
                return section->child(i);
        }
        return nullptr;
    }

    qint64 addPublication(const QString &title, PublicationType type, const QString &author = QString(),
                          std::optional<int> year = std::nullopt)
    {
        Publication p;
        p.title = title;
        p.type = type;
        p.year = year;
        if (!author.isEmpty())
            p.authors = {{author, QString()}};
        return m_session->publications().create(p);
    }

    void addMany(int count, int firstNumber = 0)
    {
        Transaction transaction(m_session->database());
        for (int i = 0; i < count; ++i) {
            addPublication(QStringLiteral("Paper %1").arg(firstNumber + i, 5, 10, QLatin1Char('0')),
                           PublicationType::JournalArticle);
        }
        transaction.commit();
    }

    void openWindow()
    {
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->show();
    }

    static QString sample(const QString &name)
    {
        return QDir(QStringLiteral(PAPERTRAIL_TEST_SAMPLES)).filePath(name);
    }

    QString copySamples(const QStringList &names)
    {
        const QString folder = m_dir->filePath(QStringLiteral("docs"));
        QDir().mkpath(folder);
        for (const QString &name : names)
            QFile::copy(sample(name), QDir(folder).filePath(name));
        return folder;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
};

QTEST_MAIN(LibraryUiTest)
#include "test_library_ui.moc"
