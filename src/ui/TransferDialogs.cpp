#include "ui/TransferDialogs.h"

#include "core/db/Database.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace transfer;

// ---------------------------------------------------------------------------------------------------- Import dialog

ImportLibraryDialog::ImportLibraryDialog(const Bundle &bundle, const QString &libraryFolder, QWidget *parent)
    : QDialog(parent), m_libraryFolder(libraryFolder)
{
    setWindowTitle(tr("Import a library"));
    auto *layout = new QVBoxLayout(this);

    const QJsonObject counts = bundle.manifest.value(QStringLiteral("counts")).toObject();
    const QDateTime exported = QDateTime::fromString(bundle.manifest.value(QStringLiteral("exportedAt")).toString(), Qt::ISODate);
    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    m_summary->setText(tr("Exported %1, with %2, %3 tags, %4 collections and %5 highlights and notes.")
                           .arg(exported.isValid() ? QLocale().toString(exported.toLocalTime(), QLocale::LongFormat) : tr("at an unknown time"),
                                publicationCount(qint64(bundle.publications.size())))
                           .arg(bundle.tags.size())
                           .arg(bundle.collections.size())
                           .arg(bundle.annotations.size()));
    layout->addWidget(m_summary);

    auto *files = new QLabel(tr("Its documents aren't in it. They're found by their contents in your library's folders, "
                                "wherever they are and whatever they're called. Anything not found yet is kept, and connected "
                                "when its file turns up."),
                             this);
    files->setWordWrap(true);
    layout->addWidget(files);
    auto *folders = new QPushButton(tr("Library folders..."), this);
    connect(folders, &QPushButton::clicked, this, &ImportLibraryDialog::foldersRequested);
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(folders);
    folderRow->addStretch();
    layout->addLayout(folderRow);

    auto *parts = new QGroupBox(tr("Bring in"), this);
    auto *partsLayout = new QVBoxLayout(parts);
    m_details = new QCheckBox(tr("Publications and their details, read status, ratings, notes and summaries"), parts);
    m_tags = new QCheckBox(tr("Tags and collections"), parts);
    m_annotations = new QCheckBox(tr("Highlights and notes in documents"), parts);
    m_reading = new QCheckBox(tr("Places in documents"), parts);
    m_settings = new QCheckBox(tr("Settings"), parts);
    for (QCheckBox *box : {m_details, m_tags, m_annotations, m_reading, m_settings}) {
        box->setChecked(true);
        partsLayout->addWidget(box);
        connect(box, &QCheckBox::toggled, this, &ImportLibraryDialog::updateButton);
    }
    layout->addWidget(parts);

    auto *mode = new QGroupBox(tr("How"), this);
    auto *modeLayout = new QVBoxLayout(mode);
    m_merge = new QRadioButton(tr("Add to this library"), mode);
    m_merge->setChecked(true);
    auto *mergeNote = new QLabel(tr("Where both have a publication, the more recently edited is kept, and the other can be undone. "
                                    "Nothing here is removed."),
                                 mode);
    mergeNote->setWordWrap(true);
    mergeNote->setContentsMargins(20, 0, 0, 6);
    m_replace = new QRadioButton(tr("Replace this library with it"), mode);
    auto *replaceNote = new QLabel(tr("This library becomes a copy of the export. A backup of it is kept in %1 first.")
                                       .arg(QDir::toNativeSeparators(m_libraryFolder)),
                                   mode);
    replaceNote->setWordWrap(true);
    replaceNote->setContentsMargins(20, 0, 0, 0);
    modeLayout->addWidget(m_merge);
    modeLayout->addWidget(mergeNote);
    modeLayout->addWidget(m_replace);
    modeLayout->addWidget(replaceNote);
    layout->addWidget(mode);
    connect(m_replace, &QRadioButton::toggled, this, &ImportLibraryDialog::updateButton);

    auto *closing = new QLabel(tr("Open documents are closed while it imports. Your files aren't changed."), this);
    closing->setWordWrap(true);
    layout->addWidget(closing);

    auto *buttons = new QDialogButtonBox(this);
    m_import = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    updateButton();
}

