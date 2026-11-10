#include "ui/ReferenceImportDialog.h"

#include "core/db/Backups.h"
#include "core/db/Database.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>
#include <memory>

namespace papertrail::ui {

using transfer::ReferenceMatch;

namespace {

// A reference manager's export is text, so anything far larger than a library's worth isn't one.
constexpr qint64 kLargestFile = 200LL * 1024 * 1024;

QString count(int n, const QString &one, const QString &many)
{
    return n == 1 ? one : many.arg(QLocale().toString(n));
}

} // namespace

QString referenceMatchLabel(ReferenceMatch match)
{
    switch (match) {
    case ReferenceMatch::ByFile: return QObject::tr("Its file");
    case ReferenceMatch::ByIdentifier: return QObject::tr("Its DOI, ISBN or arXiv number");
    case ReferenceMatch::ByTitle: return QObject::tr("Its title");
    case ReferenceMatch::ByTitleUnsure: return QObject::tr("Its short title, to check");
    case ReferenceMatch::Repeated: return QObject::tr("An earlier entry's publication");
    case ReferenceMatch::KeptOut: return QObject::tr("Left alone: removed");
    case ReferenceMatch::None: return QObject::tr("Nothing here");
    }
    return {};
}

QString describeReferencePlan(const transfer::ReferencePlan &plan)
{
    const int matched = plan.count(ReferenceMatch::ByFile) + plan.count(ReferenceMatch::ByIdentifier)
                        + plan.count(ReferenceMatch::ByTitle) + plan.count(ReferenceMatch::ByTitleUnsure)
                        + plan.count(ReferenceMatch::Repeated);
    QStringList lines;
    lines << QObject::tr("%1 in this file. %2 publications in your library: %3 by their files, %4 by an identifier, and "
                         "%5 by their titles.")
                 .arg(count(int(plan.entries.size()), QObject::tr("1 entry"), QObject::tr("%1 entries")),
                      count(matched, QObject::tr("1 matches one of the"), QObject::tr("%1 match")))
                 .arg(plan.count(ReferenceMatch::ByFile))
                 .arg(plan.count(ReferenceMatch::ByIdentifier))
                 .arg(plan.count(ReferenceMatch::ByTitle) + plan.count(ReferenceMatch::ByTitleUnsure));
    if (const int unsure = plan.count(ReferenceMatch::ByTitleUnsure); unsure > 0)
        lines << count(unsure, QObject::tr("1 has only a short title to go by, so it'll be marked for review."),
                       QObject::tr("%1 have only a short title to go by, so they'll be marked for review."));
    if (const int none = plan.count(ReferenceMatch::None); none > 0)
        lines << count(none, QObject::tr("1 matches nothing here."), QObject::tr("%1 match nothing here."));
    if (const int kept = plan.count(ReferenceMatch::KeptOut); kept > 0)
        lines << count(kept, QObject::tr("1 is for something removed from the library, so it's left alone."),
                       QObject::tr("%1 are for things removed from the library, so they're left alone."));
    if (!plan.foldersOutside.isEmpty()) {
        lines << QObject::tr("Some name files in folders that aren't in your library, so they can't be matched by them. "
                             "Add the folders to match them by their files: %1")
                     .arg(plan.foldersOutside.mid(0, 5).join(QStringLiteral(", ")) + (plan.foldersOutside.size() > 5 ? QStringLiteral(", …") : QString()));
    }
    if (!plan.problems.isEmpty())
        lines << count(int(plan.problems.size()), QObject::tr("1 entry couldn't be read: %2"), QObject::tr("%1 entries couldn't be read, such as %2"))
                     .arg(plan.problems.first());
    lines << QObject::tr("Details from the file replace those guessed from documents or found online, but never anything you've "
                         "set yourself, and every change can be undone from a publication's details. A backup is made first.");
    return lines.join(QStringLiteral("\n\n"));
}

QString describeReferenceImport(const transfer::ReferenceImportReport &report)
{
    QStringList lines;
    lines << count(report.updated, QObject::tr("1 publication was given details or tags from the file."),
                   QObject::tr("%1 publications were given details or tags from the file."));
    if (report.unchanged > 0)
        lines << count(report.unchanged, QObject::tr("1 already had everything its entry had."),
                       QObject::tr("%1 already had everything their entries had."));
    if (report.added > 0)
        lines << count(report.added, QObject::tr("1 was added without a file. It joins its file if one with its DOI or title turns up."),
                       QObject::tr("%1 were added without files. Each joins its file if one with its DOI or title turns up."));
    if (report.notAdded > 0)
        lines << count(report.notAdded, QObject::tr("1 entry matched nothing, and was left out."),
                       QObject::tr("%1 entries matched nothing, and were left out."));
    if (report.toReview > 0)
        lines << count(report.toReview, QObject::tr("1 was matched by a short title, so it's marked for review."),
                       QObject::tr("%1 were matched by short titles, so they're marked for review."));
    if (report.notesLeftOut > 0)
        lines << count(report.notesLeftOut, QObject::tr("1 note wasn't brought in, since its publication already had one."),
                       QObject::tr("%1 notes weren't brought in, since their publications already had one."));
    return lines.join(u'\n');
}

ReferenceImportDialog::ReferenceImportDialog(const transfer::ReferencePlan &plan, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Import references"));
    resize(760, 560);
    auto *layout = new QVBoxLayout(this);
    m_summary = new QLabel(describeReferencePlan(plan), this);
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(int(plan.entries.size()), 3, this);
    m_table->setHorizontalHeaderLabels({tr("Entry"), tr("Matches"), tr("By")});
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    for (int row = 0; row < plan.entries.size(); ++row) {
        const transfer::PlannedReference &entry = plan.entries.at(row);
        // Table items show text as it is, so nothing from the file can become formatting.
        m_table->setItem(row, 0, new QTableWidgetItem(entry.entry.publication.title.left(300)));
        m_table->setItem(row, 1, new QTableWidgetItem(entry.matchedTitle.left(300)));
        m_table->setItem(row, 2, new QTableWidgetItem(referenceMatchLabel(entry.match)));
    }
    layout->addWidget(m_table, 1);

    const int none = plan.count(ReferenceMatch::None);
    m_addUnmatched = new QCheckBox(count(none, tr("Add the entry that matches nothing, without a file"),
                                         tr("Add the %1 entries that match nothing, without files")),
                                   this);
    m_addUnmatched->setEnabled(none > 0);
    layout->addWidget(m_addUnmatched);
    m_collection = new QCheckBox(tr("Put everything imported in a new collection called “%1”").arg(plan.name), this);
    layout->addWidget(m_collection);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *import = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    import->setDefault(true);
    import->setEnabled(!plan.entries.isEmpty());
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

transfer::ReferenceImportOptions ReferenceImportDialog::options() const
{
    transfer::ReferenceImportOptions options;
    options.addUnmatched = m_addUnmatched->isEnabled() && m_addUnmatched->isChecked();
    options.intoNewCollection = m_collection->isChecked();
    return options;
}

ReferenceImportActions::ReferenceImportActions(LibrarySession &session, QWidget *window)
    : QObject(window), m_session(session), m_window(window)
{
}

ReferenceImportActions::~ReferenceImportActions()
{
    if (m_thread)
        m_thread->wait();
}

void ReferenceImportActions::run(const QString &label, bool cancellable,
                                 const std::function<void(core::Database &, const std::function<bool()> &,
                                                          const std::function<void(int, int)> &)> &work,
                                 const std::function<void()> &finished)
{
    const QString databasePath = m_session.paths().database;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_progress = new QProgressDialog(label, cancellable ? tr("Cancel") : QString(), 0, 100, m_window);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(400);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    connect(m_progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });

    QPointer<ReferenceImportActions> self(this);
    m_thread = QThread::create([self, cancel, databasePath, work] {
        core::Database db(databasePath);
        work(db, [cancel] { return cancel->load(); }, [self](int done, int total) {
            const int percent = total > 0 ? done * 100 / total : 0;
            QMetaObject::invokeMethod(qApp, [self, percent] {
                if (self && self->m_progress)
                    self->m_progress->setValue(percent);
            }, Qt::QueuedConnection);
        });
    });
    connect(m_thread, &QThread::finished, this, [this, finished] {
        if (m_progress)
            m_progress->deleteLater();
        m_thread->deleteLater();
        m_thread = nullptr;
        finished();
    });
    m_thread->start();
}

void ReferenceImportActions::planFile(const QString &path,
                                      const std::function<void(const transfer::ReferencePlan &, const QString &)> &done)
{
    auto plan = std::make_shared<transfer::ReferencePlan>();
    auto problem = std::make_shared<QString>();
    const QString name = QFileInfo(path).completeBaseName();
    run(tr("Reading the references..."), false,
        [plan, problem, path, name](core::Database &db, const std::function<bool()> &, const std::function<void(int, int)> &) {
            try {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    *problem = file.errorString();
                    return;
                }
                if (file.size() > kLargestFile) {
                    *problem = QObject::tr("It's too large to be a list of references.");
                    return;
                }
                const citation::ParsedReferences parsed = citation::parseReferences(file.readAll());
                if (parsed.format == citation::ReferenceFormat::Unknown) {
                    *problem = QObject::tr("It isn't a BibTeX or RIS file.");
                    return;
                }
                *plan = transfer::planReferenceImport(db, parsed, name);
            } catch (const std::exception &error) {
                *problem = QString::fromUtf8(error.what());
            }
        },
        [plan, problem, done] { done(*plan, *problem); });
}

