#include "SelfTest.h"
#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMessageBox>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include <exception>
#include <memory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Papertrail"));
    QApplication::setApplicationVersion(QStringLiteral(PAPERTRAIL_VERSION));
    QIcon icon;
    for (const int size : {16, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/papertrail-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);
    // Lets Wayland match the window to its desktop file and icon, which every package names by the app id.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.hachigodesu.Papertrail"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QApplication::translate("main", "A library manager and reader for academic publications."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption library(QStringLiteral("library"),
                                     QApplication::translate("main", "Opens the library in <folder>, making one there if there isn't one."),
                                     QApplication::translate("main", "folder"));
    const QCommandLineOption selfTest(QStringLiteral("self-test"),
                                      QApplication::translate("main", "Checks this copy works, without opening a window or your library, "
                                                                      "and exits."));
    const QCommandLineOption report(QStringLiteral("report"),
                                    QApplication::translate("main", "Writes the self-test's findings to <file> rather than the console."),
                                    QApplication::translate("main", "file"));
    parser.addOptions({library, selfTest, report});
    parser.process(app);

    if (parser.isSet(selfTest))
        return papertrail::app::runSelfTest(parser.value(report)) ? 0 : 1;

    const papertrail::ui::LibraryPaths paths = parser.isSet(library) ? papertrail::ui::LibraryPaths::inFolder(parser.value(library))
                                                                     : papertrail::ui::LibraryPaths::forThisCopy();
    std::unique_ptr<papertrail::ui::LibrarySession> session;
    try {
        session = std::make_unique<papertrail::ui::LibrarySession>(paths);
    } catch (const papertrail::ui::LibraryInUse &) {
        QMessageBox::information(nullptr, QStringLiteral("Papertrail"),
                                 QApplication::translate("main", "Papertrail is already open."));
        return 0;
    } catch (const std::exception &error) {
        QMessageBox::critical(nullptr, QStringLiteral("Papertrail"),
                              QApplication::translate("main", "Your library couldn't be opened:\n\n%1")
                                  .arg(QString::fromUtf8(error.what())));
        return 1;
    }

    // The library's theme, or the system's if it can't be read.
    QString theme;
    try {
        theme = session->settings().value(QStringLiteral("appearance.theme")).toString();
    } catch (const std::exception &) {
    }
    papertrail::ui::applyTheme(papertrail::ui::themeFromSetting(theme));

    papertrail::ui::MainWindow window(*session);
    window.show();

    return QApplication::exec();
}
