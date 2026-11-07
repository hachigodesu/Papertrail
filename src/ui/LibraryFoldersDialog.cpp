#include "ui/LibraryFoldersDialog.h"

#include "ui/LibrarySession.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace papertrail::ui {

LibraryFoldersDialog::LibraryFoldersDialog(LibrarySession &session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(tr("Library folders"));
    resize(680, 360);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Papertrail indexes these folders. Your files stay where they are, and are never changed.")));

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({tr("Location"), tr("Files"), tr("Status")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &LibraryFoldersDialog::updateButtons);
    layout->addWidget(m_table, 1);

    m_note = new QLabel(tr("An import is running. Stop it to change or remove a folder."), this);
    m_note->hide();
    layout->addWidget(m_note);

    auto *hiddenRow = new QHBoxLayout;
    m_hiddenNote = new QLabel(this);
    m_hiddenNote->setWordWrap(true);
    m_forget = new QPushButton(tr("Delete their details..."), this);
    m_forget->setToolTip(tr("Deletes their tags, notes and reading places from Papertrail. Your files aren't touched."));
    hiddenRow->addWidget(m_hiddenNote, 1);
    hiddenRow->addWidget(m_forget);
    layout->addLayout(hiddenRow);
    connect(m_forget, &QPushButton::clicked, this, &LibraryFoldersDialog::forgetRequested);

    auto *waitingRow = new QHBoxLayout;
    m_waitingNote = new QLabel(this);
    m_waitingNote->setWordWrap(true);
    m_forgetWaiting = new QPushButton(tr("Delete them..."), this);
    m_forgetWaiting->setToolTip(tr("Deletes their details from Papertrail, so they're no longer waited for."));
    waitingRow->addWidget(m_waitingNote, 1);
    waitingRow->addWidget(m_forgetWaiting);
    layout->addLayout(waitingRow);
    connect(m_forgetWaiting, &QPushButton::clicked, this, &LibraryFoldersDialog::forgetWaitingRequested);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add folder..."), this);
    m_rescan = new QPushButton(tr("Scan again"), this);
    m_change = new QPushButton(tr("Change location..."), this);
    m_change->setToolTip(tr("For a folder that has moved. Its files keep their tags and notes."));
    m_remove = new QPushButton(tr("Remove"), this);
    m_remove->setToolTip(tr("Papertrail stops indexing the chosen folders. Nothing is deleted from your disk."));
    buttons->addWidget(add);
    buttons->addWidget(m_rescan);
    buttons->addWidget(m_change);
    buttons->addStretch();
    buttons->addWidget(m_remove);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &LibraryFoldersDialog::addRequested);
    connect(m_rescan, &QPushButton::clicked, this, [this] { emit rescanRequested(selectedRoots()); });
    connect(m_change, &QPushButton::clicked, this, [this] { emit changeLocationRequested(selectedRoot()); });
    connect(m_remove, &QPushButton::clicked, this, [this] { emit removeRequested(selectedRoots()); });

    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);

    reload();
}

void LibraryFoldersDialog::reload()
{
    const QList<qint64> selected = selectedRoots();
    const QList<core::LibraryRoot> roots = m_session.files().roots();
    m_table->clearSelection();
    const QHash<qint64, qint64> counts = m_session.files().fileCountsByRoot();

    m_table->setRowCount(int(roots.size()));
    for (int row = 0; row < roots.size(); ++row) {
        const core::LibraryRoot &root = roots.at(row);
        auto *location = new QTableWidgetItem(QDir::toNativeSeparators(root.path));
        location->setData(Qt::UserRole, root.id);
        location->setToolTip(location->text());
        m_table->setItem(row, 0, location);

        auto *files = new QTableWidgetItem(QString::number(counts.value(root.id)));
        files->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(row, 1, files);
        m_table->setItem(row, 2, new QTableWidgetItem(QFileInfo::exists(root.path) ? tr("Available") : tr("Not found")));
        if (selected.contains(root.id))
            m_table->selectionModel()->select(m_table->model()->index(row, 0),
                                              QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }

    const qint64 hidden = m_session.files().hiddenCount();
    const QString count = hidden == 1 ? tr("1 publication") : tr("%1 publications").arg(QLocale().toString(hidden));
    m_hiddenNote->setText(tr("Details of %1 from removed folders are being kept, out of sight. "
                             "Adding a folder again brings back the ones it holds.")
                              .arg(count));
    m_hiddenNote->setVisible(hidden > 0);
    m_forget->setVisible(hidden > 0);

    const qint64 waiting = m_session.files().waitingPublicationCount();
    m_waitingNote->setText(tr("%1 imported from another computer are waiting for their files, out of sight. Each appears when "
                              "a file with the same contents is found in one of these folders.")
                               .arg(waiting == 1 ? tr("1 publication") : tr("%1 publications").arg(QLocale().toString(waiting))));
    m_waitingNote->setVisible(waiting > 0);
    m_forgetWaiting->setVisible(waiting > 0);
    updateButtons();
}

void LibraryFoldersDialog::setImportRunning(bool running)
{
    m_importRunning = running;
    m_note->setVisible(running);
    updateButtons();
}

QList<qint64> LibraryFoldersDialog::selectedRoots() const
{
    QList<qint64> ids;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, 0);
        if (item && item->isSelected())
            ids << item->data(Qt::UserRole).toLongLong();
    }
    return ids;
}

qint64 LibraryFoldersDialog::selectedRoot() const
{
    const QList<qint64> ids = selectedRoots();
    return ids.size() == 1 ? ids.first() : 0;
}

void LibraryFoldersDialog::updateButtons()
{
    const qsizetype chosen = selectedRoots().size();
    m_rescan->setEnabled(chosen > 0);
    m_change->setEnabled(chosen == 1 && !m_importRunning); // One folder moves at a time.
    m_remove->setEnabled(chosen > 0 && !m_importRunning);
    m_remove->setText(chosen > 1 ? tr("Remove %1 folders").arg(chosen) : tr("Remove"));
    m_forget->setEnabled(!m_importRunning);
}

} // namespace papertrail::ui