void ReferenceImportActions::apply(const transfer::ReferencePlan &plan, const transfer::ReferenceImportOptions &options,
                                   const std::function<void(bool, const transfer::ReferenceImportReport &, const QString &)> &done)
{
    auto report = std::make_shared<transfer::ReferenceImportReport>();
    auto problem = std::make_shared<QString>();
    auto ok = std::make_shared<bool>(false);
    const QString databasePath = m_session.paths().database;
    run(tr("Importing the references..."), true,
        [plan, options, report, problem, ok, databasePath](core::Database &db, const std::function<bool()> &cancelled,
                                                            const std::function<void(int, int)> &progress) {
            try {
                // With the other backups, so it can be restored the same way.
                core::makeBackup(db, databasePath, core::BackupKind::BeforeImport);
                *report = transfer::applyReferenceImport(db, plan, options, cancelled, progress);
                *ok = !report->cancelled;
                if (report->cancelled)
                    *problem = QObject::tr("It was cancelled, so nothing was changed.");
            } catch (const std::exception &error) {
                *problem = QString::fromUtf8(error.what());
            }
        },
        [this, report, problem, ok, done] {
            emit changed();
            done(*ok, *report, *problem);
        });
}

void ReferenceImportActions::askAndImport()
{
    if (isBusy())
        return;
    const QString path = QFileDialog::getOpenFileName(m_window, tr("Import references"),
                                                      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                      tr("References (*.bib *.bibtex *.ris *.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    planFile(path, [this](const transfer::ReferencePlan &plan, const QString &problem) {
        if (!problem.isEmpty()) {
            QMessageBox box(QMessageBox::Warning, tr("Import references"), tr("The file couldn't be read: %1").arg(problem),
                            QMessageBox::Ok, m_window);
            box.setTextFormat(Qt::PlainText);
            box.exec();
            return;
        }
        ReferenceImportDialog dialog(plan, m_window);
        if (dialog.exec() != QDialog::Accepted)
            return;
        apply(plan, dialog.options(), [this](bool ok, const transfer::ReferenceImportReport &report, const QString &problem) {
            showReport(ok, report, problem);
        });
    });
}

void ReferenceImportActions::showReport(bool ok, const transfer::ReferenceImportReport &report, const QString &problem)
{
    QMessageBox box(ok ? QMessageBox::Information : QMessageBox::Warning, tr("Import references"),
                    ok ? describeReferenceImport(report) : tr("The references weren't imported. %1").arg(problem), QMessageBox::Ok,
                    m_window);
    box.setTextFormat(Qt::PlainText);
    QPushButton *review = ok && report.toReview > 0 ? box.addButton(tr("Show those to review"), QMessageBox::ActionRole) : nullptr;
    box.exec();
    if (review && box.clickedButton() == review)
        emit reviewWanted();
}

} // namespace papertrail::ui
