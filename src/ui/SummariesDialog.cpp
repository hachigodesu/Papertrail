#include "ui/SummariesDialog.h"

#include "core/repo/SummaryRepository.h"
#include "summary/SummarySpending.h"
#include "ui/LibrarySession.h"
#include "ui/SummaryDialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace papertrail::ui {

using namespace summary;

namespace {

QLabel *wrapped(QWidget *parent, const QString &text = QString())
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText); // Titles come from files, and replies from Claude.
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString when(qint64 msecs)
{
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(msecs), QLocale::ShortFormat);
}

QString modelName(const QString &id)
{
    const std::optional<ModelInfo> info = modelInfo(id);
    return info ? info->label : id;
}

QString titleOf(const QString &title)
{
    return title.trimmed().isEmpty() ? QObject::tr("(untitled)") : title;
}

} // namespace

SummariesDialog::SummariesDialog(LibrarySession &session, SummaryBatches &batches, QWidget *parent)
    : QDialog(parent), m_session(session), m_batches(batches)
{
    setWindowTitle(tr("Summaries from Claude"));
    resize(820, 600);

    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildWaitingPage(), QString());
    m_tabs->addTab(buildReviewPage(), QString());
    m_tabs->addTab(buildSpendingPage(), tr("Spending"));
    layout->addWidget(m_tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Whatever arrives or changes while this is open shows straight away.
    connect(&m_batches, &SummaryBatches::changed, this, &SummariesDialog::reload);
    connect(&m_batches, &SummaryBatches::checked, this, [this] { m_check->setEnabled(true); });
    reload();
    showPage(m_entries.isEmpty() ? Page::Spending : Page::OnTheirWay);
    for (const BatchEntry &entry : std::as_const(m_entries)) {
        if (entry.state == BatchEntry::State::Arrived) {
            showPage(Page::ToReview);
            break;
        }
    }
}

QWidget *SummariesDialog::buildWaitingPage()
{
    auto *page = new QWidget;
    auto *column = new QVBoxLayout(page);
    m_waiting = new QTreeWidget(page);
    m_waiting->setHeaderLabels({tr("Publication"), tr("Sent"), tr("Model"), tr("Status")});
    m_waiting->setRootIsDecorated(false);
    m_waiting->setAlternatingRowColors(true);
    m_waiting->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_waiting->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_waiting->header()->setStretchLastSection(false);
    m_waiting->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_waiting->setColumnWidth(3, 220);
    column->addWidget(m_waiting);
    m_waitingNote = wrapped(page, tr("Papertrail asks Anthropic about these every few minutes while it's open, and when it opens, so "
                                     "closing it loses nothing. Most arrive within the hour, and all within a day. Anthropic keeps "
                                     "the results for 29 days, so open Papertrail before then to collect them."));
    column->addWidget(m_waitingNote);
    auto *row = new QHBoxLayout;
    m_check = new QPushButton(tr("Check now"), page);
    connect(m_check, &QPushButton::clicked, this, [this] {
        m_check->setEnabled(false);
        emit checkRequested();
        if (!m_batches.isBusy())
            m_check->setEnabled(true); // Nothing was asked, such as when there's no key.
    });
    m_dismiss = new QPushButton(tr("Remove from list"), page);
    m_dismiss->setToolTip(tr("Takes summaries that failed off this list."));
    connect(m_dismiss, &QPushButton::clicked, this, &SummariesDialog::dismissChosen);
    connect(m_waiting, &QTreeWidget::itemSelectionChanged, this, [this] {
        bool anyFailed = false;
        for (const QTreeWidgetItem *item : m_waiting->selectedItems())
            anyFailed = anyFailed || item->data(0, Qt::UserRole + 2).toBool();
        m_dismiss->setEnabled(anyFailed);
    });
    m_dismiss->setEnabled(false);
    row->addWidget(m_check);
    row->addWidget(m_dismiss);
    row->addStretch();
    column->addLayout(row);
    return page;
}

