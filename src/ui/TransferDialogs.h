#pragma once

#include "transfer/LibraryTransfer.h"

#include <QDialog>
#include <QObject>

#include <atomic>
#include <memory>

class QCheckBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QThread;
class QWidget;

namespace papertrail::ui {

class LibrarySession;

// Chooses what to bring in from a library export, after saying what's in it.
class ImportLibraryDialog : public QDialog {
    Q_OBJECT

public:
    ImportLibraryDialog(const transfer::Bundle &bundle, const QString &libraryFolder, QWidget *parent = nullptr);

    transfer::ImportOptions options() const;

    QCheckBox *detailsBox() const { return m_details; }
    QCheckBox *tagsBox() const { return m_tags; }
    QCheckBox *annotationsBox() const { return m_annotations; }
    QCheckBox *readingBox() const { return m_reading; }
    QCheckBox *settingsBox() const { return m_settings; }
    QRadioButton *mergeButton() const { return m_merge; }
    QRadioButton *replaceButton() const { return m_replace; }
    QPushButton *importButton() const { return m_import; }
    QLabel *summaryLabel() const { return m_summary; }

signals:
    // The user wants to look at the library's folders first, since files are found in them.
    void foldersRequested();

private:
    void updateButton();

    QString m_libraryFolder;
    QLabel *m_summary = nullptr;
    QCheckBox *m_details = nullptr;
    QCheckBox *m_tags = nullptr;
    QCheckBox *m_annotations = nullptr;
    QCheckBox *m_reading = nullptr;
    QCheckBox *m_settings = nullptr;
    QRadioButton *m_merge = nullptr;
    QRadioButton *m_replace = nullptr;
    QPushButton *m_import = nullptr;
};

// Files suggested for imported publications, for the user to accept or reject one at a time.
class FileProposalsDialog : public QDialog {
    Q_OBJECT

public:
    FileProposalsDialog(LibrarySession &session, QWidget *parent = nullptr);

    QTableWidget *table() const { return m_table; }
    bool acceptRow(int row);
    bool rejectRow(int row);
    int changed() const { return m_changed; }

private:
    void reload();

    LibrarySession &m_session;
    QList<transfer::FileProposal> m_proposals;
    QTableWidget *m_table = nullptr;
    QLabel *m_message = nullptr;
    QPushButton *m_accept = nullptr;
    QPushButton *m_reject = nullptr;
    int m_changed = 0;
};

// Exporting and importing a whole library, each on a thread and a database connection of its own.
class LibraryTransferActions : public QObject {
    Q_OBJECT

public:
    LibraryTransferActions(LibrarySession &session, QWidget *window);
    ~LibraryTransferActions() override;

    void askAndExport();
    void startExport(const QString &path);

    // Asks for a file and what to bring in. `beforeImport` runs once the user has confirmed, to quieten
    // anything else that writes to the library.
    void askAndImport(const std::function<void()> &beforeImport);
    void startImport(const transfer::Bundle &bundle, const transfer::ImportOptions &options);

    bool isBusy() const { return m_thread != nullptr; }

    // Where Replace keeps the library as it was, beside the library itself.
    QString newBackupPath() const;

signals:
    void message(const QString &text);
    void exportFinished(bool ok, const QString &message);
    void importFinished(bool ok, const papertrail::transfer::ImportReport &report, const QString &message);
    void foldersRequested();

private:
    void start(const QString &label, const std::function<QString(core::Database &, const transfer::Progress &, bool &)> &work,
               const std::function<void(bool, const QString &)> &done);

    LibrarySession &m_session;
    QWidget *m_window;
    QThread *m_thread = nullptr;
    QProgressDialog *m_progress = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<transfer::ImportReport> m_lastReport;
};

// A plain-language account of what an import did.
QString describeImport(const transfer::ImportReport &report);

} // namespace papertrail::ui

Q_DECLARE_METATYPE(papertrail::transfer::ImportReport)
