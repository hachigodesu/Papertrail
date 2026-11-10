#pragma once

#include "core/repo/LibraryCheck.h"

#include <QDialog>
#include <QList>

#include <memory>

class QLabel;
class QPushButton;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;

namespace papertrail::ui {

class LibrarySession;

// Checks the library for what needs attention, on a connection of its own so the window never waits, and offers a fix
// for each kind of problem: files that are missing or can't be read, publications waiting for files, files in no
// publication, and identifiers shared by two. Fixes only ever change Papertrail's records, never a file. Those the
// window has to do, such as combining publications, it's asked for by signal.
class LibraryCheckDialog : public QDialog {
    Q_OBJECT

public:
    explicit LibraryCheckDialog(LibrarySession &session, QWidget *parent = nullptr);
    ~LibraryCheckDialog() override;

    // Checks again, as after a fix.
    void recheck();
    bool isChecking() const { return m_thread != nullptr; }
    const core::LibraryCheckResult &result() const { return m_result; }

    // For tests: what's listed, and the buttons for the row chosen.
    QTreeWidget *list() const { return m_list; }
    QPushButton *button(const QString &name) const;

signals:
    void findFileRequested(qint64 publicationId);
    void scanRequested();
    void suggestedFilesRequested();
    void removeRequested(const QList<qint64> &publicationIds);
    void deleteDetailsRequested(const QList<qint64> &publicationIds);
    // The first is kept, and the others combined with it.
    void combineRequested(const QList<qint64> &publicationIds);
    void showRequested(qint64 publicationId);
    void showInFolderRequested(qint64 publicationId);
    // Something's changed that the library's list should show.
    void libraryChanged();
    void checked();

private:
    enum class Kind { Missing, Unreadable, Waiting, Unattached, Shared };

    void showResult(const core::LibraryCheckResult &result);
    void updateButtons();
    QTreeWidgetItem *current() const;
    Kind kindOf(const QTreeWidgetItem *item) const;
    QList<qint64> idsOf(const QTreeWidgetItem *item) const;
    qint64 fileOf(const QTreeWidgetItem *item) const;
    bool confirm(const QString &title, const QString &text, const QString &action);
    void fixed();

    LibrarySession &m_session;
    QTreeWidget *m_list = nullptr;
    QLabel *m_status = nullptr;
    QList<QPushButton *> m_buttons;
    QThread *m_thread = nullptr;
    bool m_again = false; // Asked to check again while checking.
    core::LibraryCheckResult m_result;
};

} // namespace papertrail::ui
