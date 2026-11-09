// Tests of highlights and notes.

#include "core/db/Database.h"
#include "core/db/Schema.h"
#include "core/repo/AnnotationRepository.h"
#include "core/repo/FileRepository.h"
#include "core/repo/PublicationMerge.h"
#include "core/repo/PublicationRepository.h"
#include "core/repo/VolumeArticles.h"
#include "ui/Delegates.h"
#include "ui/DetailsPanel.h"
#include "ui/ImportThread.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/MarkedCopy.h"
#include "ui/NotesDialog.h"
#include "ui/PageView.h"
#include "ui/ReaderTab.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QClipboard>
#include <QGuiApplication>
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

class AnnotationsTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_docs = QDir::cleanPath(m_dir->filePath(QStringLiteral("docs")));
        QDir().mkpath(m_docs);
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
        m_session->settings().setValue(QStringLiteral("library.index_contents"), false);
    }

    void cleanup()
    {
        m_boxWatcher.reset();
        QVERIFY2(m_unexpectedBoxes.isEmpty(), qPrintable(m_unexpectedBoxes.join(QStringLiteral("; "))));
        m_window.reset();
        m_session.reset();
        m_dir.reset();
    }

    // ---- Storing them ----

    void anAnnotationSurvivesBeingStored()
    {
        Publication p;
        p.title = QStringLiteral("A Paper");
        const qint64 id = m_session->publications().create(p);
        AnnotationRepository annotations(m_session->database());

        Annotation annotation;
        annotation.publicationId = id;
        annotation.contentHash = QStringLiteral("hash");
        annotation.type = AnnotationType::Underline;
        annotation.colour = QStringLiteral("#8fe388");
        annotation.note = QStringLiteral("My note");
        AnnotationPart fixed;
        fixed.text = QStringLiteral("some words");
        fixed.page = 3;
        fixed.rects = {QRectF(10.5, 20, 100, 12), QRectF(10, 34, 50, 12)};
        AnnotationPart flow;
        flow.text = QStringLiteral("other words");
        flow.place = QStringLiteral("2:0.5");
        flow.occurrence = 1;
        annotation.parts = {fixed, flow};
        const qint64 annotationId = annotations.create(annotation);
        QVERIFY(!annotation.uuid.isEmpty());

        const Annotation back = *annotations.get(annotationId);
        QCOMPARE(back.uuid, annotation.uuid);
        QCOMPARE(back.type, AnnotationType::Underline);
        QCOMPARE(back.colour, QStringLiteral("#8fe388"));
        QCOMPARE(back.note, QStringLiteral("My note"));
        QCOMPARE(back.parts.size(), 2);
        QCOMPARE(back.parts.at(0).page, 3);
        QCOMPARE(back.parts.at(0).rects.at(0), QRectF(10.5, 20, 100, 12));
        QCOMPARE(back.parts.at(1).place, QStringLiteral("2:0.5"));
        QCOMPARE(back.parts.at(1).occurrence, 1);
        QCOMPARE(back.selectedText(), QStringLiteral("some words\nother words"));
        QCOMPARE(back.firstPage(), 3);

        annotations.setNote(annotationId, QStringLiteral("Changed"));
        annotations.setColour(annotationId, QStringLiteral("#7fc8f8"));
        annotations.setType(annotationId, AnnotationType::Highlight);
        QCOMPARE(annotations.get(annotationId)->note, QStringLiteral("Changed"));
        QCOMPARE(annotations.get(annotationId)->colour, QStringLiteral("#7fc8f8"));
        QCOMPARE(annotations.get(annotationId)->type, AnnotationType::Highlight);

        annotations.remove(annotationId);
        QVERIFY(!annotations.get(annotationId).has_value());
    }

    void theyreListedInDocumentOrder()
    {
        Publication p;
        p.title = QStringLiteral("A Paper");
        const qint64 id = m_session->publications().create(p);
        AnnotationRepository annotations(m_session->database());
        const auto add = [&](int page, const QString &place, const QString &text) {
            Annotation annotation;
            annotation.publicationId = id;
            annotation.contentHash = QStringLiteral("hash");
            AnnotationPart part;
            part.page = page;
            part.place = place;
            part.text = text;
            annotation.parts = {part};
            annotations.create(annotation);
        };
        add(5, QString(), QStringLiteral("page five"));
        add(1, QString(), QStringLiteral("page one"));
        const QList<Annotation> fixed = annotations.forContents(QStringLiteral("hash"));
        QCOMPARE(fixed.first().selectedText(), QStringLiteral("page one"));

        // Sorting chapters as numbers, and 10 comes after 2.
        add(-1, QStringLiteral("10:0.1"), QStringLiteral("chapter ten"));
        add(-1, QStringLiteral("2:0.9"), QStringLiteral("chapter two late"));
        add(-1, QStringLiteral("2:0.1"), QStringLiteral("chapter two early"));
        const QList<Annotation> flow = annotations.forPublication(id);
        QCOMPARE(flow.at(0).selectedText(), QStringLiteral("chapter two early"));
        QCOMPARE(flow.at(1).selectedText(), QStringLiteral("chapter two late"));
        QCOMPARE(flow.at(2).selectedText(), QStringLiteral("chapter ten"));
        QCOMPARE(annotations.countFor(id), qint64(5));
        QCOMPARE(annotations.forContents(QString()).size(), 0);
    }

    void theyFollowAPublicationThroughBeingCombined()
    {
        Publication first;
        first.title = QStringLiteral("One");
        Publication second;
        second.title = QStringLiteral("Two");
        const qint64 keep = m_session->publications().create(first);
        const qint64 other = m_session->publications().create(second);
        AnnotationRepository annotations(m_session->database());
        Annotation annotation;
        annotation.publicationId = other;
        annotation.contentHash = QStringLiteral("hash");
        const qint64 id = annotations.create(annotation);
        mergePublications(m_session->database(), keep, other, QStringLiteral("manual"));
        QCOMPARE(annotations.get(id)->publicationId, keep);
    }

    void markdownHasTheDetailsQuotesAndNotes()
    {
        Publication p;
        p.title = QStringLiteral("Fatigue of\nSteel Beams");
        p.authors = {{QStringLiteral("Example"), QStringLiteral("Ada")}};
        p.containerTitle = QStringLiteral("Journal of Made-up Results");
        p.year = 2020;
        p.doi = QStringLiteral("10.1000/x");

        Annotation withNote;
        withNote.note = QStringLiteral("  This matters.  ");
        AnnotationPart part;
        part.page = 11;
        part.text = QStringLiteral("first line\nsecond line");
        withNote.parts = {part};
        Annotation flow;
        AnnotationPart flowPart;
        flowPart.place = QStringLiteral("1:0.5");
        flowPart.text = QStringLiteral("flowing words");
        flow.parts = {flowPart};

        const QString markdown = annotationsToMarkdown(p, {withNote, flow});
        QVERIFY(markdown.startsWith(QStringLiteral("# Fatigue of Steel Beams\n")));
        QVERIFY(markdown.contains(QStringLiteral("Example, Ada")));
        QVERIFY(markdown.contains(QStringLiteral("Journal of Made-up Results, 2020, doi:10.1000/x")));
        QVERIFY(markdown.contains(QStringLiteral("## Page 12\n")));
        QVERIFY(markdown.contains(QStringLiteral("> first line\n> second line\n")));
        QVERIFY(markdown.contains(QStringLiteral("\nThis matters.\n")));
        QVERIFY(markdown.contains(QStringLiteral("> flowing words\n")));
    }

    // ---- Fixed pages, in the reader ----

    void aHighlightOnAPageStaysThroughRestartsAndMoves()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        const QString file = QDir(m_docs).filePath(QStringLiteral("reader_doc.pdf"));
        const QByteArray before = hashOf(file);

        ReaderTab *reader = openReader(id);
        const auto [page, rect] = findWord(reader, QStringLiteral("findme"));
        QCOMPARE(page, 1);
        selectRect(reader, page, rect);
        QVERIFY(reader->pageView()->selectedText().contains(QStringLiteral("findme")));

        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight, QStringLiteral("Look at this"));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);

        // Saved, showing, listed, and the selection has gone.
        AnnotationRepository annotations(m_session->database());
        QCOMPARE(annotations.forPublication(id).size(), 1);
        const Annotation saved = annotations.forPublication(id).first();
        QCOMPARE(saved.note, QStringLiteral("Look at this"));
        QVERIFY(saved.selectedText().contains(QStringLiteral("findme")));
        QCOMPARE(saved.firstPage(), 1);
        QVERIFY(!saved.contentHash.isEmpty());
        QCOMPARE(reader->pageView()->marksOn(1).size(), 1);
        QVERIFY(!reader->pageView()->hasSelection());
        QCOMPARE(reader->annotationList()->count(), 1);
        QVERIFY(reader->annotationList()->item(0)->text().contains(QStringLiteral("Look at this")));
        // The panel draws the words and the note apart, so it's given them apart.
        {
            const QListWidgetItem *listed = reader->annotationList()->item(0);
            QCOMPARE(listed->data(AnnotationDelegate::HeadingRole).toString(), QStringLiteral("Page 2"));
            QVERIFY(listed->data(AnnotationDelegate::QuoteRole).toString().contains(QStringLiteral("findme")));
            QVERIFY(!listed->data(AnnotationDelegate::QuoteRole).toString().contains(QStringLiteral("Look at this")));
            QCOMPARE(listed->data(AnnotationDelegate::NoteRole).toString(), QStringLiteral("Look at this"));
            QCOMPARE(listed->data(AnnotationDelegate::ColourRole).toString(), saved.colour);
            QStyleOptionViewItem option;
            option.initFrom(reader->annotationList());
            option.widget = reader->annotationList();
            const QModelIndex index = reader->annotationList()->model()->index(0, 0);
            QVERIFY(reader->annotationList()->itemDelegate()->sizeHint(option, index).height()
                    > 3 * reader->annotationList()->fontMetrics().lineSpacing());
        }

        // Reopening it, as after a restart.
        reader = reopenReader(id);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->marksOn(1).size(), 1, kLongWaitMs);
        QCOMPARE(reader->pageView()->marksOn(1).first().rects, saved.parts.first().rects);

        // Moving and renaming the file, or the whole folder, changes nothing.
        closeReaders();
        const QString moved = QDir::cleanPath(m_dir->filePath(QStringLiteral("moved")));
        QVERIFY(QDir().rename(m_docs, moved));
        m_session->files().repointRoot(m_session->files().roots().first().id, moved);
        const QString renamed = QDir(moved).filePath(QStringLiteral("a new name.pdf"));
        QVERIFY(QFile::rename(QDir(moved).filePath(QStringLiteral("reader_doc.pdf")), renamed));
        m_window->addFolder(moved);
        QTRY_VERIFY_WITH_TIMEOUT(m_session->files().filesFor(id).first().relativePath == QStringLiteral("a new name.pdf"), kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->importThread()->isBusy(), kLongWaitMs);

        reader = openReader(id);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->marksOn(1).size(), 1, kLongWaitMs);

        // The document itself is never written to.
        QCOMPARE(hashOf(renamed), before);
    }

    void aSelectionAcrossPagesIsOneAnnotation()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        PageView *view = reader->pageView();
        view->setSelection(0, {QRectF(10, 10, 100, 12)}, QStringLiteral("on one page"));
        view->setSelection(1, {QRectF(10, 10, 100, 12)}, QStringLiteral("on the next"));
        QCOMPARE(view->selectionParts().size(), 2);

        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Underline);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);

        AnnotationRepository annotations(m_session->database());
        QCOMPARE(annotations.forPublication(id).size(), 1);
        QCOMPARE(annotations.forPublication(id).first().parts.size(), 2);
        QCOMPARE(view->marksOn(0).size(), 1);
        QCOMPARE(view->marksOn(1).size(), 1);
        QVERIFY(view->marksOn(0).first().underline);
    }

    void everyHighlightIsListedAndOpensAtItsPlace()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        QVERIFY(reader);
        PageView *view = reader->pageView();
        const int page = view->pageCount() - 1;
        QVERIFY(page > 0);
        view->setSelection(page, {QRectF(10, 10, 100, 12)}, QStringLiteral("a made-up quotation"));
        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight, QStringLiteral("compare with chapter two"));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        closeReaders();

        NotesDialog dialog(*m_session);
        QCOMPARE(dialog.list()->topLevelItemCount(), 1);
        QTreeWidgetItem *item = dialog.list()->topLevelItem(0);
        QCOMPARE(item->text(0), QStringLiteral("a made-up quotation"));
        QCOMPARE(item->text(1), QStringLiteral("compare with chapter two"));
        QCOMPARE(item->text(3), QString::number(page + 1));
        QCOMPARE(dialog.countLabel()->text(), QStringLiteral("1 highlight or note"));

        // Searching looks in both the words and the note.
        dialog.searchBox()->setText(QStringLiteral("chap"));
        dialog.reload();
        QCOMPARE(dialog.list()->topLevelItemCount(), 1);
        dialog.searchBox()->setText(QStringLiteral("nowhere"));
        dialog.reload();
        QCOMPARE(dialog.list()->topLevelItemCount(), 0);
        QCOMPARE(dialog.countLabel()->text(), QStringLiteral("Nothing matches."));
        dialog.searchBox()->setText(QStringLiteral("quotation"));
        dialog.reload();
        QCOMPARE(dialog.list()->topLevelItemCount(), 1);

        // Opening it shows its page in its document.
        QSignalSpy open(&dialog, &NotesDialog::openRequested);
        emit dialog.list()->itemActivated(dialog.list()->topLevelItem(0), 0);
        QCOMPARE(open.size(), 1);
        QVERIFY(m_window->openAnnotation(open.first().first().toLongLong()));
        QCOMPARE(m_window->documentTabs()->count(), 2);
        auto *opened = qobject_cast<ReaderTab *>(m_window->documentTabs()->currentWidget());
        QVERIFY(opened);
        QTRY_VERIFY_WITH_TIMEOUT(opened->isReady(), kLongWaitMs);
        QTRY_COMPARE_WITH_TIMEOUT(opened->pageView()->currentPage(), page, kLongWaitMs);
    }

    void aCopyIsSavedWithItsHighlightsAndTheOriginalIsLeftAlone()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        QVERIFY(reader);
        PageView *view = reader->pageView();
        view->setSelection(1, {QRectF(72, 100, 200, 14)}, QStringLiteral("a made-up passage"));
        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight, QStringLiteral("a made-up remark"), QStringLiteral("#81c784"));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        view->setSelection(2, {QRectF(72, 140, 120, 14)}, QStringLiteral("underlined words"));
        changed.clear();
        reader->annotateSelection(AnnotationType::Underline);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        closeReaders();

        const QString original = QDir(m_docs).filePath(QStringLiteral("reader_doc.pdf"));
        const QByteArray before = hashOf(original);
        const std::optional<qint64> fileId = markedPdfFor(*m_session, id);
        QVERIFY(fileId);
        int count = 0;
        const QJsonArray marks = marksForContents(m_session->database(), m_session->files().get(*fileId)->sha256, &count);
        QCOMPARE(count, 2);
        QCOMPARE(marks.size(), 2);

        // Somewhere outside the library, as it's offered.
        const QString out = m_dir->filePath(QStringLiteral("elsewhere/copy with highlights.pdf"));
        QDir().mkpath(QFileInfo(out).absolutePath());
        QSignalSpy finished(m_window->markedCopy(), &MarkedCopyActions::finished);
        QVERIFY(m_window->markedCopy()->save(*fileId, out));
        QVERIFY(finished.wait(kLongWaitMs));
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("2 highlights and notes")));

        QFile copy(out);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        const QByteArray written = copy.readAll();
        QVERIFY(written.startsWith("%PDF"));
        QVERIFY(written.contains("/Highlight"));
        QVERIFY(written.contains("/Underline"));
        QVERIFY(written.contains("made-up remark"));
        QCOMPARE(hashOf(original), before); // The original is never touched.

        // A copy can't be written over the original, even if asked.
        finished.clear();
        QVERIFY(m_window->markedCopy()->save(*fileId, original));
        QVERIFY(finished.wait(kLongWaitMs));
        QVERIFY(!finished.first().at(0).toBool());
        QCOMPARE(hashOf(original), before);
        // The window says why, which is expected here.
        QTRY_VERIFY_WITH_TIMEOUT(!m_unexpectedBoxes.isEmpty(), kLongWaitMs);
        QVERIFY(m_unexpectedBoxes.first().contains(QStringLiteral("can't replace the original")));
        m_unexpectedBoxes.clear();
    }

    void aMarkCanBeFoundUnderTheCursor()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        const auto [page, rect] = findWord(reader, QStringLiteral("findme"));
        selectRect(reader, page, rect);
        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        const qint64 annotation = AnnotationRepository(m_session->database()).forPublication(id).first().id;

        PageView *view = reader->pageView();
        view->goToPage(page);
        QTest::qWait(100);
        const QPoint inside = view->viewportPointFor(page, rect.center());
        QCOMPARE(view->annotationAt(inside).value_or(0), annotation);
        QVERIFY(!view->annotationAt(view->viewportPointFor(page, QPointF(1, 1))).has_value());
    }

    void changingAndDeletingFromThePanel()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        const auto [page, rect] = findWord(reader, QStringLiteral("findme"));
        selectRect(reader, page, rect);
        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        AnnotationRepository annotations(m_session->database());
        const qint64 annotation = annotations.forPublication(id).first().id;

        reader->setAnnotationNote(annotation, QStringLiteral("Added later"));
        QCOMPARE(annotations.get(annotation)->note, QStringLiteral("Added later"));
        QVERIFY(reader->annotationList()->item(0)->text().contains(QStringLiteral("Added later")));

        reader->setAnnotationColour(annotation, annotationColours().at(2));
        QCOMPARE(reader->pageView()->marksOn(1).first().colour, QColor(annotationColours().at(2)));

        // Its colour menu ticks the colour it has now, and only that one.
        QMenu menu;
        QMenu *colours = reader->addColourMenu(&menu, annotation);
        QCOMPARE(colours->actions().size(), annotationColours().size());
        for (qsizetype i = 0; i < colours->actions().size(); ++i) {
            QVERIFY(colours->actions().at(i)->isCheckable());
            QCOMPARE(colours->actions().at(i)->isChecked(), i == 2);
        }
        colours->actions().at(1)->trigger();
        QCOMPARE(annotations.get(annotation)->colour.toLower(), annotationColours().at(1).toLower());
        QMenu again;
        QVERIFY(reader->addColourMenu(&again, annotation)->actions().at(1)->isChecked());

        // For looking at by eye, if asked for.
        const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES");
        if (!pictures.isEmpty()) {
            m_window->resize(1200, 800);
            reader->setSidePanelVisible(true);
            reader->findChild<QTabWidget *>()->setCurrentIndex(2);
            reader->pageView()->goToPage(1);
            QTest::qWait(1500);
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("highlight.png")));
        }

        // Choosing it in the list goes there.
        reader->pageView()->goToPage(0);
        emit reader->annotationList()->itemActivated(reader->annotationList()->item(0));
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->currentPage(), 1, kLongWaitMs);

        reader->deleteAnnotation(annotation);
        QVERIFY(!annotations.get(annotation).has_value());
        QVERIFY(reader->pageView()->marksOn(1).isEmpty());
        QCOMPARE(reader->annotationList()->count(), 0);
    }

    void severalCanBeChosenAndChangedTogether()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        const auto [page, rect] = findWord(reader, QStringLiteral("findme"));
        AnnotationRepository annotations(m_session->database());
        for (const QString &note : {QStringLiteral("First note"), QStringLiteral("Second note"), QStringLiteral("Third note")}) {
            selectRect(reader, page, rect);
            QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
            reader->annotateSelection(AnnotationType::Highlight, note);
            QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        }
        QListWidget *list = reader->annotationList();
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 3, kLongWaitMs);
        QCOMPARE(list->selectionMode(), QAbstractItemView::ExtendedSelection);
        // The Notes panel with several in it, one chosen, for looking at by eye if asked for.
        if (const QString pictures = qEnvironmentVariable("PAPERTRAIL_TEST_PICTURES"); !pictures.isEmpty()) {
            m_window->resize(1200, 800);
            reader->setSidePanelVisible(true);
            reader->findChild<QTabWidget *>()->setCurrentIndex(2);
            list->item(1)->setSelected(true);
            QTest::qWait(1500);
            m_window->grab().save(QDir(pictures).filePath(QStringLiteral("notes-panel.png")));
            list->clearSelection();
        }

        // Choose the first and third, by their notes, as Ctrl-clicking would.
        const auto rowWith = [list](const QString &note) {
            for (int row = 0; row < list->count(); ++row) {
                if (list->item(row)->text().contains(note))
                    return row;
            }
            return -1;
        };
        const auto idWith = [&](const QString &note) { return list->item(rowWith(note))->data(Qt::UserRole).toLongLong(); };
        const qint64 first = idWith(QStringLiteral("First note"));
        const qint64 second = idWith(QStringLiteral("Second note"));
        const qint64 third = idWith(QStringLiteral("Third note"));
        list->clearSelection();
        list->item(rowWith(QStringLiteral("First note")))->setSelected(true);
        list->item(rowWith(QStringLiteral("Third note")))->setSelected(true);
        QList<qint64> chosen = reader->chosenAnnotations();
        std::sort(chosen.begin(), chosen.end());
        QList<qint64> expected{first, third};
        std::sort(expected.begin(), expected.end());
        QCOMPARE(chosen, expected);

        // The menu offers what makes sense for several, and no editing of one note.
        QMenu menu;
        reader->addChosenAnnotationActions(&menu);
        QStringList offered;
        for (QAction *action : menu.actions())
            offered << action->text();
        QVERIFY(!offered.contains(QStringLiteral("Edit note...")));
        QVERIFY(offered.contains(QStringLiteral("Colour")));
        QVERIFY(offered.contains(QStringLiteral("Copy 2")));
        QVERIFY(offered.contains(QStringLiteral("Export 2 to Markdown...")));
        QVERIFY(offered.contains(QStringLiteral("Delete 2...")));

        // A colour for both, which leaves the other alone and keeps them chosen.
        const QString before = annotations.get(second)->colour;
        QMenu *colours = nullptr;
        for (QAction *action : menu.actions()) {
            if (action->menu())
                colours = action->menu();
        }
        QVERIFY(colours);
        colours->actions().at(3)->trigger();
        QCOMPARE(annotations.get(first)->colour.toLower(), annotationColours().at(3).toLower());
        QCOMPARE(annotations.get(third)->colour.toLower(), annotationColours().at(3).toLower());
        QCOMPARE(annotations.get(second)->colour, before);
        QCOMPARE(reader->chosenAnnotations().size(), 2);
        QMenu again;
        reader->addChosenAnnotationActions(&again);
        for (QAction *action : again.actions()) {
            if (action->menu())
                QVERIFY(action->menu()->actions().at(3)->isChecked());
        }

        // Copying and exporting take only the chosen ones.
        const QString text = reader->annotationsAsText(reader->chosenAnnotations());
        QVERIFY(text.contains(QStringLiteral("First note")));
        QVERIFY(text.contains(QStringLiteral("Third note")));
        QVERIFY(!text.contains(QStringLiteral("Second note")));
        QVERIFY(text.contains(QStringLiteral("findme")));
        reader->copyAnnotations(reader->chosenAnnotations());
        QCOMPARE(QGuiApplication::clipboard()->text(), text);
        const QString out = m_dir->filePath(QStringLiteral("chosen.md"));
        QVERIFY(reader->exportAnnotations(out, reader->chosenAnnotations()));
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString exported = QString::fromUtf8(file.readAll());
        QVERIFY(exported.contains(QStringLiteral("First note")));
        QVERIFY(exported.contains(QStringLiteral("Third note")));
        QVERIFY(!exported.contains(QStringLiteral("Second note")));

        // And deleting them leaves the other.
        reader->deleteAnnotations(reader->chosenAnnotations());
        QVERIFY(!annotations.get(first).has_value());
        QVERIFY(!annotations.get(third).has_value());
        QVERIFY(annotations.get(second).has_value());
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Second note")));
    }

    void exportingToMarkdownWritesTheChosenFileOnly()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        ReaderTab *reader = openReader(id);
        const auto [page, rect] = findWord(reader, QStringLiteral("findme"));
        selectRect(reader, page, rect);
        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight, QStringLiteral("An exported note"));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);

        const QString out = m_dir->filePath(QStringLiteral("notes.md"));
        QVERIFY(reader->exportAnnotations(out));
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QStringLiteral("## Page 2")));
        QVERIFY(text.contains(QStringLiteral("> ")));
        QVERIFY(text.contains(QStringLiteral("findme")));
        QVERIFY(text.contains(QStringLiteral("An exported note")));
    }

    void anArticlesHighlightsShowInItsVolume()
    {
        const qint64 volume = importSample(QStringLiteral("reader_doc.pdf"));
        Publication record = *m_session->publications().get(volume);
        record.type = PublicationType::JournalIssue;
        m_session->publications().update(record);
        ArticleDraft draft;
        draft.title = QStringLiteral("An Article");
        draft.firstPage = 2;
        draft.lastPage = 2;
        const qint64 article = createArticles(m_session->database(), volume, {draft}, false).first();

        ReaderTab *reader = openReader(article);
        const auto [page, rect] = findWord(reader, QStringLiteral("findme"));
        selectRect(reader, page, rect);
        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);

        AnnotationRepository annotations(m_session->database());
        QCOMPARE(annotations.forPublication(article).size(), 1);
        QCOMPARE(annotations.forPublication(volume).size(), 0);

        // The same contents, seen as the whole volume.
        ReaderTab *volumeReader = openReader(volume);
        QTRY_COMPARE_WITH_TIMEOUT(volumeReader->pageView()->marksOn(1).size(), 1, kLongWaitMs);
    }

    void highlightsFromAnotherVersionAreKeptAndSaidToBe()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        AnnotationRepository annotations(m_session->database());
        Annotation old;
        old.publicationId = id;
        old.contentHash = QStringLiteral("an earlier version of the file");
        AnnotationPart part;
        part.page = 1;
        part.text = QStringLiteral("words from before");
        part.rects = {QRectF(10, 10, 100, 12)};
        old.parts = {part};
        annotations.create(old);

        ReaderTab *reader = openReader(id);
        QTRY_COMPARE_WITH_TIMEOUT(reader->annotationList()->count(), 1, kLongWaitMs);
        QVERIFY(reader->annotationList()->item(0)->text().contains(QStringLiteral("words from before")));
        QVERIFY(reader->annotationList()->item(0)->text().contains(QStringLiteral("another copy"), Qt::CaseInsensitive));
        // It may not line up, so it isn't drawn.
        QTest::qWait(300);
        QVERIFY(reader->pageView()->marksOn(1).isEmpty());
    }

    // ---- Reflowable text, in the reader ----

    void theDetailsSayHowManyThereAre()
    {
        const qint64 id = importSample(QStringLiteral("reader_doc.pdf"));
        AnnotationRepository annotations(m_session->database());
        Annotation annotation;
        annotation.publicationId = id;
        annotation.contentHash = QStringLiteral("hash");
        annotations.create(annotation);
        m_window->details()->showPublication(id);
        bool mentioned = false;
        for (const QLabel *label : m_window->details()->findChildren<QLabel *>())
            mentioned = mentioned || label->text() == QStringLiteral("Highlights and notes");
        QVERIFY(mentioned);
    }

    void aHighlightFollowsItsWordsThroughAFontChange()
    {
        const qint64 id = importSample(QStringLiteral("kf8_frankenstein.azw3"));
        ReaderTab *reader = openReader(id);
        QVERIFY(reader->isReflowable());
        const auto [page, rect] = findWord(reader, QStringLiteral("Geneva"));
        selectRect(reader, page, rect);
        QVERIFY(reader->pageView()->selectedText().contains(QStringLiteral("Geneva")));

        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight, QStringLiteral("A place"));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);

        AnnotationRepository annotations(m_session->database());
        const Annotation saved = annotations.forPublication(id).first();
        QVERIFY(!saved.parts.first().place.isEmpty());
        QVERIFY(saved.parts.first().rects.isEmpty()); // Screen positions wouldn't last.
        QTRY_COMPARE_WITH_TIMEOUT(reader->markedPages(), QList<int>{page}, kLongWaitMs);

        // A bigger font moves the words to a later page, and the mark goes with them.
        reader->changeFontSize(6);
        QTRY_VERIFY_WITH_TIMEOUT(!reader->markedPages().isEmpty() && reader->markedPages().first() > page, kLongWaitMs);
        const int bigger = reader->markedPages().first();
        const QRectF moved = reader->pageView()->marksOn(bigger).first().rects.first();
        QVERIFY(selectedTextAt(reader, bigger, moved).contains(QStringLiteral("Geneva")));

        // And a restart, at that font size, finds it too.
        reader = reopenReader(id);
        QTRY_VERIFY_WITH_TIMEOUT(!reader->markedPages().isEmpty(), kLongWaitMs);
        const int after = reader->markedPages().first();
        QVERIFY(selectedTextAt(reader, after, reader->pageView()->marksOn(after).first().rects.first()).contains(QStringLiteral("Geneva")));
    }

    void aRepeatedWordIsTheRightOneOfThem()
    {
        const qint64 id = importSample(QStringLiteral("kf8_frankenstein.azw3"));
        ReaderTab *reader = openReader(id);

        // "the" is on every page, more than once, and this takes the second on a page.
        QSignalSpy hits(reader->client(), &DocumentClient::searchHits);
        reader->searchBox()->setText(QStringLiteral("the"));
        reader->applySearch();
        QTRY_VERIFY_WITH_TIMEOUT(!hits.isEmpty(), kLongWaitMs);
        int page = -1;
        QList<QRectF> rects;
        for (int i = 0; i < hits.size() && rects.size() < 3; ++i) {
            page = hits.at(i).at(0).toInt();
            rects = hits.at(i).at(1).value<QList<QRectF>>();
        }
        QVERIFY(rects.size() >= 3);
        reader->pageView()->setSelection(page, {rects.at(1)}, QStringLiteral("the"));

        QSignalSpy changed(reader, &ReaderTab::annotationsChanged);
        reader->annotateSelection(AnnotationType::Highlight);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), kLongWaitMs);
        QCOMPARE(AnnotationRepository(m_session->database()).forPublication(id).first().parts.first().occurrence, 1);
        QTRY_COMPARE_WITH_TIMEOUT(reader->pageView()->marksOn(page).size(), 1, kLongWaitMs);
        QCOMPARE(reader->pageView()->marksOn(page).first().rects.first(), rects.at(1));
    }

    void wordsThatCantBeFoundAreStillListed()
    {
        const qint64 id = importSample(QStringLiteral("reader_nav.epub"));
        AnnotationRepository annotations(m_session->database());
        Annotation lost;
        lost.publicationId = id;
        lost.contentHash = m_session->files().filesFor(id).first().sha256;
        AnnotationPart part;
        part.place = QStringLiteral("0:0");
        part.text = QStringLiteral("words that are not in this book at all");
        lost.parts = {part};
        annotations.create(lost);

        ReaderTab *reader = openReader(id);
        QTRY_COMPARE_WITH_TIMEOUT(reader->annotationList()->count(), 1, kLongWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(reader->annotationList()->item(0)->text().contains(QStringLiteral("can't be found"), Qt::CaseInsensitive), kLongWaitMs);
        QVERIFY(reader->markedPages().isEmpty());
    }

private:
    qint64 importSample(const QString &name)
    {
        QFile::copy(sample(name), QDir(m_docs).filePath(name));
        m_window = std::make_unique<MainWindow>(*m_session);
        m_window->show();
        m_window->addFolder(m_docs);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kLongWaitMs && (m_session->query().count({}) < 1 || m_window->importThread()->isBusy()))
            QTest::qWait(50);
        const QList<qint64> ids = m_session->query().ids({}, {});
        return ids.isEmpty() ? 0 : ids.first();
    }

    ReaderTab *openReader(qint64 id)
    {
        ReaderTab *reader = m_window->openPublication(id);
        if (!reader)
            return nullptr;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kLongWaitMs && !reader->isReady())
            QTest::qWait(20);
        return reader;
    }

    void closeReaders()
    {
        while (m_window->documentTabs()->count() > 1)
            m_window->closeDocumentTab(1);
    }

    ReaderTab *reopenReader(qint64 id)
    {
        closeReaders();
        return openReader(id);
    }

    // Finds a word with the reader's own search, skipping the opening pages so that there's room to move.
    std::pair<int, QRectF> findWord(ReaderTab *reader, const QString &word)
    {
        QSignalSpy hits(reader->client(), &DocumentClient::searchHits);
        reader->searchBox()->setText(word);
        reader->applySearch();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kLongWaitMs && hits.isEmpty())
            QTest::qWait(20);
        if (hits.isEmpty())
            return {-1, QRectF()};
        const int page = hits.first().at(0).toInt();
        const QRectF rect = hits.first().at(1).value<QList<QRectF>>().first();
        reader->searchBox()->clear();
        reader->applySearch();
        return {page, rect};
    }

    void selectRect(ReaderTab *reader, int page, const QRectF &rect)
    {
        reader->pageView()->clearSelection();
        reader->client()->requestSelection(page, QPointF(rect.left() + 0.5, rect.center().y()), QPointF(rect.right() - 0.5, rect.center().y()));
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kLongWaitMs && !reader->pageView()->hasSelection())
            QTest::qWait(20);
    }

    QString selectedTextAt(ReaderTab *reader, int page, const QRectF &rect)
    {
        selectRect(reader, page, rect);
        const QString text = reader->pageView()->selectedText();
        reader->pageView()->clearSelection();
        return text;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_docs;
    std::unique_ptr<LibrarySession> m_session;
    std::unique_ptr<MainWindow> m_window;
    std::unique_ptr<QTimer> m_boxWatcher;
    QStringList m_unexpectedBoxes;
};

QTEST_MAIN(AnnotationsTest)
#include "test_annotations.moc"
