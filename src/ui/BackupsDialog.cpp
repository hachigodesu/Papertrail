#include "ui/BackupsDialog.h"

#include "core/db/Backups.h"
#include "core/db/Database.h"
#include "ui/LibrarySession.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace papertrail::ui {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;

QString kindText(core::BackupKind kind)
{
    switch (kind) {
    case core::BackupKind::Daily:
        return QDialog::tr("Daily");
    case core::BackupKind::BeforeUpgrade:
        return QDialog::tr("Before an upgrade");
    case core::BackupKind::BeforeImport:
        return QDialog::tr("Before an import replaced the library");
    case core::BackupKind::BeforeRestore:
        return QDialog::tr("Before a backup was restored");
    case core::BackupKind::ByHand:
        return QDialog::tr("Made by hand");
    }
    return QString();
}

} // namespace

DailyBackupThread::DailyBackupThread(const QString &databasePath, QObject *parent)
    : QThread(parent)
    , m_databasePath(databasePath)
{
}

void DailyBackupThread::run()
{
    try {
        if (!core::dailyBackupDue(m_databasePath, QDate::currentDate()))
            return;
        core::Database db(m_databasePath);
        const QString path = core::makeBackup(db, m_databasePath, core::BackupKind::Daily);
        core::pruneBackups(m_databasePath);
        emit made(path);
    } catch (const std::exception &error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

BackupsDialog::BackupsDialog(LibrarySession &session, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
{
    setWindowTitle(tr("Backups"));
    resize(620, 420);
    auto *layout = new QVBoxLayout(this);

    auto *about = new QLabel(tr("Papertrail keeps a copy of your library's details, tags, collections, highlights and notes. "
                                "Your documents aren't copied, and aren't changed by restoring."),
                             this);
    about->setWordWrap(true);
    about->setTextFormat(Qt::PlainText);
    layout->addWidget(about);

    m_daily = new QCheckBox(tr("Back up each day the library is open, keeping the last %1").arg(core::kDailyBackupsKept), this);
    m_daily->setChecked(m_session.settings().value(QStringLiteral("library.daily_backups")).toBool());
    connect(m_daily, &QCheckBox::toggled, this,
            [this](bool on) { m_session.settings().setValue(QStringLiteral("library.daily_backups"), on); });
    layout->addWidget(m_daily);

    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({tr("Made"), tr("Why"), tr("Size")});
    m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &BackupsDialog::updateButtons);
    layout->addWidget(m_list, 1);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setTextFormat(Qt::PlainText);
    layout->addWidget(m_note);

    auto *buttons = new QHBoxLayout;
    m_backUp = new QPushButton(tr("Back up now"), this);
    m_restore = new QPushButton(tr("Restore..."), this);
    m_restore->setToolTip(tr("Replace the library with the chosen backup. The library as it is now is kept first."));
    m_show = new QPushButton(tr("Show folder"), this);
    connect(m_backUp, &QPushButton::clicked, this, &BackupsDialog::backUpNow);
    connect(m_restore, &QPushButton::clicked, this, [this] { restoreChosen(true); });
    connect(m_show, &QPushButton::clicked, this, [this] {
        const QString folder = core::backupFolder(m_session.paths().database);
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    buttons->addWidget(m_backUp);
    buttons->addWidget(m_restore);
    buttons->addWidget(m_show);
    buttons->addStretch(1);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    reload();
}

void BackupsDialog::reload()
{
    m_list->clear();
    const QList<core::BackupInfo> found = core::backups(m_session.paths().database);
    const QLocale locale;
    for (const core::BackupInfo &backup : found) {
        auto *item = new QTreeWidgetItem(m_list, {locale.toString(backup.made, QLocale::ShortFormat), kindText(backup.kind),
                                                  locale.formattedDataSize(backup.size)});
        item->setData(0, kPathRole, backup.path);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    m_note->setText(found.isEmpty() ? tr("There are no backups yet.") : QString());
    m_note->setVisible(found.isEmpty());
    updateButtons();
}

void BackupsDialog::updateButtons()
{
    m_restore->setEnabled(m_list->currentItem() != nullptr);
}

void BackupsDialog::backUpNow()
{
    try {
        core::makeBackup(m_session.database(), m_session.paths().database, core::BackupKind::ByHand);
    } catch (const std::exception &error) {
        QMessageBox box(QMessageBox::Warning, tr("Back up now"),
                        tr("The library couldn't be backed up: %1").arg(QString::fromUtf8(error.what())), QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    reload();
}

void BackupsDialog::restoreChosen(bool ask)
{
    QTreeWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString path = item->data(0, kPathRole).toString();
    const core::BackupCheck check = core::checkBackup(path);
    if (!check.usable) {
        QMessageBox box(QMessageBox::Warning, tr("Restore a backup"), tr("This backup can't be used. %1").arg(check.problem),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        if (ask)
            box.exec();
        return;
    }
    if (ask) {
        QMessageBox box(QMessageBox::Question, tr("Restore a backup"),
                        tr("Replace your library with the backup made %1?\n\nEverything changed since then is replaced: details, "
                           "tags, collections, highlights and notes. Your library as it is now is backed up first, so this "
                           "can be undone. Open documents are closed. Your files aren't touched.")
                            .arg(item->text(0)),
                        QMessageBox::NoButton, this);
        box.setTextFormat(Qt::PlainText);
        QPushButton *restore = box.addButton(tr("Restore"), QMessageBox::DestructiveRole);
        restore->setObjectName(QStringLiteral("restoreBackup"));
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != restore)
            return;
    }
    emit restoreRequested(path);
    reload();
}

} // namespace papertrail::ui
