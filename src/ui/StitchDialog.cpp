#include "ui/StitchDialog.h"

#include "core/repo/StitchRepository.h"
#include "import/ImportService.h"
#include "import/Restitch.h"
#include "import/WorkerClient.h"
#include "ui/LibrarySession.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace papertrail::ui {

namespace {

constexpr int kReadTimeoutMs = 10 * 60 * 1000;
const int kNameRole = Qt::UserRole;
const int kIndexRole = Qt::UserRole + 1;

} // namespace

StitchDialog::StitchDialog(LibrarySession &session, const QString &zipSha256, const QString &zipPath, const QString &title,
                           QWidget *parent)
    : QDialog(parent), m_session(session), m_sha256(zipSha256), m_zipPath(zipPath)
{
    setWindowTitle(tr("Order of its parts"));
    resize(640, 560);
    auto *layout = new QVBoxLayout(this);

    auto *heading = new QLabel(title.isEmpty() ? tr("This book") : title, this);
    heading->setTextFormat(Qt::PlainText); // A title can come from a document, which can't be trusted.
    heading->setWordWrap(true);
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    layout->addWidget(heading);

    m_summary = new QLabel(tr("Reading the parts in the zip..."), this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("parts"));
    m_list->setIconSize(QSize(48, 64));
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setWordWrap(true);
    m_list->setEnabled(false);
    layout->addWidget(m_list, 1);
    connect(m_list, &QListWidget::currentRowChanged, this, &StitchDialog::updateButtons);

    auto *moves = new QHBoxLayout;
    m_up = new QPushButton(tr("Move up"), this);
    m_up->setObjectName(QStringLiteral("moveUp"));
    m_down = new QPushButton(tr("Move down"), this);
    m_down->setObjectName(QStringLiteral("moveDown"));
    m_again = new QPushButton(tr("Work it out again"), this);
    m_again->setObjectName(QStringLiteral("workOutAgain"));
    m_again->setToolTip(tr("Puts the parts back in the order Papertrail worked out, all of them included."));
    moves->addWidget(m_up);
    moves->addWidget(m_down);
    moves->addStretch();
    moves->addWidget(m_again);
    layout->addLayout(moves);
    connect(m_up, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    connect(m_again, &QPushButton::clicked, this, &StitchDialog::workOutAgain);

    auto *note = new QLabel(tr("The zip itself is never changed. The stitched PDF is kept in Papertrail's own folder, and "
                               "this order can be changed again whenever you like: highlights, notes and bookmarks move "
                               "with their parts."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(this);
    m_stitch = buttons->addButton(tr("Stitch"), QDialogButtonBox::AcceptRole);
    m_stitch->setObjectName(QStringLiteral("stitch"));
    m_stitch->setDefault(true);
    QPushButton *later = buttons->addButton(tr("Later"), QDialogButtonBox::RejectRole);
    later->setObjectName(QStringLiteral("later"));
    layout->addWidget(buttons);
    connect(m_stitch, &QPushButton::clicked, this, &StitchDialog::stitch);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateButtons();

    try {
        if (const std::optional<core::StitchOrder> kept = core::StitchRepository(m_session.database()).get(m_sha256))
            m_kept = import::orderFromJson(kept->partsJson);
    } catch (const std::exception &) {
        // Shown in the order worked out instead.
    }

    const QString converted = import::convertedPathFor(m_session.paths().cacheDirectory, m_sha256, m_zipPath);
    m_coverFolder = QFileInfo(converted).dir().filePath(m_sha256 + QStringLiteral("-parts"));
    const QString worker = m_session.paths().workerPath;
    const QString path = m_zipPath;
    const QString covers = m_coverFolder;
    auto result = std::make_shared<import::WorkerClient::ZipParts>();
    const std::shared_ptr<std::atomic<bool>> cancelled = m_cancelled;
    m_thread = QThread::create([worker, path, covers, result, cancelled] {
        // Closing the dialog stops the worker, so it never waits for a large zip.
        import::WorkerClient client(worker, [cancelled] { return cancelled->load(); });
        *result = client.zipParts(path, covers, kReadTimeoutMs);
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        m_thread->deleteLater();
        m_thread = nullptr;
        onRead(result->plan, result->failure == import::WorkerFailure::None ? QString() : result->error);
    });
    m_thread->start();
}

StitchDialog::~StitchDialog()
{
    if (m_thread) {
        *m_cancelled = true;
        m_thread->wait();
        delete m_thread;
    }
}

QPushButton *StitchDialog::button(const QString &name) const
{
    return findChild<QPushButton *>(name);
}

void StitchDialog::onRead(const import::StitchPlan &plan, const QString &error)
{
    if (!error.isEmpty() || plan.parts.isEmpty()) {
        m_summary->setText(tr("The parts in the zip couldn't be read: %1").arg(error.isEmpty() ? tr("it holds none") : error));
        updateButtons();
        emit partsRead();
        return;
    }
    m_workedOut = plan;

    // In the order kept, if there is one, else as worked out.
    QList<import::PlacedPart> parts;
    QList<import::PlacedPart> rest = plan.parts;
    for (const import::OrderedPart &kept : std::as_const(m_kept)) {
        for (qsizetype i = 0; i < rest.size(); ++i) {
            if (rest.at(i).part.name == kept.name) {
                import::PlacedPart part = rest.takeAt(i);
                part.included = kept.included && part.part.problem.isEmpty();
                parts << part;
                break;
            }
        }
    }
    parts << rest;
    fill(parts);

    QString summary = tr("Papertrail worked out this order from the page numbers printed on the parts, and from their "
                         "names. Check it, drag any part that's out of place, and untick any you'd rather leave out.");
    if (plan.unplaced > 0)
        summary += u' ' + tr("%n part(s) couldn't be placed, and are at the end.", "", plan.unplaced);
    else if (!plan.certain)
        summary += u' ' + tr("Some parts were placed by their names alone, so check those especially.");
    m_summary->setText(summary);
    m_list->setEnabled(true);
    updateButtons();
    emit partsRead();
}

void StitchDialog::fill(const QList<import::PlacedPart> &parts)
{
    m_list->clear();
    for (const import::PlacedPart &placed : parts) {
        const QString pages = placed.part.pageCount > 0 ? tr("%n page(s)", "", placed.part.pageCount) : QString();
        const QString title = import::partTitle(placed.part);
        auto *item = new QListWidgetItem(pages.isEmpty() ? QStringLiteral("%1\n%2").arg(title, placed.reason)
                                                         : QStringLiteral("%1 (%2)\n%3").arg(title, pages, placed.reason));
        item->setData(kNameRole, placed.part.name);
        item->setToolTip(placed.part.name);
        const QString cover = QDir(m_coverFolder).filePath(placed.part.sha256 + QStringLiteral(".jpg"));
        if (!placed.part.sha256.isEmpty() && QFileInfo::exists(cover))
            item->setIcon(QIcon(cover));
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        if (placed.part.problem.isEmpty())
            flags |= Qt::ItemIsUserCheckable;
        item->setFlags(flags);
        item->setCheckState(placed.included && placed.part.problem.isEmpty() ? Qt::Checked : Qt::Unchecked);
        // Kept with the item, so it goes wherever the item's dragged.
        const qsizetype index = std::find_if(m_workedOut.parts.cbegin(), m_workedOut.parts.cend(),
                                             [&](const import::PlacedPart &other) { return other.part.name == placed.part.name; })
            - m_workedOut.parts.cbegin();
        item->setData(kIndexRole, int(index));
        m_list->addItem(item);
    }
}

void StitchDialog::moveCurrent(int by)
{
    const int row = m_list->currentRow();
    const int to = row + by;
    if (row < 0 || to < 0 || to >= m_list->count())
        return;
    QListWidgetItem *item = m_list->takeItem(row);
    m_list->insertItem(to, item);
    m_list->setCurrentRow(to);
}

void StitchDialog::workOutAgain()
{
    QList<import::PlacedPart> parts = m_workedOut.parts;
    for (import::PlacedPart &part : parts)
        part.included = part.part.problem.isEmpty();
    fill(parts);
    updateButtons();
}

void StitchDialog::updateButtons()
{
    const int row = m_list->currentRow();
    const bool ready = m_list->isEnabled() && m_list->count() > 0;
    m_up->setEnabled(ready && row > 0);
    m_down->setEnabled(ready && row >= 0 && row + 1 < m_list->count());
    m_again->setEnabled(ready);
    m_stitch->setEnabled(ready);
}

QList<import::OrderedPart> StitchDialog::chosenOrder() const
{
    QList<import::OrderedPart> order;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        const int index = item->data(kIndexRole).toInt();
        if (index < 0 || index >= m_workedOut.parts.size())
            continue;
        const import::ZipPart &part = m_workedOut.parts.at(index).part;
        import::OrderedPart ordered;
        ordered.name = part.name;
        ordered.sha256 = part.sha256;
        ordered.pageCount = part.pageCount;
        ordered.included = item->checkState() == Qt::Checked && part.problem.isEmpty();
        ordered.title = import::partTitle(part);
        order << ordered;
    }
    return order;
}

bool StitchDialog::changed() const
{
    const QList<import::OrderedPart> chosen = chosenOrder();
    if (chosen.size() != m_kept.size())
        return true;
    for (qsizetype i = 0; i < chosen.size(); ++i) {
        if (chosen.at(i).name != m_kept.at(i).name || chosen.at(i).included != m_kept.at(i).included
            || chosen.at(i).title != m_kept.at(i).title) {
            return true;
        }
    }
    return false;
}

void StitchDialog::stitch()
{
    const QList<import::OrderedPart> chosen = chosenOrder();
    if (std::none_of(chosen.cbegin(), chosen.cend(), [](const import::OrderedPart &part) { return part.included; })) {
        QMessageBox::information(this, tr("Order of its parts"), tr("At least one part has to be included."));
        return;
    }
    int leftOut = 0;
    try {
        leftOut = import::marksLeftOut(m_session.database(), m_sha256, chosen);
    } catch (const std::exception &) {
        leftOut = 0;
    }
    if (leftOut > 0) {
        const auto answer = QMessageBox::question(
            this, tr("Order of its parts"),
            tr("%n highlight(s), note(s) or bookmark(s) are on parts you're leaving out. They're kept, and come back "
               "if you include those parts again.",
               "", leftOut),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
        if (answer != QMessageBox::Ok)
            return;
    }
    accept();
}

} // namespace papertrail::ui
