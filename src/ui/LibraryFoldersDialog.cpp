#include "ui/LibraryFoldersDialog.h"

#include "ui/LibrarySession.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
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
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &LibraryFoldersDialog::updateButtons);
    layout->addWidget(m_table, 1);

    m_note = new QLabel(tr("An import is running. Stop it to change or remove a folder."), this);
    m_note->hide();
    layout->addWidget(m_note);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add folder..."), this);
    m_rescan = new QPushButton(tr("Scan again"), this);
    m_change = new QPushButton(tr("Change location..."), this);
    m_change->setToolTip(tr("For a folder that has moved. Its files keep their tags and notes."));
    m_remove = new QPushButton(tr("Remove"), this);
    m_remove->setToolTip(tr("Papertrail stops indexing this folder. Nothing is deleted from your disk."));
    buttons->addWidget(add);
    buttons->addWidget(m_rescan);
    buttons->addWidget(m_change);
    buttons->addStretch();
    buttons->addWidget(m_remove);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &LibraryFoldersDialog::addRequested);
    connect(m_rescan, &QPushButton::clicked, this, [this] { emit rescanRequested(selectedRoot()); });
    connect(m_change, &QPushButton::clicked, this, [this] { emit changeLocationRequested(selectedRoot()); });
    connect(m_remove, &QPushButton::clicked, this, [this] { emit removeRequested(selectedRoot()); });

    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);

    reload();
}

void LibraryFoldersDialog::reload()
{
    const qint64 selected = selectedRoot();
    const QList<core::LibraryRoot> roots = m_session.files().roots();
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
        if (root.id == selected)
            m_table->selectRow(row);
    }
    updateButtons();
}

void LibraryFoldersDialog::setImportRunning(bool running)
{
    m_importRunning = running;
    m_note->setVisible(running);
    updateButtons();
}

qint64 LibraryFoldersDialog::selectedRoot() const
{
    const QList<QTableWidgetItem *> items = m_table->selectedItems();
    for (const QTableWidgetItem *item : items) {
        if (item->column() == 0)
            return item->data(Qt::UserRole).toLongLong();
    }
    return 0;
}

void LibraryFoldersDialog::updateButtons()
{
    const bool hasRoot = selectedRoot() != 0;
    m_rescan->setEnabled(hasRoot);
    m_change->setEnabled(hasRoot && !m_importRunning);
    m_remove->setEnabled(hasRoot && !m_importRunning);
}

} // namespace papertrail::ui
