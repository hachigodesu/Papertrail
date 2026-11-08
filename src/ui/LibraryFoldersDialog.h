#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;
class QTreeWidget;

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
    void removedListRequested();

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
    QLabel *m_removedNote = nullptr;
    QPushButton *m_showRemoved = nullptr;

public:
    QPushButton *showRemovedButton() const { return m_showRemoved; }
};

// Publications the user removed, kept out of the library, any of which can be brought back.
class RemovedPublicationsDialog : public QDialog {
    Q_OBJECT

public:
    explicit RemovedPublicationsDialog(LibrarySession &session, QWidget *parent = nullptr);

    QTreeWidget *list() const { return m_list; }
    QPushButton *bringBackButton() const { return m_bringBack; }
    // How many files will be read again by the next scan.
    int filesBroughtBack() const { return m_files; }

    void bringBackChosen();

private:
    void reload();

    LibrarySession &m_session;
    QTreeWidget *m_list = nullptr;
    QPushButton *m_bringBack = nullptr;
    int m_files = 0;
};

} // namespace papertrail::ui
