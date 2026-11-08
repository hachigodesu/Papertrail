#pragma once

#include <QDialog>
#include <QString>
#include <QThread>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace papertrail::ui {

class LibrarySession;

// Makes the day's backup, if there isn't one yet, on its own connection so the window never waits for it.
class DailyBackupThread : public QThread {
    Q_OBJECT
public:
    DailyBackupThread(const QString &databasePath, QObject *parent = nullptr);

signals:
    void made(const QString &path);
    void failed(const QString &error);

protected:
    void run() override;

private:
    QString m_databasePath;
};

// Lists the library's backups, makes one by hand, and restores one, which the window does after quietening
// everything else.
class BackupsDialog : public QDialog {
    Q_OBJECT
public:
    explicit BackupsDialog(LibrarySession &session, QWidget *parent = nullptr);

    void reload();

    QTreeWidget *list() const { return m_list; }
    QCheckBox *dailyBox() const { return m_daily; }
    QPushButton *backUpButton() const { return m_backUp; }
    QPushButton *restoreButton() const { return m_restore; }

    // Restores the chosen backup without asking, for tests.
    void restoreChosen(bool ask = true);

signals:
    // The user has confirmed replacing the library with this backup.
    void restoreRequested(const QString &backupPath);

private:
    void backUpNow();
    void updateButtons();

    LibrarySession &m_session;
    QTreeWidget *m_list = nullptr;
    QCheckBox *m_daily = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_backUp = nullptr;
    QPushButton *m_restore = nullptr;
    QPushButton *m_show = nullptr;
};

} // namespace papertrail::ui