QWidget *SummariesDialog::buildReviewPage()
{
    auto *page = new QWidget;
    auto *column = new QVBoxLayout(page);
    column->addWidget(wrapped(page, tr("These have come back from Claude. Read each, then keep it as the publication's summary or "
                                       "discard it. They wait here until you do, however often Papertrail is closed.")));
    auto *split = new QSplitter(Qt::Horizontal, page);
    m_review = new QListWidget(split);
    m_review->setAccessibleName(tr("Summaries to review"));
    auto *right = new QWidget(split);
    auto *rightColumn = new QVBoxLayout(right);
    rightColumn->setContentsMargins(0, 0, 0, 0);
    m_reviewTitle = wrapped(right);
    QFont heading = m_reviewTitle->font();
    heading.setBold(true);
    m_reviewTitle->setFont(heading);
    rightColumn->addWidget(m_reviewTitle);
    m_reviewInfo = wrapped(right);
    rightColumn->addWidget(m_reviewInfo);
    m_reviewText = new QPlainTextEdit(right);
    m_reviewText->setReadOnly(true);
    m_reviewText->setAccessibleName(tr("Summary to review"));
    rightColumn->addWidget(m_reviewText, 1);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setSizes({260, 520});
    column->addWidget(split, 1);

    auto *row = new QHBoxLayout;
    m_keep = new QPushButton(tr("Keep"), page);
    m_keep->setToolTip(tr("Makes this the publication's summary, replacing any it has."));
    m_discard = new QPushButton(tr("Discard"), page);
    m_keepAll = new QPushButton(tr("Keep all"), page);
    connect(m_keep, &QPushButton::clicked, this, &SummariesDialog::keepChosen);
    connect(m_discard, &QPushButton::clicked, this, &SummariesDialog::discardChosen);
    connect(m_keepAll, &QPushButton::clicked, this, &SummariesDialog::keepAll);
    connect(m_review, &QListWidget::currentRowChanged, this, &SummariesDialog::showChosenReview);
    row->addWidget(m_keep);
    row->addWidget(m_discard);
    row->addStretch();
    row->addWidget(m_keepAll);
    column->addLayout(row);
    return page;
}

QWidget *SummariesDialog::buildSpendingPage()
{
    auto *page = new QWidget;
    auto *column = new QVBoxLayout(page);
    m_totals = wrapped(page);
    column->addWidget(m_totals);
    m_spending = new QTreeWidget(page);
    m_spending->setHeaderLabels({tr("When"), tr("For"), tr("Model"), tr("How"), tr("Cost")});
    m_spending->setRootIsDecorated(false);
    m_spending->setAlternatingRowColors(true);
    m_spending->header()->setStretchLastSection(false);
    m_spending->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    column->addWidget(m_spending, 1);
    m_spendingNote = consoleLabel(page, QStringLiteral("%1"), kConsoleBillingUrl);
    column->addWidget(m_spendingNote);
    return page;
}

void SummariesDialog::showPage(Page page)
{
    m_tabs->setCurrentIndex(int(page));
}

void SummariesDialog::showPublication(qint64 publicationId)
{
    for (int row = 0; row < m_review->count(); ++row) {
        if (m_review->item(row)->data(Qt::UserRole + 2).toLongLong() == publicationId) {
            showPage(Page::ToReview);
            m_review->setCurrentRow(row);
            return;
        }
    }
    for (int row = 0; row < m_waiting->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_waiting->topLevelItem(row);
        if (item->data(0, Qt::UserRole + 3).toLongLong() == publicationId) {
            showPage(Page::OnTheirWay);
            m_waiting->setCurrentItem(item);
            return;
        }
    }
}

