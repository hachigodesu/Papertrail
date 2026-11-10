#include "ui/LibraryCheckDialog.h"

#include "core/db/Database.h"
#include "ui/LibrarySession.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace core;

namespace {

constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kIdsRole = Qt::UserRole + 2;
constexpr int kFileRole = Qt::UserRole + 3;
constexpr int kOtherFilesRole = Qt::UserRole + 4;
constexpr int kSuggestedRole = Qt::UserRole + 5;

QVariantList variants(const QList<qint64> &ids)
{
    QVariantList list;
    for (const qint64 id : ids)
        list << id;
    return list;
}

QString titleOr(const QString &title)
{
    return title.isEmpty() ? QObject::tr("(untitled)") : title;
}

} // namespace

LibraryCheckDialog::LibraryCheckDialog(LibrarySession &session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(tr("Check the library"));
    resize(820, 560);
    auto *column = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    column->addWidget(m_status);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(2);
    m_list->setHeaderLabels({tr("What"), tr("Where")});
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_list->header()->resizeSection(1, 280);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setAccessibleName(tr("What needs attention"));
    column->addWidget(m_list, 1);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &LibraryCheckDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const QList<qint64> ids = idsOf(item);
        if (!ids.isEmpty())
            emit showRequested(ids.first());
    });

    auto *row = new QHBoxLayout;
    const auto add = [&](const QString &name, const QString &text, const std::function<void()> &action) {
        auto *button = new QPushButton(text, this);
        button->setObjectName(name);
        button->setAutoDefault(false);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, action);
        m_buttons << button;
    };
    add(QStringLiteral("findFile"), tr("Find its file..."), [this] {
        if (QTreeWidgetItem *item = current(); item && !idsOf(item).isEmpty()) {
            emit findFileRequested(idsOf(item).first());
            fixed();
        }
    });
    add(QStringLiteral("scan"), tr("Scan folders again"), [this] {
        emit scanRequested();
        fixed();
    });
    add(QStringLiteral("forget"), tr("Forget this location"), [this] {
        QTreeWidgetItem *item = current();
        if (!item || !confirm(tr("Forget this location"),
                              tr("Forget where this file was? Its publication keeps its other files, and the file is found "
                                 "again if it comes back."),
                              tr("Forget it")))
            return;
        try {
            forgetFileLocation(m_session.database(), fileOf(item));
        } catch (const std::exception &) {
        }
        fixed();
    });
    add(QStringLiteral("readAgain"), tr("Read it again"), [this] {
        QTreeWidgetItem *item = current();
        if (!item)
            return;
        try {
            readFileAgain(m_session.database(), fileOf(item));
        } catch (const std::exception &) {
        }
        emit scanRequested();
        fixed();
    });
    add(QStringLiteral("showInFolder"), tr("Show in folder"), [this] {
        if (QTreeWidgetItem *item = current(); item && !idsOf(item).isEmpty())
            emit showInFolderRequested(idsOf(item).first());
    });
    add(QStringLiteral("suggested"), tr("Check suggested files..."), [this] {
        emit suggestedFilesRequested();
        fixed();
    });
    add(QStringLiteral("remove"), tr("Remove..."), [this] {
        QTreeWidgetItem *item = current();
        if (!item || idsOf(item).isEmpty()
            || !confirm(tr("Remove"),
                        tr("Remove \"%1\" from the library? Its file is kept out until you bring it back, and your files "
                           "aren't touched.").arg(item->text(0)),
                        tr("Remove")))
            return;
        emit removeRequested(idsOf(item));
        fixed();
    });
    add(QStringLiteral("deleteDetails"), tr("Delete details..."), [this] {
        QTreeWidgetItem *item = current();
        if (!item || idsOf(item).isEmpty()
            || !confirm(tr("Delete details"),
                        tr("Delete the details of \"%1\", waiting for its file? Its tags, notes and highlights will be gone "
                           "for good. Your files aren't touched.").arg(item->text(0)),
                        tr("Delete details")))
            return;
        emit deleteDetailsRequested(idsOf(item));
        fixed();
    });
    add(QStringLiteral("addToLibrary"), tr("Add to the library"), [this] {
        QTreeWidgetItem *item = current();
        if (!item)
            return;
        try {
            readFileAgain(m_session.database(), fileOf(item));
        } catch (const std::exception &) {
        }
        emit scanRequested();
        fixed();
    });
    add(QStringLiteral("keepOut"), tr("Keep out"), [this] {
        QTreeWidgetItem *item = current();
        if (!item)
            return;
        try {
            keepFileOut(m_session.database(), fileOf(item), QDateTime::currentMSecsSinceEpoch());
        } catch (const std::exception &) {
        }
        fixed();
    });
    add(QStringLiteral("combine"), tr("Combine..."), [this] {
        QTreeWidgetItem *item = current();
        const QList<qint64> ids = idsOf(item);
        if (ids.size() < 2)
            return;
        QStringList others;
        for (int i = 1; i < item->childCount(); ++i)
            others << item->child(i)->text(0);
        if (!confirm(tr("Combine"),
                     tr("Combine these into \"%1\", keeping its details and adding the others' files, tags, notes and "
                        "highlights?\n\n%2").arg(item->childCount() > 0 ? item->child(0)->text(0) : QString(), others.join(u'\n')),
                     tr("Combine")))
            return;
        emit combineRequested(ids);
        fixed();
    });
    add(QStringLiteral("notSame"), tr("Not the same"), [this] {
        QTreeWidgetItem *item = current();
        if (!item)
            return;
        try {
            markDistinct(m_session.database(), idsOf(item));
        } catch (const std::exception &) {
        }
        fixed();
    });
    row->addStretch();
    column->addLayout(row);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *again = buttons->addButton(tr("Check again"), QDialogButtonBox::ActionRole);
    again->setObjectName(QStringLiteral("checkAgain"));
    connect(again, &QPushButton::clicked, this, &LibraryCheckDialog::recheck);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    column->addWidget(buttons);

    updateButtons();
    recheck();
}

