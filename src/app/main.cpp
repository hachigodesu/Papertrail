#include "ui/LibrarySession.h"
#include "ui/MainWindow.h"

#include <QApplication>
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

#ifdef Q_OS_WIN
    // The Windows 10 style has no dark version, so dark mode needs a style that does.
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
        && QApplication::style()->name().compare(QStringLiteral("windows11"), Qt::CaseInsensitive) != 0) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
#endif

    std::unique_ptr<papertrail::ui::LibrarySession> session;
    try {
        session = std::make_unique<papertrail::ui::LibrarySession>(papertrail::ui::LibraryPaths::standard());
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

    papertrail::ui::MainWindow window(*session);
    window.show();

    return QApplication::exec();
}
