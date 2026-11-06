// Saves pictures of the library window, so its appearance can be checked without a screen.
//
//   ui-preview --db LIBRARY.db --cache CACHE_DIR --out OUTPUT_DIR [--dark] [--search TEXT]
//
// Run it with QT_QPA_PLATFORM=offscreen. It works on a copy of the library or a generated one,
// since opening a library can change its settings.

#include "ui/DetailsPanel.h"
#include "ui/FilterSidebar.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/MetadataEditor.h"
#include "ui/PublicationListModel.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QLineEdit>
#include <QListView>
#include <QStyleFactory>
#include <QStyleHints>
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
    parser.process(app);

    if (parser.isSet(QStringLiteral("dark"))) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    }

    ui::LibraryPaths paths;
    paths.database = parser.value(QStringLiteral("db"));
    paths.cacheDirectory = parser.value(QStringLiteral("cache"));
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
    return 0;
}
