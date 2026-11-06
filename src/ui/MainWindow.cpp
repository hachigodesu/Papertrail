#include "ui/MainWindow.h"

#include <QLabel>

namespace papertrail::ui {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Papertrail"));
    resize(1280, 800);

    auto *placeholder = new QLabel(
        tr("Papertrail %1").arg(QStringLiteral(PAPERTRAIL_VERSION)), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);
}

} // namespace papertrail::ui
