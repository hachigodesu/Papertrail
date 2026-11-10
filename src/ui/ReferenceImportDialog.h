#pragma once

#include "transfer/ReferenceImport.h"

#include <QDialog>
#include <QObject>
#include <QPointer>

#include <functional>

class QCheckBox;
class QLabel;
class QProgressDialog;
class QTableWidget;
class QThread;

namespace papertrail::ui {

class LibrarySession;

// What importing another manager's references would do, entry by entry, with the choices it leaves to the user.
// Everything from the file is shown as plain text.
class ReferenceImportDialog : public QDialog {
    Q_OBJECT

public:
    ReferenceImportDialog(const transfer::ReferencePlan &plan, QWidget *parent = nullptr);

    transfer::ReferenceImportOptions options() const;

    QLabel *summaryLabel() const { return m_summary; }
    QTableWidget *table() const { return m_table; }
    QCheckBox *addUnmatchedBox() const { return m_addUnmatched; }
    QCheckBox *collectionBox() const { return m_collection; }

private:
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
    QCheckBox *m_addUnmatched = nullptr;
    QCheckBox *m_collection = nullptr;
};

QString referenceMatchLabel(transfer::ReferenceMatch match);
QString describeReferencePlan(const transfer::ReferencePlan &plan);
QString describeReferenceImport(const transfer::ReferenceImportReport &report);

// File > Import references: reading and matching on a thread, the dialog, a backup, then the changes on a thread,
// each with its own connection, so the window never waits.
class ReferenceImportActions : public QObject {
    Q_OBJECT

public:
    ReferenceImportActions(LibrarySession &session, QWidget *window);
    ~ReferenceImportActions() override;

    void askAndImport();
    bool isBusy() const { return m_thread != nullptr; }

    // The steps on their own, for tests. `done` is called on the window's thread.
    void planFile(const QString &path, const std::function<void(const transfer::ReferencePlan &, const QString &problem)> &done);
    void apply(const transfer::ReferencePlan &plan, const transfer::ReferenceImportOptions &options,
               const std::function<void(bool ok, const transfer::ReferenceImportReport &, const QString &problem)> &done);

signals:
    void changed();      // The library has been changed, so the window looks at it afresh.
    void reviewWanted(); // Show the publications marked for review.
    void message(const QString &text);

private:
    void run(const QString &label, bool cancellable, const std::function<void(core::Database &, const std::function<bool()> &,
                                                                              const std::function<void(int, int)> &)> &work,
             const std::function<void()> &finished);
    void showReport(bool ok, const transfer::ReferenceImportReport &report, const QString &problem);

    LibrarySession &m_session;
    QWidget *m_window;
    QThread *m_thread = nullptr;
    QPointer<QProgressDialog> m_progress;
};

} // namespace papertrail::ui
