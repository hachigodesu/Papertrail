#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Papertrail"));
    QApplication::setApplicationVersion(QStringLiteral(PAPERTRAIL_VERSION));

    papertrail::ui::MainWindow window;
    window.show();

    return QApplication::exec();
}
