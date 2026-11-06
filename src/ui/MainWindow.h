#pragma once

#include <QMainWindow>

namespace papertrail::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
};

} // namespace papertrail::ui
