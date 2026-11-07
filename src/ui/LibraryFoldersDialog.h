#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;

namespace papertrail::ui {

class LibrarySession;

// The folders that make up the library. Files stay where they are: this only says where to look.
class LibraryFoldersDialog : public QDialog {
    Q_OBJECT

public:
    explicit LibraryFoldersDialog(LibrarySession &session, QWidget *parent = nullptr);

    void reload();

    // Changing a folder while it's being imported would pull the files out from under the import.
    void setImportRunning(bool running);

signals:
    void addRequested();
    void rescanRequested(const QList<qint64> &rootIds);
    void changeLocationRequested(qint64 rootId);
    void removeRequested(const QList<qint64> &rootIds);
    void forgetRequested();
    void forgetWaitingRequested();

private:
public:
    // In the order they're listed. Several can be chosen, to scan or remove together.
    QList<qint64> selectedRoots() const;
    QTableWidget *table() const { return m_table; }
    QPushButton *removeButton() const { return m_remove; }
    QPushButton *changeButton() const { return m_change; }

private:
    qint64 selectedRoot() const;
    void updateButtons();

    LibrarySession &m_session;
    bool m_importRunning = false;
    QTableWidget *m_table = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_rescan = nullptr;
    QPushButton *m_change = nullptr;
    QPushButton *m_remove = nullptr;
    QLabel *m_hiddenNote = nullptr;
    QPushButton *m_forget = nullptr;
    QLabel *m_waitingNote = nullptr;
    QPushButton *m_forgetWaiting = nullptr;
};

} // namespace papertrail::ui