LibraryCheckDialog::~LibraryCheckDialog()
{
    if (m_thread) {
        m_thread->disconnect(this);
        m_thread->wait();
        delete m_thread;
    }
}

void LibraryCheckDialog::recheck()
{
    if (m_thread) {
        m_again = true;
        return;
    }
    m_status->setText(tr("Checking the library..."));
    auto found = std::make_shared<LibraryCheckResult>();
    auto failure = std::make_shared<QString>();
    const QString path = m_session.paths().database;
    m_thread = QThread::create([path, found, failure] {
        try {
            Database db(path);
            *found = checkLibrary(db);
        } catch (const std::exception &error) {
            *failure = QString::fromUtf8(error.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, found, failure] {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (m_again) {
            m_again = false;
            recheck();
            return;
        }
        if (!failure->isEmpty())
            m_status->setText(tr("The library couldn't be checked: %1").arg(*failure));
        else
            showResult(*found);
        emit checked();
    });
    m_thread->start();
}

void LibraryCheckDialog::showResult(const LibraryCheckResult &result)
{
    m_result = result;
    m_list->clear();
    const auto section = [&](const QString &heading, Kind kind, qsizetype count) {
        auto *item = new QTreeWidgetItem(m_list, {heading.arg(count)});
        item->setData(0, kKindRole, int(kind));
        item->setFlags(Qt::ItemIsEnabled | (count > 0 ? Qt::ItemIsSelectable : Qt::NoItemFlags));
        QFont bold = item->font(0);
        bold.setBold(true);
        item->setFont(0, bold);
        item->setExpanded(count > 0);
        return item;
    };
    const auto fileRow = [&](QTreeWidgetItem *parent, Kind kind, const CheckedFile &file, const QString &text) {
        auto *item = new QTreeWidgetItem(parent, {text, file.folder + u'/' + file.path});
        item->setData(0, kKindRole, int(kind));
        item->setData(0, kIdsRole, file.publicationId ? variants({file.publicationId}) : QVariantList{});
        item->setData(0, kFileRole, file.fileId);
        item->setData(0, kOtherFilesRole, file.otherFiles);
        item->setToolTip(1, item->text(1));
        return item;
    };

    QTreeWidgetItem *missing = section(tr("Missing files (%1)"), Kind::Missing, result.missing.size());
    for (const CheckedFile &file : result.missing)
        fileRow(missing, Kind::Missing, file, titleOr(file.title));
    QTreeWidgetItem *unreadable = section(tr("Files that can't be read (%1)"), Kind::Unreadable, result.unreadable.size());
    for (const CheckedFile &file : result.unreadable) {
        QTreeWidgetItem *item = fileRow(unreadable, Kind::Unreadable, file, titleOr(file.title));
        if (!file.problem.isEmpty())
            item->setToolTip(0, file.problem);
    }
    QTreeWidgetItem *waiting = section(tr("Waiting for their files (%1)"), Kind::Waiting, result.waiting.size());
    for (const WaitingForFile &entry : result.waiting) {
        auto *item = new QTreeWidgetItem(waiting, {titleOr(entry.title), entry.fileName});
        item->setData(0, kKindRole, int(Kind::Waiting));
        item->setData(0, kIdsRole, variants({entry.publicationId}));
        item->setData(0, kSuggestedRole, entry.suggested);
    }
    QTreeWidgetItem *unattached = section(tr("Files in no publication (%1)"), Kind::Unattached, result.unattached.size());
    for (const CheckedFile &file : result.unattached)
        fileRow(unattached, Kind::Unattached, file, file.path.section(u'/', -1));
    QTreeWidgetItem *shared = section(tr("Identifiers shared by more than one (%1)"), Kind::Shared, result.shared.size());
    for (const SharedIdentifier &entry : result.shared) {
        auto *group = new QTreeWidgetItem(shared, {QStringLiteral("%1 %2").arg(entry.kind, entry.value),
                                                   tr("%n publications", nullptr, int(entry.publicationIds.size()))});
        group->setData(0, kKindRole, int(Kind::Shared));
        group->setData(0, kIdsRole, variants(entry.publicationIds));
        group->setExpanded(true);
        for (qsizetype i = 0; i < entry.publicationIds.size(); ++i) {
            auto *member = new QTreeWidgetItem(group, {titleOr(entry.titles.value(i)), i == 0 ? tr("Kept if combined") : QString()});
            member->setData(0, kKindRole, int(Kind::Shared));
            member->setData(0, kIdsRole, variants({entry.publicationIds.at(i)}));
            member->setFlags(Qt::ItemIsEnabled); // The group is what's chosen.
        }
    }

    if (result.problems() == 0)
        m_status->setText(tr("Nothing needs attention."));
    else
        m_status->setText(tr("%n things need attention. Choose one to see what can be done. Your files are never changed.",
                             nullptr, int(result.problems())));
    if (!result.offlineFolders.isEmpty())
        m_status->setText(m_status->text() + u' '
                          + tr("These folders aren't there right now, so their files aren't checked: %1.")
                                .arg(result.offlineFolders.join(QStringLiteral(", "))));
    updateButtons();
}

QPushButton *LibraryCheckDialog::button(const QString &name) const
{
    for (QPushButton *button : m_buttons) {
        if (button->objectName() == name)
            return button;
    }
    return findChild<QPushButton *>(name);
}

QTreeWidgetItem *LibraryCheckDialog::current() const
{
    QTreeWidgetItem *item = m_list->currentItem();
    // A section heading isn't a problem of its own.
    return item && item->parent() ? item : nullptr;
}

LibraryCheckDialog::Kind LibraryCheckDialog::kindOf(const QTreeWidgetItem *item) const
{
    return Kind(item->data(0, kKindRole).toInt());
}

QList<qint64> LibraryCheckDialog::idsOf(const QTreeWidgetItem *item) const
{
    QList<qint64> ids;
    if (item) {
        for (const QVariant &id : item->data(0, kIdsRole).toList())
            ids << id.toLongLong();
    }
    return ids;
}

qint64 LibraryCheckDialog::fileOf(const QTreeWidgetItem *item) const
{
    return item ? item->data(0, kFileRole).toLongLong() : 0;
}

void LibraryCheckDialog::updateButtons()
{
    QTreeWidgetItem *item = current();
    QStringList shown;
    if (item) {
        switch (kindOf(item)) {
        case Kind::Missing:
            shown << QStringLiteral("findFile") << QStringLiteral("scan");
            if (item->data(0, kOtherFilesRole).toBool())
                shown << QStringLiteral("forget");
            break;
        case Kind::Unreadable:
            shown << QStringLiteral("readAgain") << QStringLiteral("showInFolder") << QStringLiteral("remove");
            break;
        case Kind::Waiting:
            shown << QStringLiteral("findFile");
            if (item->data(0, kSuggestedRole).toBool())
                shown << QStringLiteral("suggested");
            shown << QStringLiteral("deleteDetails");
            break;
        case Kind::Unattached:
            shown << QStringLiteral("addToLibrary") << QStringLiteral("keepOut");
            break;
        case Kind::Shared:
            shown << QStringLiteral("combine") << QStringLiteral("notSame");
            break;
        }
    }
    for (QPushButton *button : std::as_const(m_buttons))
        button->setVisible(shown.contains(button->objectName()));
}

bool LibraryCheckDialog::confirm(const QString &title, const QString &text, const QString &action)
{
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, this);
    box.setTextFormat(Qt::PlainText);
    QPushButton *yes = box.addButton(action, QMessageBox::AcceptRole);
    yes->setObjectName(QStringLiteral("confirm"));
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    return box.clickedButton() == yes;
}

void LibraryCheckDialog::fixed()
{
    emit libraryChanged();
    recheck();
}

} // namespace papertrail::ui
