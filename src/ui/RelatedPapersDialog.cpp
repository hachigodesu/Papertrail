#include "ui/RelatedPapersDialog.h"

#include "core/repo/PublicationRepository.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"
#include "ui/LookupThread.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace core;
using namespace metadata;

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;
constexpr int kRelationRole = Qt::UserRole + 2; // 0 for references, 1 for citing papers.

enum Column { TitleColumn, AuthorsColumn, YearColumn, CitedByColumn, InLibraryColumn };

} // namespace

RelatedPapersDialog::RelatedPapersDialog(LibrarySession &session, LookupThread *lookups, qint64 publicationId, QWidget *parent)
    : QDialog(parent), m_session(session), m_lookups(lookups), m_publicationId(publicationId)
{
    setWindowTitle(tr("Related papers"));
    resize(900, 560);
    auto *column = new QVBoxLayout(this);

    m_heading = new QLabel(this);
    m_heading->setTextFormat(Qt::PlainText); // A title can come from a document.
    m_heading->setWordWrap(true);
    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    column->addWidget(m_heading);
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    column->addWidget(m_status);

    m_tabs = new QTabWidget(this);
    const auto makeTree = [this](const QString &name) {
        auto *tree = new QTreeWidget(m_tabs);
        tree->setAccessibleName(name);
        tree->setRootIsDecorated(false);
        tree->setUniformRowHeights(true);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree->setHeaderLabels({tr("Title"), tr("Authors"), tr("Year"), tr("Cited by"), tr("In your library")});
        tree->header()->setStretchLastSection(false);
        tree->header()->setSectionResizeMode(TitleColumn, QHeaderView::Stretch);
        tree->header()->resizeSection(AuthorsColumn, 180);
        tree->header()->resizeSection(YearColumn, 50);
        tree->header()->resizeSection(CitedByColumn, 70);
        tree->header()->resizeSection(InLibraryColumn, 100);
        connect(tree, &QTreeWidget::itemSelectionChanged, this, &RelatedPapersDialog::updateButtons);
        // Double-clicking shows one that's here, and otherwise opens its page.
        connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
            const RelatedWork *work = workFor(item);
            if (work && work->inLibrary)
                emit showRequested(*work->inLibrary);
            else
                openChosen();
        });
        return tree;
    };
    m_references = makeTree(tr("Papers it cites"));
    m_citedBy = makeTree(tr("Papers that cite it"));
    m_tabs->addTab(m_references, tr("References"));
    m_tabs->addTab(m_citedBy, tr("Cited by"));
    connect(m_tabs, &QTabWidget::currentChanged, this, &RelatedPapersDialog::updateButtons);
    column->addWidget(m_tabs, 1);

    auto *buttons = new QHBoxLayout;
    m_add = new QPushButton(tr("Add to library"), this);
    m_add->setToolTip(tr("Adds the chosen papers with their details from OpenAlex and no file. A file that turns up "
                         "later in your folders joins them."));
    m_copy = new QPushButton(tr("Copy DOI"), this);
    m_open = new QPushButton(tr("Open in browser"), this);
    m_show = new QPushButton(tr("Show in library"), this);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setToolTip(tr("Asks OpenAlex again, sending only this publication's DOI."));
    connect(m_add, &QPushButton::clicked, this, &RelatedPapersDialog::addChosen);
    connect(m_copy, &QPushButton::clicked, this, &RelatedPapersDialog::copyDois);
    connect(m_open, &QPushButton::clicked, this, &RelatedPapersDialog::openChosen);
    connect(m_show, &QPushButton::clicked, this, &RelatedPapersDialog::showChosen);
    connect(m_refresh, &QPushButton::clicked, this, [this] { load(true); });
    for (QPushButton *button : {m_add, m_copy, m_open, m_show})
        buttons->addWidget(button);
    buttons->addStretch();
    buttons->addWidget(m_refresh);
    auto *close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(close);
    column->addLayout(buttons);

    if (m_lookups) {
        connect(m_lookups, &LookupThread::relatedFound, this, &RelatedPapersDialog::onFound);
        connect(m_lookups, &LookupThread::relatedFailed, this, &RelatedPapersDialog::onFailed);
    }
    try {
        const std::optional<Publication> publication = m_session.publications().get(publicationId);
        m_heading->setText(publication ? publication->title : QString());
    } catch (const std::exception &) {
        // The heading's only a reminder.
    }
    updateButtons();
}

