// Saves pictures of the library window, so its appearance can be checked without a screen.
//
//   ui-preview --db LIBRARY.db --cache CACHE_DIR --out OUTPUT_DIR [--dark] [--search TEXT]
//
// Run it with QT_QPA_PLATFORM=offscreen. It works on a copy of the library or a generated one,
// since opening a library can change its settings.

#include "ui/DetailsPanel.h"
#include "ui/PageView.h"
#include "ui/ReaderTab.h"
#include "ui/FilterSidebar.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/MetadataEditor.h"
#include "ui/OnlineLookupDialog.h"
#include "ui/PublicationListModel.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QLineEdit>
#include <QListView>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTabWidget>
#include <QTimer>

using namespace papertrail;

namespace {

void wait(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("db"), QStringLiteral("Library database."), QStringLiteral("file")});
    parser.addOption({QStringLiteral("cache"), QStringLiteral("Cache folder."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("out"), QStringLiteral("Folder for the pictures."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("dark"), QStringLiteral("Use the dark colour scheme.")});
    parser.addOption({QStringLiteral("search"), QStringLiteral("Search text for the search picture."), QStringLiteral("text"),
                      QStringLiteral("year:>2000")});
    parser.addOption({QStringLiteral("worker"), QStringLiteral("Path to papertrail-worker."), QStringLiteral("file")});
    parser.addOption({QStringLiteral("read"), QStringLiteral("Also picture the reader on this title."), QStringLiteral("title")});
    parser.process(app);

    if (parser.isSet(QStringLiteral("dark"))) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    }

    ui::LibraryPaths paths;
    paths.database = parser.value(QStringLiteral("db"));
    paths.cacheDirectory = parser.value(QStringLiteral("cache"));
    paths.workerPath = parser.isSet(QStringLiteral("worker")) ? parser.value(QStringLiteral("worker"))
                                                              : ui::LibraryPaths::standard().workerPath;
    ui::LibrarySession session(paths);
    session.settings().setValue(QStringLiteral("library.rescan_on_startup"), false);

    const QDir out(parser.value(QStringLiteral("out")));
    QDir().mkpath(out.path());

    ui::MainWindow window(session);
    window.resize(1400, 900);
    window.show();
    const auto save = [&](const QString &name) {
        wait(1200);
        window.grab().save(out.filePath(name + QStringLiteral(".png")));
    };

    window.setViewMode(ui::MainWindow::ViewMode::Grid);
    save(QStringLiteral("grid"));

    if (window.model()->rowCount() > 0) {
        window.gridView()->setCurrentIndex(window.model()->index(0, 0));
        save(QStringLiteral("grid-selected"));

        window.setViewMode(ui::MainWindow::ViewMode::List);
        save(QStringLiteral("list"));
    }

    window.searchBox()->setText(parser.value(QStringLiteral("search")));
    save(QStringLiteral("search"));
    window.searchBox()->clear();
    wait(500);

    if (const auto id = window.details()->currentId()) {
        ui::MetadataEditor editor(*session.publications().get(*id));
        editor.show();
        wait(300);
        editor.grab().save(out.filePath(QStringLiteral("editor.png")));
    }

    {
        ui::OnlineLookupDialog dialog(QStringLiteral("Look up details for these files online?"), QStringLiteral("online_review"), QString(), true);
        dialog.grab().save(out.filePath(QStringLiteral("lookup-dialog.png")));
    }

    // The reader, for anything whose title is given.
    const QStringList readerTitles = parser.values(QStringLiteral("read"));
    for (const qint64 id : session.query().ids({}, {})) {
        const auto publication = session.publications().get(id);
        if (!publication || !readerTitles.contains(publication->title))
            continue;
        ui::ReaderTab *reader = window.openPublication(id);
        if (!reader)
            continue;
        const QString name = publication->title.left(12).replace(u' ', u'-');
        save(QStringLiteral("reader-") + name);
        reader->searchBox()->setText(QStringLiteral("page"));
        reader->applySearch();
        save(QStringLiteral("reader-search-") + name);
        reader->searchBox()->clear();
        reader->applySearch();
        reader->pageView()->setLayoutMode(ui::PageView::LayoutMode::TwoPages);
        reader->pageView()->setZoomMode(ui::PageView::ZoomMode::FitPage);
        reader->pageView()->setColours(ui::PageView::Colours::Sepia);
        save(QStringLiteral("reader-sepia-") + name);
        reader->pageView()->setLayoutMode(ui::PageView::LayoutMode::Continuous);
        reader->pageView()->setColours(ui::PageView::Colours::Dark);
        reader->findChild<QTabWidget *>()->setCurrentIndex(1);
        reader->setSidePanelVisible(true);
        save(QStringLiteral("reader-dark-") + name);
    }
    return 0;
}