void SummariesDialog::problem(const QString &doing, const std::exception &error)
{
    QMessageBox box(QMessageBox::Warning, windowTitle(), tr("%1 failed: %2").arg(doing, QString::fromUtf8(error.what())), QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

void SummariesDialog::reload()
{
    const QString chosenReview = m_review->currentItem() ? m_review->currentItem()->data(Qt::UserRole).toString() : QString();
    try {
        m_entries = SummaryBatches::entries(m_session.database());
    } catch (const std::exception &error) {
        m_entries.clear();
        m_waitingNote->setText(tr("The list couldn't be read: %1").arg(QString::fromUtf8(error.what())));
    }

    m_waiting->clear();
    {
        const QSignalBlocker blocker(m_review);
        m_review->clear();
    }
    int waiting = 0;
    int toReview = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        const BatchEntry &entry = m_entries.at(i);
        const QString key = entry.batchId + u'/' + entry.customId;
        if (entry.state == BatchEntry::State::Arrived) {
            auto *item = new QListWidgetItem(titleOf(entry.title), m_review);
            item->setToolTip(tr("Arrived %1").arg(when(entry.arrivedAt)));
            item->setData(Qt::UserRole, key);
            item->setData(Qt::UserRole + 1, i);
            item->setData(Qt::UserRole + 2, entry.publicationId);
            ++toReview;
            continue;
        }
        QString status;
        switch (entry.state) {
        case BatchEntry::State::Waiting:
            status = entry.continuations == 0 ? tr("On its way") : tr("On its way, carried on after pausing");
            break;
        case BatchEntry::State::Paused: status = tr("Paused partway, and sent on at the next check"); break;
        default: status = tr("Failed: %1").arg(entry.problem); break;
        }
        auto *item = new QTreeWidgetItem(m_waiting, {titleOf(entry.title), when(entry.sentAt), modelName(entry.model), status});
        item->setToolTip(3, status);
        item->setData(0, Qt::UserRole, key);
        item->setData(0, Qt::UserRole + 1, i);
        item->setData(0, Qt::UserRole + 2, entry.state == BatchEntry::State::Failed);
        item->setData(0, Qt::UserRole + 3, entry.publicationId);
        if (entry.state != BatchEntry::State::Failed)
            ++waiting;
    }
    m_tabs->setTabText(int(Page::OnTheirWay), waiting > 0 ? tr("On their way (%1)").arg(waiting) : tr("On their way"));
    m_tabs->setTabText(int(Page::ToReview), toReview > 0 ? tr("To review (%1)").arg(toReview) : tr("To review"));

    int row = 0;
    for (int i = 0; i < m_review->count(); ++i) {
        if (m_review->item(i)->data(Qt::UserRole).toString() == chosenReview)
            row = i;
    }
    if (m_review->count() > 0)
        m_review->setCurrentRow(row);
    showChosenReview();
    m_keepAll->setEnabled(m_review->count() > 1);
    reloadSpending();
}

std::optional<BatchEntry> SummariesDialog::chosenReview() const
{
    const QListWidgetItem *item = m_review->currentItem();
    if (!item)
        return std::nullopt;
    const int index = item->data(Qt::UserRole + 1).toInt();
    if (index < 0 || index >= m_entries.size())
        return std::nullopt;
    return m_entries.at(index);
}

void SummariesDialog::showChosenReview()
{
    const std::optional<BatchEntry> entry = chosenReview();
    m_keep->setEnabled(entry.has_value());
    m_discard->setEnabled(entry.has_value());
    if (!entry) {
        m_reviewTitle->clear();
        m_reviewInfo->setText(m_review->count() == 0 ? tr("Nothing is waiting to be reviewed.") : QString());
        m_reviewText->clear();
        return;
    }
    const SummaryText text = parseSummaryText(entry->text);
    m_reviewTitle->setText(titleOf(entry->title));
    QStringList info;
    info << tr("Generated by %1, asked for on %2 and arrived on %3. It cost %4, at half price.")
                .arg(modelName(entry->model), when(entry->sentAt), when(entry->arrivedAt),
                     entry->cost ? formatDollars(*entry->cost) : tr("an unknown amount"));
    info << basisDescription(text.basis, text.identificationUnsure, false, text.isCase);
    bool hasOne = false;
    try {
        hasOne = core::SummaryRepository(m_session.database()).forPublication(entry->publicationId).has_value();
    } catch (const std::exception &) {
    }
    if (hasOne)
        info << tr("Keeping it replaces the summary this publication has now.");
    m_reviewInfo->setText(info.join(u'\n'));

    QString shown = summaryAsText(text);
    if (!entry->sources.isEmpty()) {
        shown += QStringLiteral("\n\n") + tr("Sources") + u'\n';
        for (const Source &source : std::as_const(entry->sources))
            shown += (source.title.isEmpty() ? source.url : source.title + QStringLiteral(" (") + source.url + u')') + u'\n';
    }
    m_reviewText->setPlainText(shown.trimmed());
}

void SummariesDialog::keepChosen()
{
    const std::optional<BatchEntry> entry = chosenReview();
    if (!entry)
        return;
    try {
        m_batches.keep(entry->batchId, entry->customId);
    } catch (const std::exception &error) {
        problem(tr("Keeping the summary"), error);
    }
    reload();
}

void SummariesDialog::keepAll()
{
    // Chosen first, since each one kept reloads the list.
    QList<QPair<QString, QString>> chosen;
    for (const BatchEntry &entry : std::as_const(m_entries)) {
        if (entry.state == BatchEntry::State::Arrived)
            chosen.append({entry.batchId, entry.customId});
    }
    try {
        for (const auto &[batchId, customId] : std::as_const(chosen))
            m_batches.keep(batchId, customId);
    } catch (const std::exception &error) {
        problem(tr("Keeping the summaries"), error);
    }
    reload();
}

void SummariesDialog::discardChosen()
{
    const std::optional<BatchEntry> entry = chosenReview();
    if (!entry)
        return;
    QMessageBox box(QMessageBox::Question, tr("Discard summary"),
                    tr("Discard this summary? It has been paid for, and asking again would cost money again."),
                    QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Discard)
        return;
    try {
        m_batches.discard(entry->batchId, entry->customId);
    } catch (const std::exception &error) {
        problem(tr("Discarding the summary"), error);
    }
    reload();
}

void SummariesDialog::dismissChosen()
{
    // Chosen first, since each one taken off reloads the list.
    QList<QPair<QString, QString>> chosen;
    for (const QTreeWidgetItem *item : m_waiting->selectedItems()) {
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (item->data(0, Qt::UserRole + 2).toBool() && index >= 0 && index < m_entries.size())
            chosen.append({m_entries.at(index).batchId, m_entries.at(index).customId});
    }
    try {
        for (const auto &[batchId, customId] : std::as_const(chosen))
            m_batches.dismiss(batchId, customId);
    } catch (const std::exception &error) {
        problem(tr("Removing them from the list"), error);
    }
    reload();
}

void SummariesDialog::reloadSpending()
{
    m_spending->clear();
    SpendTotals totals;
    QList<SpendEntry> entries;
    try {
        totals = spendTotals(m_session.database(), QDateTime::currentMSecsSinceEpoch());
        entries = spendEntries(m_session.database());
    } catch (const std::exception &error) {
        m_totals->setText(tr("What's been spent couldn't be read: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    QStringList lines;
    lines << tr("This month: %1").arg(formatDollars(totals.thisMonth));
    lines << tr("The last 30 days: %1").arg(formatDollars(totals.last30Days));
    lines << tr("Altogether: %1, of which %2 straight away and %3 at half price.")
                 .arg(formatDollars(totals.allTime), formatDollars(totals.instant), formatDollars(totals.batch));
    if (totals.unpriced > 0)
        lines << (totals.unpriced == 1 ? tr("1 reply came from a model whose prices aren't known, so it isn't counted.")
                                       : tr("%1 replies came from models whose prices aren't known, so they aren't counted.").arg(totals.unpriced));
    m_totals->setText(lines.join(u'\n'));

    bool earlier = false;
    for (const SpendEntry &entry : std::as_const(entries)) {
        earlier = earlier || entry.key.startsWith(QLatin1StringView("summary:"));
        auto *item = new QTreeWidgetItem(m_spending, {when(entry.spentAt),
                                                      entry.title.trimmed().isEmpty() ? tr("(untitled)") : entry.title,
                                                      modelName(entry.model),
                                                      entry.kind == SpendKind::Batch ? tr("At half price") : tr("Straight away"),
                                                      entry.cost ? formatDollars(*entry.cost) : tr("Unknown")});
        item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(4, tr("%1 tokens in, %2 out, %3 web searches").arg(entry.inputTokens).arg(entry.outputTokens).arg(entry.webSearches));
    }
    for (const int column : {0, 2, 3, 4})
        m_spending->resizeColumnToContents(column);

    QString note = tr("Worked out on this computer from the tokens Anthropic counted and its prices as checked on %1, so it's close "
                      "but not your bill, which is in the %2. Each reply is counted, whether or not its summary was kept.");
    if (earlier)
        note += tr(" Before this list was kept, only the summaries that were kept were noted.");
    // Only the Console's name becomes a link. Everything else is shown as it is.
    m_spendingNote->setText(note.toHtmlEscaped().arg(pricesCheckedOn().toHtmlEscaped(),
                                                     QStringLiteral("<a href=\"%1\">%2</a>")
                                                         .arg(QString::fromLatin1(kConsoleBillingUrl), tr("Claude Console").toHtmlEscaped())));
}

} // namespace papertrail::ui