void RelatedPapersDialog::load(bool refresh)
{
    fill();
    QString doi;
    try {
        if (const std::optional<Publication> publication = m_session.publications().get(m_publicationId))
            doi = publication->doi;
    } catch (const std::exception &) {
    }
    const bool recent = m_papers.fetched
        && QDateTime::currentMSecsSinceEpoch() - m_papers.fetchedAt < qint64(RelatedFinder::kKeepDays) * 24 * 60 * 60 * 1000;
    if (!refresh && recent && !doi.isEmpty())
        return;
    if (!m_lookups)
        return;
    m_asking = true;
    m_status->setText(tr("Asking OpenAlex, with only this publication's DOI..."));
    m_refresh->setEnabled(false);
    m_lookups->findRelated(m_publicationId, refresh);
}

void RelatedPapersDialog::fill()
{
    try {
        m_papers = relatedPapersFor(m_session.database(), m_publicationId);
    } catch (const std::exception &error) {
        m_status->setText(tr("What was found before couldn't be read: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    fillTree(m_references, m_papers.references);
    fillTree(m_citedBy, m_papers.citedBy);
    m_tabs->setTabText(0, tr("References (%1)").arg(m_papers.references.size()));
    m_tabs->setTabText(1, m_papers.citedByTotal > m_papers.citedBy.size()
                              ? tr("Cited by (%1 most cited of %2)")
                                    .arg(m_papers.citedBy.size())
                                    .arg(QLocale().toString(m_papers.citedByTotal))
                              : tr("Cited by (%1)").arg(m_papers.citedBy.size()));
    if (m_papers.fetched && !m_asking) {
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(m_papers.fetchedAt);
        m_status->setText(tr("From OpenAlex, %1. Papers OpenAlex doesn't know of aren't listed.")
                              .arg(QLocale().toString(when.date(), QLocale::ShortFormat)));
    }
    updateButtons();
}

void RelatedPapersDialog::fillTree(QTreeWidget *tree, const QList<RelatedWork> &works)
{
    tree->clear();
    const int relation = tree == m_references ? 0 : 1;
    for (qsizetype i = 0; i < works.size(); ++i) {
        const RelatedWork &work = works.at(i);
        const Publication &p = work.record.publication;
        QStringList names;
        for (const PersonName &author : p.authors.mid(0, 3))
            names << author.family;
        QString authors = names.join(QStringLiteral(", "));
        if (p.authors.size() > 3)
            authors += tr(" et al.");
        // Items show text as it is, so nothing from OpenAlex becomes formatting.
        auto *item = new QTreeWidgetItem({p.title, authors, p.year ? QString::number(*p.year) : QString(),
                                          QLocale().toString(work.citedByCount), work.inLibrary ? tr("Yes") : QString()});
        item->setData(0, kIndexRole, int(i));
        item->setData(0, kRelationRole, relation);
        item->setToolTip(TitleColumn, p.containerTitle);
        item->setTextAlignment(CitedByColumn, Qt::AlignRight | Qt::AlignVCenter);
        if (work.inLibrary) {
            QFont font = item->font(TitleColumn);
            font.setBold(true);
            item->setFont(TitleColumn, font);
        }
        tree->addTopLevelItem(item);
    }
}

void RelatedPapersDialog::onFound(qint64 publicationId)
{
    if (publicationId != m_publicationId)
        return;
    m_asking = false;
    m_refresh->setEnabled(true);
    fill();
    if (m_papers.references.isEmpty() && m_papers.citedBy.isEmpty())
        m_status->setText(tr("OpenAlex knows this publication, but not what it cites or what cites it."));
}

void RelatedPapersDialog::onFailed(qint64 publicationId, int problem, const QString &detail)
{
    if (publicationId != m_publicationId)
        return;
    m_asking = false;
    m_refresh->setEnabled(true);
    switch (RelatedProblem(problem)) {
    case RelatedProblem::NoDoi:
        m_status->setText(tr("Related papers are found by DOI, and this publication has none. Look it up online, or add "
                             "its DOI in Edit, then try again."));
        break;
    case RelatedProblem::NotFound:
        m_status->setText(tr("OpenAlex doesn't know this DOI."));
        break;
    case RelatedProblem::RateLimited:
        m_status->setText(tr("OpenAlex's free daily allowance has been used up for now. Try again later. Your other lookups "
                             "aren't affected."));
        break;
    case RelatedProblem::Offline:
        m_status->setText(tr("Lookups are offline, so nothing is asked online. Choose a lookup mode in Settings to find "
                             "related papers."));
        break;
    case RelatedProblem::Unreadable:
        m_status->setText(tr("OpenAlex's answer couldn't be read."));
        break;
    case RelatedProblem::Network:
        m_status->setText(tr("OpenAlex couldn't be reached: %1").arg(detail));
        break;
    }
}

const RelatedWork *RelatedPapersDialog::workFor(QTreeWidgetItem *item) const
{
    if (!item)
        return nullptr;
    const QList<RelatedWork> &works = item->data(0, kRelationRole).toInt() == 0 ? m_papers.references : m_papers.citedBy;
    const int index = item->data(0, kIndexRole).toInt();
    return index >= 0 && index < works.size() ? &works.at(index) : nullptr;
}

QList<QTreeWidgetItem *> RelatedPapersDialog::chosen() const
{
    auto *tree = qobject_cast<QTreeWidget *>(m_tabs->currentWidget());
    return tree ? tree->selectedItems() : QList<QTreeWidgetItem *>();
}

void RelatedPapersDialog::updateButtons()
{
    bool anyMissing = false;
    bool anyHere = false;
    bool anyDoi = false;
    for (QTreeWidgetItem *item : chosen()) {
        if (const RelatedWork *work = workFor(item)) {
            anyMissing = anyMissing || !work->inLibrary;
            anyHere = anyHere || work->inLibrary;
            anyDoi = anyDoi || !work->record.publication.doi.isEmpty();
        }
    }
    m_add->setEnabled(anyMissing);
    m_show->setEnabled(anyHere && chosen().size() == 1);
    m_copy->setEnabled(anyDoi);
    m_open->setEnabled(chosen().size() == 1);
}

void RelatedPapersDialog::addChosen()
{
    int added = 0;
    try {
        for (QTreeWidgetItem *item : chosen()) {
            const RelatedWork *work = workFor(item);
            if (!work || work->inLibrary)
                continue;
            addRelatedToLibrary(m_session.database(), work->record);
            ++added;
        }
    } catch (const std::exception &error) {
        m_status->setText(tr("They couldn't all be added: %1").arg(QString::fromUtf8(error.what())));
    }
    if (added == 0)
        return;
    fill();
    m_status->setText(tr("Added %n publication(s) without a file.", nullptr, added));
    emit libraryChanged();
}

void RelatedPapersDialog::copyDois()
{
    QStringList dois;
    for (QTreeWidgetItem *item : chosen()) {
        if (const RelatedWork *work = workFor(item); work && !work->record.publication.doi.isEmpty())
            dois << work->record.publication.doi;
    }
    if (!dois.isEmpty())
        QApplication::clipboard()->setText(dois.join(u'\n'));
}

void RelatedPapersDialog::openChosen()
{
    const QList<QTreeWidgetItem *> items = chosen();
    const RelatedWork *work = items.size() == 1 ? workFor(items.first()) : nullptr;
    if (!work)
        return;
    // Its DOI's page, or else its page at OpenAlex. Both are built here, not taken from the answer.
    const QUrl url = !work->record.publication.doi.isEmpty()
        ? doiLink(work->record.publication.doi)
        : safeWebLink(QStringLiteral("https://openalex.org/") + work->openAlexId);
    if (url.isValid())
        QDesktopServices::openUrl(url);
}

void RelatedPapersDialog::showChosen()
{
    const QList<QTreeWidgetItem *> items = chosen();
    if (const RelatedWork *work = items.size() == 1 ? workFor(items.first()) : nullptr; work && work->inLibrary)
        emit showRequested(*work->inLibrary);
}

} // namespace papertrail::ui
