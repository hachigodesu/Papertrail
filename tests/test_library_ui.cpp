// Tests of the library window, working on real libraries and the real worker, with no screen.

#include "core/model/Publication.h"
#include "ui/BulkChanges.h"
#include "ui/CitationActions.h"
#include "ui/DetailsPanel.h"
#include "ui/FilterSidebar.h"
#include "ui/FolderWatcher.h"
#include "ui/ImportThread.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"
#include "ui/LibraryFoldersDialog.h"
#include "ui/MainWindow.h"
#include "ui/MetadataEditor.h"
#include "ui/PublicationListModel.h"
#include "import/ImportService.h"

#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
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
        // An error box would wait for a click forever, with no one here to click it. A test that
        // brings one up fails, and says what it said.
        m_unexpectedBoxes.clear();
        m_boxWatcher = std::make_unique<QTimer>();
        connect(m_boxWatcher.get(), &QTimer::timeout, this, [this] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                m_unexpectedBoxes << box->text();
                box->close();
            }
        });
        m_boxWatcher->start(100);

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
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
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

    void removingAFolderTakesItsPublicationsOutOfTheLibrary()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        const qint64 id = m_window->model()->idAt(0).value();
        m_session->tags().assign(id, m_session->tags().ensurePath(QStringLiteral("Keep")));
        m_window->details()->showPublication(id);
        const qint64 root = m_session->files().roots().first().id;

        m_window->removeRootKeepingDetails(root);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        QCOMPARE(m_session->query().count({}), 0);
        QCOMPARE(m_session->query().flagCounts({}).noFile, 0);
        QVERIFY(m_session->files().roots().isEmpty());
        QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("article.pdf"))));
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->details()->currentId().has_value(), kLongWaitMs);

        // Adding it again brings back the same publication, tags and all.
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        QCOMPARE(m_window->model()->idAt(0).value(), id);
        QCOMPARE(m_session->tags().tagsFor(id).size(), 1);
        QCOMPARE(m_session->files().hiddenCount(), qint64(0));
        QCOMPARE(m_session->files().roots().size(), 1);
    }

    void removingAFolderCanDeleteItsDetailsToo()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        const qint64 id = m_window->model()->idAt(0).value();
        m_session->tags().assign(id, m_session->tags().ensurePath(QStringLiteral("Keep")));
        QVERIFY(m_window->openPublication(id) != nullptr);
        QCOMPARE(m_window->documentTabs()->count(), 2);

        m_window->removeRootAndDetails(m_session->files().roots().first().id);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        QCOMPARE(m_window->documentTabs()->count(), 1); // Its reader closed first, and saved nothing.
        QTRY_VERIFY_WITH_TIMEOUT(!m_session->publications().get(id).has_value(), kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->files().hiddenCount(), qint64(0), kLongWaitMs);
        QVERIFY(m_session->files().removedRoots().isEmpty());
        QVERIFY(QFile::exists(QDir(folder).filePath(QStringLiteral("article.pdf"))));

        // The file's still there to be added again, as something new.
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        QVERIFY(m_session->tags().tagsFor(m_window->model()->idAt(0).value()).isEmpty());
    }

    void whatsBeenKeptCanBeDeletedLater()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        const qint64 id = m_window->model()->idAt(0).value();
        m_window->removeRootKeepingDetails(m_session->files().roots().first().id);

        LibraryFoldersDialog dialog(*m_session);
        const auto hasNote = [&] {
            for (const QLabel *label : dialog.findChildren<QLabel *>()) {
                if (label->isVisibleTo(&dialog) && label->text().contains(QStringLiteral("1 publication from removed")))
                    return true;
            }
            return false;
        };
        QVERIFY(hasNote());

        m_window->forgetHiddenPublications();
        QTRY_VERIFY_WITH_TIMEOUT(!m_session->publications().get(id).has_value(), kLongWaitMs);
        QVERIFY(m_session->files().removedRoots().isEmpty());
        dialog.reload();
        QVERIFY(!hasNote());
    }

    void removingAFolderAsksWhatToDo_data()
    {
        QTest::addColumn<QString>("button");
        QTest::addColumn<int>("rowsAfter");
        QTest::addColumn<qint64>("hiddenAfter");
        QTest::newRow("cancel") << QString() << 1 << qint64(0);
        QTest::newRow("keep") << QStringLiteral("removeKeep") << 0 << qint64(1);
        QTest::newRow("delete") << QStringLiteral("removeForget") << 0 << qint64(0);
    }

    void removingAFolderAsksWhatToDo()
    {
        QFETCH(QString, button);
        QFETCH(int, rowsAfter);
        QFETCH(qint64, hiddenAfter);
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        // This box is meant to be here, so the watcher isn't what closes it.
        m_boxWatcher->stop();
        bool sawChoice = false;
        QTimer::singleShot(100, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            sawChoice = box->findChild<QPushButton *>(QStringLiteral("removeKeep"))
                && box->findChild<QPushButton *>(QStringLiteral("removeForget"));
            // Cancelling has to be what happens if nothing's chosen with care.
            QVERIFY(box->defaultButton() == box->button(QMessageBox::Cancel));
            QAbstractButton *choice = button.isEmpty() ? static_cast<QAbstractButton *>(box->button(QMessageBox::Cancel))
                                                       : box->findChild<QPushButton *>(button);
            choice->click();
        });
        m_window->removeRoot(m_session->files().roots().first().id);
        m_boxWatcher->start(100);

        QVERIFY(sawChoice);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), rowsAfter, kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->files().hiddenCount(), hiddenAfter, kLongWaitMs);
    }

    void aFolderWithNothingToLoseIsntAskedAbout()
    {
        const QString folder = copySamples({});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(m_session->files().roots().size(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        m_boxWatcher->stop();
        bool offeredToDelete = true;
        QTimer::singleShot(100, this, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            offeredToDelete = box->findChild<QPushButton *>(QStringLiteral("removeForget")) != nullptr;
            box->findChild<QPushButton *>(QStringLiteral("removeKeep"))->click();
        });
        m_window->removeRoot(m_session->files().roots().first().id);
        m_boxWatcher->start(100);
        QVERIFY(!offeredToDelete);
        QVERIFY(m_session->files().roots().isEmpty());
    }

    void choosingACollectionInTheSidebarShowsOnlyItsPublications()
    {
        addMany(5);
        const qint64 id = m_session->collections().create(QStringLiteral("Project"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 5, kLongWaitMs);
        QCOMPARE(m_window->documentTabs()->count(), 1); // No tabs of their own.

        selectRows(1, 2);
        QSignalSpy finished(m_window->bulkChanges(), &BulkChanges::finished);
        m_window->addSelectedToCollection(id);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, kLongWaitMs);
        QCOMPARE(m_session->collections().get(id)->count, qint64(2));

        // It's listed with a count, above the tags.
        QTRY_VERIFY_WITH_TIMEOUT(collectionItem(QStringLiteral("Project")) != nullptr, kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(collectionItem(QStringLiteral("Project"))->text(1), QStringLiteral("2"), kLongWaitMs);
        QTreeWidget *tree = m_window->sidebar()->tree();
        QCOMPARE(tree->indexOfTopLevelItem(m_window->sidebar()->sectionItem(FilterSidebar::Collections)), 0);

        collectionItem(QStringLiteral("Project"))->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(title_(0), QStringLiteral("Paper 00001"), kLongWaitMs);
        QVERIFY(m_window->sidebar()->hasSelection());
        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty()) { // For looking at by eye, if asked for.
            m_window->resize(1200, 700);
            QTest::qWait(300);
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("collection.png")));
        }

        // It's a filter like the others, so it combines with search, and Clear undoes it.
        m_window->searchBox()->setText(QStringLiteral("00002"));
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->searchBox()->clear();
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);

        m_window->sidebar()->clearSelection();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 5, kLongWaitMs);
    }

    void theMenuShowsWhatsAlreadyInACollection()
    {
        addMany(3);
        const qint64 rd = m_session->collections().create(QStringLiteral("R&D"));
        const qint64 other = m_session->collections().create(QStringLiteral("Other"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, kLongWaitMs);
        const QList<qint64> ids = allIds();
        m_session->collections().add(rd, {ids.at(0)});

        // One that's in a collection: ticked and can't be added again, and can be removed, with no filter.
        selectRows(0, 0);
        QMenu menu;
        m_window->addCollectionActions(&menu);
        QMenu *addTo = submenu(menu, QStringLiteral("Add to collection"));
        QMenu *removeFrom = submenu(menu, QStringLiteral("Remove from collection"));
        QVERIFY(addTo != nullptr);
        QVERIFY(removeFrom != nullptr);

        QAction *in = action(*addTo, QStringLiteral("R&&D"));
        QVERIFY(in != nullptr);
        QVERIFY(in->isChecked());
        QVERIFY(!in->isEnabled());
        QAction *notIn = action(*addTo, QStringLiteral("Other"));
        QVERIFY(!notIn->isChecked());
        QVERIFY(notIn->isEnabled());
        QVERIFY(action(*addTo, QStringLiteral("New collection...")) != nullptr);
        QCOMPARE(removeFrom->actions().size(), 1);

        QSignalSpy finished(m_window->bulkChanges(), &BulkChanges::finished);
        action(*removeFrom, QStringLiteral("R&&D"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, kLongWaitMs);
        QVERIFY(!m_session->collections().contains(rd, ids.at(0)));
        QCOMPARE(rows(), 3); // It wasn't filtered, and nothing has left the library.

        // One that's in none has nothing to remove from.
        QMenu none;
        m_window->addCollectionActions(&none);
        QVERIFY(submenu(none, QStringLiteral("Remove from collection")) == nullptr);
        QVERIFY(!action(*submenu(none, QStringLiteral("Add to collection")), QStringLiteral("R&&D"))->isChecked());

        // Several, with some in it: it says so, and can still add the rest or remove those.
        m_session->collections().add(other, {ids.at(0)});
        selectRows(0, 1);
        QMenu several;
        m_window->addCollectionActions(&several);
        QAction *partly = action(*submenu(several, QStringLiteral("Add to collection")), QStringLiteral("Other (1 of 2 already in it)"));
        QVERIFY(partly != nullptr);
        QVERIFY(partly->isEnabled());
        QVERIFY(!partly->isChecked());
        QVERIFY(action(*submenu(several, QStringLiteral("Remove from collection")), QStringLiteral("Other (1 of 2 in it)")) != nullptr);
        partly->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, kLongWaitMs);
        QCOMPARE(m_session->collections().get(other)->count, qint64(2));

        // A large selection isn't worked out, and every collection is offered both ways.
        addMany(1100, 1000);
        m_window->applyFiltersNow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1103, kLongWaitMs);
        m_window->gridView()->selectAll();
        QCOMPARE(m_window->selectedCount(), 1103);
        QMenu large;
        m_window->addCollectionActions(&large);
        QCOMPARE(submenu(large, QStringLiteral("Remove from collection"))->actions().size(), 2);
    }

    void theSidebarSaysWhereCollectionsWillAppear()
    {
        openWindow();
        QTreeWidgetItem *section = m_window->sidebar()->sectionItem(FilterSidebar::Collections);
        QTRY_COMPARE_WITH_TIMEOUT(section->childCount(), 1, kLongWaitMs);
        QVERIFY(section->child(0)->text(0).contains(QStringLiteral("appear here")));
        QVERIFY(!section->isHidden());
    }

    void removingFromACollectionLeavesItInTheLibrary()
    {
        addMany(3);
        const qint64 id = m_session->collections().create(QStringLiteral("Project"));
        m_session->collections().add(id, allIds());
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(collectionItem(QStringLiteral("Project")) != nullptr, kLongWaitMs);
        collectionItem(QStringLiteral("Project"))->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->filter().collectionIds.size(), 1, kLongWaitMs);

        selectRows(0, 0);
        QSignalSpy finished(m_window->bulkChanges(), &BulkChanges::finished);
        m_window->removeSelectedFromCollection(id);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);

        m_window->sidebar()->clearSelection();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 3, kLongWaitMs);
    }

    void anEmptyCollectionSaysHowToAddToIt()
    {
        addMany(2);
        m_session->collections().create(QStringLiteral("Project"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(collectionItem(QStringLiteral("Project")) != nullptr, kLongWaitMs);
        collectionItem(QStringLiteral("Project"))->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->viewStack()->currentWidget() != m_window->gridView()
                                     && m_window->viewStack()->currentWidget() != m_window->listView(), kLongWaitMs);
        bool mentioned = false;
        for (const QLabel *label : m_window->viewStack()->currentWidget()->findChildren<QLabel *>())
            mentioned = mentioned || label->text().contains(QStringLiteral("Add to collection"));
        QVERIFY(mentioned);
    }

    void deletingACollectionAsksFirstAndLeavesItsPublications()
    {
        addMany(2);
        const qint64 id = m_session->collections().create(QStringLiteral("Project"));
        m_session->collections().add(id, {allIds().first()});
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(collectionItem(QStringLiteral("Project")) != nullptr, kLongWaitMs);
        collectionItem(QStringLiteral("Project"))->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);

        m_boxWatcher->stop();
        bool cancelWasDefault = false;
        QTimer::singleShot(100, this, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                cancelWasDefault = box->defaultButton() == box->button(QMessageBox::Cancel);
                box->button(QMessageBox::Cancel)->click();
            }
        });
        m_window->sidebar()->deleteCollection(id);
        QVERIFY(cancelWasDefault);
        QVERIFY(m_session->collections().get(id).has_value());

        QTimer::singleShot(100, this, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Yes)->click();
        });
        m_window->sidebar()->deleteCollection(id);
        m_boxWatcher->start(100);

        QVERIFY(!m_session->collections().get(id).has_value());
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, kLongWaitMs); // The filter's gone, and they're still there.
        QVERIFY(!m_window->sidebar()->hasSelection());
        QTRY_VERIFY_WITH_TIMEOUT(collectionItem(QStringLiteral("Project")) == nullptr, kLongWaitMs);
    }

    void aPublicationFromARemovedFolderLeavesItsCollectionUntilItReturns()
    {
        const QString folder = copySamples({QStringLiteral("article.pdf")});
        openWindow();
        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        const qint64 id = m_session->collections().create(QStringLiteral("Project"));
        m_session->collections().add(id, {allIds().first()});
        m_window->applyFiltersNow();
        QTRY_VERIFY_WITH_TIMEOUT(collectionItem(QStringLiteral("Project")) != nullptr, kLongWaitMs);
        collectionItem(QStringLiteral("Project"))->setCheckState(0, Qt::Checked);
        QTRY_COMPARE_WITH_TIMEOUT(m_window->model()->filter().collectionIds.size(), 1, kLongWaitMs);

        m_window->removeRootKeepingDetails(m_session->files().roots().first().id);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 0, kLongWaitMs);
        QCOMPARE(m_session->collections().get(id)->count, qint64(0));

        m_window->addFolder(folder);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);
        QCOMPARE(m_session->collections().get(id)->count, qint64(1));
    }

    void theDetailsSayWhichCollectionsSomethingIsIn()
    {
        const qint64 publication = addPublication(QStringLiteral("A Paper"), PublicationType::JournalArticle);
        const qint64 id = m_session->collections().create(QStringLiteral("R&D <b>Project</b>"));
        openWindow();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, kLongWaitMs);
        m_window->details()->showPublication(publication);
        QVERIFY(!m_window->details()->collectionsLabel()->isVisibleTo(m_window->details()));

        m_session->collections().add(id, {publication});
        m_window->details()->reload();
        QVERIFY(m_window->details()->collectionsLabel()->isVisibleTo(m_window->details()));
        QCOMPARE(m_window->details()->collectionsLabel()->textFormat(), Qt::PlainText);
        QVERIFY(m_window->details()->collectionsLabel()->text().contains(QStringLiteral("R&D <b>Project</b>")));
    }

    void copyingAReferencePutsItOnTheClipboard()
    {
        const qint64 id = addCitable(QStringLiteral("Dent"), QStringLiteral("Carla"), QStringLiteral("Why Do Models Fail"), 2019);
        openWindow();
        CitationActions *citations = m_window->citations();

        // Harvard to start with, with italics for a word processor and plain text for anything else.
        QCOMPARE(citations->style(), citation::CitationStyle::Harvard);
        QVERIFY(citations->copyReferences({id}));
        const QMimeData *copied = QApplication::clipboard()->mimeData();
        QCOMPARE(copied->text(), QStringLiteral("Dent, C., 2019. Why Do Models Fail. *Made-Up Review*, 12, 101-108.").remove(u'*'));
        QVERIFY2(copied->html().contains(QStringLiteral("<i>Made-Up Review</i>")), qPrintable(copied->html()));

        // The style chosen is remembered.
        citations->setStyle(citation::CitationStyle::Apa);
        QCOMPARE(m_session->settings().value(QStringLiteral("citation.style")).toString(), QStringLiteral("apa"));
        QVERIFY(citations->copyInTextCitation({id}));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("(Dent, 2019)"));

        // Several together make one citation, and a list with one reference on each line.
        const qint64 other = addCitable(QStringLiteral("Avery"), QStringLiteral("Sam"), QStringLiteral("Earlier Work"), 2010);
        QVERIFY(citations->copyInTextCitation({id, other}));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("(Avery, 2010; Dent, 2019)"));
        QVERIFY(citations->copyReferences({id, other}));
        QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("Avery, S. (2010).")));
        QCOMPARE(QApplication::clipboard()->text().count(u'\n'), 1);

        // Nothing, or too many, isn't copied.
        QVERIFY(!citations->copyReferences({}));
        QList<qint64> tooMany;
        for (int i = 0; i <= CitationActions::kMaxToCopy; ++i)
            tooMany << id;
        QVERIFY(!citations->copyReferences(tooMany));
    }

    void theCiteMenuOffersStylesAndSaysWhatsChosen()
    {
        const qint64 id = addCitable(QStringLiteral("Dent"), QStringLiteral("Carla"), QStringLiteral("Why Do Models Fail"), 2019);
        openWindow();
        CitationActions *citations = m_window->citations();
        citations->setStyle(citation::CitationStyle::Ieee);

        QMenu menu;
        citations->fillCiteMenu(&menu, 1, [id] { return QList<qint64>{id}; });
        QCOMPARE(menu.actions().at(0)->text(), QStringLiteral("Copy reference (IEEE)"));
        QVERIFY(menu.actions().at(0)->isEnabled());
        QMenu *styles = nullptr;
        for (QAction *action : menu.actions()) {
            if (action->menu())
                styles = action->menu();
        }
        QVERIFY(styles);
        QCOMPARE(styles->actions().size(), 4);
        for (QAction *action : styles->actions())
            QCOMPARE(action->isChecked(), action->text() == QStringLiteral("IEEE"));
        styles->actions().at(3)->trigger();
        QCOMPARE(citations->style(), citation::CitationStyle::Chicago);

        menu.actions().at(0)->trigger();
        QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("Dent, Carla. 2019.")));

        // A selection too large to copy can still be exported.
        QMenu large;
        citations->fillCiteMenu(&large, CitationActions::kMaxToCopy + 1, [] { return QList<qint64>(); });
        QVERIFY(!large.actions().at(0)->isEnabled());
        QVERIFY(large.actions().last()->isEnabled());
        QVERIFY(large.actions().last()->text().startsWith(QStringLiteral("Export %1").arg(CitationActions::kMaxToCopy + 1)));

        // The details panel's Cite button has the same menu.
        m_window->details()->showPublication(id);
        QMenu *details = m_window->details()->citeButton()->menu();
        QVERIFY(details);
        emit details->aboutToShow();
        QVERIFY(!details->actions().isEmpty());
        QVERIFY(details->actions().at(0)->text().startsWith(QStringLiteral("Copy reference")));

        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty()) { // For looking at by eye, if asked for.
            m_window->resize(1200, 700);
            QTest::qWait(300);
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("cite-button.png")));
        }
    }

    void citationsAreExportedInTheBackground()
    {
        const qint64 id = addCitable(QStringLiteral("Dent"), QStringLiteral("Carla"), QStringLiteral("Why Do Models Fail"), 2019);
        addMany(1200);
        openWindow();
        QList<qint64> ids = m_session->query().ids({}, {});
        QCOMPARE(ids.size(), 1201);

        const QString path = m_dir->filePath(QStringLiteral("export.bib"));
        QSignalSpy finished(m_window->citations(), &CitationActions::exportFinished);
        m_window->citations()->startExport(ids, path, citation::ExportFormat::BibTeX);
        QVERIFY(m_window->citations()->isExporting());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, kLongWaitMs);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString bib = QString::fromUtf8(file.readAll());
        QCOMPARE(bib.count(QStringLiteral("\n@")) + (bib.startsWith(u'@') ? 1 : 0), 1201);
        QVERIFY(bib.contains(QStringLiteral("@article{dent2019why,")));
        QVERIFY(!m_window->citations()->isExporting());

        // RIS and CSL-JSON too.
        m_window->citations()->startExport({id}, m_dir->filePath(QStringLiteral("export.ris")), citation::ExportFormat::Ris);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, kLongWaitMs);
        QFile ris(m_dir->filePath(QStringLiteral("export.ris")));
        QVERIFY(ris.open(QIODevice::ReadOnly));
        QVERIFY(ris.readAll().startsWith("TY  - JOUR"));

        // A file that can't be written says so.
        m_window->citations()->startExport({id}, m_dir->filePath(QStringLiteral("no/such/folder/x.json")), citation::ExportFormat::CslJson);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, kLongWaitMs);
        QVERIFY(!finished.at(2).at(0).toBool());
    }

    // ---- Moving a library between computers ----
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

    QList<qint64> allIds() { return m_session->query().ids({}, {}); }

    void selectRows(int first, int last)
    {
        const auto *model = m_window->model();
        m_window->gridView()->selectionModel()->select(QItemSelection(model->index(first, 0), model->index(last, 0)),
                                                       QItemSelectionModel::ClearAndSelect);
    }

    static QMenu *submenu(const QMenu &menu, const QString &title)
    {
        for (QAction *item : menu.actions()) {
            if (item->menu() && item->text() == title)
                return item->menu();
        }
        return nullptr;
    }

    static QAction *action(const QMenu &menu, const QString &text)
    {
        for (QAction *item : menu.actions()) {
            if (item->text() == text)
                return item;
        }
        return nullptr;
    }

    QTreeWidgetItem *collectionItem(const QString &name) const
    {
        QTreeWidgetItem *section = m_window->sidebar()->sectionItem(FilterSidebar::Collections);
        for (int i = 0; i < section->childCount(); ++i) {
            if (section->child(i)->text(0) == name)
                return section->child(i);
        }
        return nullptr;
    }

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

    qint64 addCitable(const QString &family, const QString &given, const QString &title, int year)
    {
        Publication p;
        p.title = title;
        p.type = PublicationType::JournalArticle;
        p.authors = {{family, given}};
        p.containerTitle = QStringLiteral("Made-Up Review");
        p.volume = QStringLiteral("12");
        p.pages = QStringLiteral("101-108");
        p.year = year;
        return m_session->publications().create(p);
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

    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
};

QTEST_MAIN(LibraryUiTest)
#include "test_library_ui.moc"