void ImportLibraryDialog::updateButton()
{
    const bool any = m_details->isChecked() || m_tags->isChecked() || m_annotations->isChecked() || m_reading->isChecked()
        || m_settings->isChecked();
    m_import->setEnabled(any);
    m_import->setText(m_replace->isChecked() ? tr("Back up and replace") : tr("Import"));
}

ImportOptions ImportLibraryDialog::options() const
{
    ImportOptions options;
    options.mode = m_replace->isChecked() ? ImportMode::Replace : ImportMode::Merge;
    options.parts.details = m_details->isChecked();
    options.parts.tags = m_tags->isChecked();
    options.parts.annotations = m_annotations->isChecked();
    options.parts.readingPlaces = m_reading->isChecked();
    options.parts.settings = m_settings->isChecked();
    return options;
}

// ---------------------------------------------------------------------------------------------------- Suggestions

FileProposalsDialog::FileProposalsDialog(LibrarySession &session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(tr("Suggested files"));
    resize(820, 420);
    auto *layout = new QVBoxLayout(this);
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setTextFormat(Qt::PlainText);
    layout->addWidget(m_message);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({tr("Publication"), tr("Suggested file"), tr("Why")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    auto *note = new QLabel(tr("Accepting connects the file. If it's already another publication's, the two become one, keeping "
                               "the imported details. Highlights made on the other computer's copy are marked for checking, "
                               "since this copy's pages may not line up."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QHBoxLayout;
    m_accept = new QPushButton(tr("Accept"), this);
    m_reject = new QPushButton(tr("Reject"), this);
    auto *close = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_accept);
    buttons->addWidget(m_reject);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(m_accept, &QPushButton::clicked, this, [this] { acceptRow(m_table->currentRow()); });
    connect(m_reject, &QPushButton::clicked, this, [this] { rejectRow(m_table->currentRow()); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool chosen = m_table->currentRow() >= 0 && !m_table->selectedItems().isEmpty();
        m_accept->setEnabled(chosen);
        m_reject->setEnabled(chosen);
    });
    reload();
}

void FileProposalsDialog::reload()
{
    try {
        m_proposals = fileProposals(m_session.database());
    } catch (const std::exception &error) {
        m_proposals.clear();
        m_message->setText(tr("The suggestions couldn't be read: %1").arg(QString::fromUtf8(error.what())));
    }
    m_table->setRowCount(int(m_proposals.size()));
    for (int row = 0; row < m_proposals.size(); ++row) {
        const FileProposal &proposal = m_proposals.at(row);
        // Shown as they are, so text from a file can't become formatting.
        m_table->setItem(row, 0, new QTableWidgetItem(proposal.publicationTitle));
        m_table->setItem(row, 1, new QTableWidgetItem(QDir::toNativeSeparators(proposal.filePath)));
        m_table->setItem(row, 2, new QTableWidgetItem(proposal.reason == QLatin1StringView("identifier")
                                                           ? tr("Same DOI, ISBN or arXiv number")
                                                           : tr("Same name and size")));
    }
    if (m_proposals.isEmpty() && m_message->text().isEmpty())
        m_message->setText(tr("There are no suggested files to check."));
    else if (!m_proposals.isEmpty())
        m_message->setText(tr("These files might be imported publications' own, but their contents differ, such as another "
                              "download of the same paper. Choose one and accept or reject it."));
    m_accept->setEnabled(false);
    m_reject->setEnabled(false);
}

bool FileProposalsDialog::acceptRow(int row)
{
    if (row < 0 || row >= m_proposals.size())
        return false;
    try {
        acceptFileProposal(m_session.database(), m_proposals.at(row).id);
    } catch (const std::exception &error) {
        m_message->setText(tr("That couldn't be accepted: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
    ++m_changed;
    m_message->clear();
    reload();
    return true;
}

bool FileProposalsDialog::rejectRow(int row)
{
    if (row < 0 || row >= m_proposals.size())
        return false;
    try {
        rejectFileProposal(m_session.database(), m_proposals.at(row).id);
    } catch (const std::exception &error) {
        m_message->setText(tr("That couldn't be rejected: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
    ++m_changed;
    m_message->clear();
    reload();
    return true;
}

// ---------------------------------------------------------------------------------------------------- Actions

QString describeImport(const ImportReport &report)
{
    QStringList lines;
    lines << QCoreApplication::translate("LibraryTransfer", "%1 added, %2 updated, and %3 already here as they were.")
                 .arg(report.added)
                 .arg(report.updated)
                 .arg(report.unchanged);
    lines << QCoreApplication::translate("LibraryTransfer", "Files found by their contents: %1. Suggested files to check: %2. "
                                                            "Still waiting for their files: %3, which stay out of sight until "
                                                            "their files are found in the library's folders.")
                 .arg(report.linked)
                 .arg(report.proposed)
                 .arg(report.waiting);
    lines << QCoreApplication::translate("LibraryTransfer", "New tags: %1. New collections: %2. Highlights and notes added: %3, "
                                                            "updated: %4.")
                 .arg(report.tagsAdded)
                 .arg(report.collectionsAdded)
                 .arg(report.annotationsAdded)
                 .arg(report.annotationsUpdated);
    if (report.settings.applied > 0)
        lines << QCoreApplication::translate("LibraryTransfer", "%n setting(s) brought in.", nullptr, report.settings.applied);
    if (!report.settings.suggestedPaths.isEmpty())
        lines << QCoreApplication::translate("LibraryTransfer", "Folders from the other computer weren't applied, since they're "
                                                                "unlikely to exist here.");
    if (!report.backupPath.isEmpty())
        lines << QCoreApplication::translate("LibraryTransfer", "The library as it was is kept in %1.").arg(QDir::toNativeSeparators(report.backupPath));
    return lines.join(u'\n');
}

LibraryTransferActions::LibraryTransferActions(LibrarySession &session, QWidget *window)
    : QObject(window), m_session(session), m_window(window)
{
    qRegisterMetaType<ImportReport>();
}

LibraryTransferActions::~LibraryTransferActions()
{
    if (m_thread) {
        if (m_cancel)
            m_cancel->store(true);
        m_thread->wait();
    }
}

QString LibraryTransferActions::newBackupPath() const
{
    const QFileInfo library(m_session.paths().database);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = library.dir().filePath(QStringLiteral("library-before-import-%1.db").arg(stamp));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = library.dir().filePath(QStringLiteral("library-before-import-%1-%2.db").arg(stamp).arg(n));
    return path;
}

void LibraryTransferActions::start(const QString &label,
                                   const std::function<QString(core::Database &, const Progress &, bool &)> &work,
                                   const std::function<void(bool, const QString &)> &done)
{
    const QString databasePath = m_session.paths().database;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    m_progress = new QProgressDialog(label, tr("Cancel"), 0, 100, m_window);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(300);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    connect(m_progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });

    QPointer<LibraryTransferActions> self(this);
    m_thread = QThread::create([self, cancel, databasePath, work, done] {
        bool ok = false;
        QString text;
        try {
            core::Database db(databasePath);
            Progress progress;
            progress.cancelled = [cancel] { return cancel->load(); };
            progress.progress = [self](qint64 doneCount, qint64 total) {
                const int percent = total > 0 ? int(doneCount * 100 / total) : 0;
                QMetaObject::invokeMethod(qApp, [self, percent] {
                    if (self && self->m_progress)
                        self->m_progress->setValue(percent);
                }, Qt::QueuedConnection);
            };
            text = work(db, progress, ok);
        } catch (const std::exception &error) {
            text = QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(qApp, [self, ok, text, done] {
            if (!self)
                return;
            if (self->m_progress) {
                self->m_progress->deleteLater();
                self->m_progress = nullptr;
            }
            if (self->m_thread) {
                self->m_thread->wait();
                self->m_thread->deleteLater();
                self->m_thread = nullptr;
            }
            done(ok, text);
        }, Qt::QueuedConnection);
    });
    m_thread->start();
}

void LibraryTransferActions::askAndExport()
{
    if (isBusy())
        return;
    QMessageBox about(QMessageBox::Information, tr("Export the library"),
                      tr("This saves your library's details, tags, collections, highlights, notes, summaries, places in "
                         "documents and settings in one file, for importing on another computer.\n\n"
                         "Your documents aren't included: they're found again on the other computer by their contents. "
                         "The file isn't encrypted, and includes your notes, and the contact email for online lookups if "
                         "you've entered one. Your Claude API key is never included."),
                      QMessageBox::Ok | QMessageBox::Cancel, m_window);
    about.setTextFormat(Qt::PlainText);
    if (about.exec() != QMessageBox::Ok)
        return;
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString name = QStringLiteral("library-%1.%2").arg(QDate::currentDate().toString(Qt::ISODate), bundleSuffix());
    QString path = QFileDialog::getSaveFileName(m_window, tr("Export the library"), QDir(folder).filePath(name),
                                                tr("Papertrail library (*.%1)").arg(bundleSuffix()));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += u'.' + bundleSuffix();
    startExport(path);
}

void LibraryTransferActions::startExport(const QString &path)
{
    if (isBusy())
        return;
    start(
        tr("Exporting the library..."),
        [path](core::Database &db, const Progress &progress, bool &ok) {
            const ExportReport report = exportLibrary(db, path, progress);
            if (report.cancelled)
                return tr("The export was cancelled, and nothing was written.");
            ok = true;
            return tr("Exported %1 to %2.").arg(publicationCount(report.publications), QDir::toNativeSeparators(path));
        },
        [this](bool ok, const QString &text) {
            emit message(text);
            emit exportFinished(ok, text);
        });
}

void LibraryTransferActions::askAndImport(const std::function<void()> &beforeImport)
{
    if (isBusy())
        return;
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(m_window, tr("Import a library"), folder,
                                                      tr("Papertrail library (*.%1)").arg(bundleSuffix()));
    if (path.isEmpty())
        return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::optional<Bundle> bundle = readBundleFile(path, &error);
    QApplication::restoreOverrideCursor();
    if (!bundle) {
        QMessageBox box(QMessageBox::Warning, tr("Import a library"), error, QMessageBox::Ok, m_window);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    ImportLibraryDialog dialog(*bundle, QFileInfo(m_session.paths().database).absolutePath(), m_window);
    connect(&dialog, &ImportLibraryDialog::foldersRequested, this, &LibraryTransferActions::foldersRequested);
    if (dialog.exec() != QDialog::Accepted)
        return;
    ImportOptions options = dialog.options();
    if (options.mode == ImportMode::Replace)
        options.backupPath = newBackupPath();
    if (beforeImport)
        beforeImport();
    startImport(*bundle, options);
}

void LibraryTransferActions::startImport(const Bundle &bundle, const ImportOptions &options)
{
    if (isBusy())
        return;
    auto report = std::make_shared<ImportReport>();
    m_lastReport = report;
    // The bundle is copied for the thread, since the caller's may not last.
    auto copy = std::make_shared<Bundle>(bundle);
    start(
        tr("Importing the library..."),
        [copy, options, report](core::Database &db, const Progress &progress, bool &ok) {
            ImportOptions withProgress = options;
            withProgress.progress = progress;
            *report = importBundle(db, *copy, withProgress);
            if (report->cancelled)
                return tr("The import was cancelled, and nothing was changed.");
            ok = true;
            return describeImport(*report);
        },
        [this, report](bool ok, const QString &text) {
            emit message(ok ? tr("The library was imported.") : text);
            emit importFinished(ok, *report, text);
        });
}

} // namespace papertrail::ui
