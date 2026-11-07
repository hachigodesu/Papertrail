#include "ui/ReviewTab.h"

#include "core/model/PublicationFields.h"
#include "core/repo/LookupRepository.h"
#include "metadata/Matching.h"
#include "ui/Labels.h"
#include "ui/LibrarySession.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <exception>

namespace papertrail::ui {

using namespace core;
using namespace metadata;

namespace {

QString fieldLabel(const QString &field)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("type"), QObject::tr("Type")},
        {QStringLiteral("title"), QObject::tr("Title")},
        {QStringLiteral("subtitle"), QObject::tr("Subtitle")},
        {QStringLiteral("authors"), QObject::tr("Authors")},
        {QStringLiteral("editors"), QObject::tr("Editors")},
        {QStringLiteral("year"), QObject::tr("Year")},
        {QStringLiteral("month"), QObject::tr("Month")},
        {QStringLiteral("day"), QObject::tr("Day")},
        {QStringLiteral("container_title"), QObject::tr("Journal or source")},
        {QStringLiteral("volume"), QObject::tr("Volume")},
        {QStringLiteral("issue"), QObject::tr("Issue")},
        {QStringLiteral("pages"), QObject::tr("Pages")},
        {QStringLiteral("publisher"), QObject::tr("Publisher")},
        {QStringLiteral("place"), QObject::tr("Place")},
        {QStringLiteral("doi"), QObject::tr("DOI")},
        {QStringLiteral("isbn13"), QObject::tr("ISBN-13")},
        {QStringLiteral("isbn10"), QObject::tr("ISBN-10")},
        {QStringLiteral("issn"), QObject::tr("ISSN")},
        {QStringLiteral("eissn"), QObject::tr("eISSN")},
        {QStringLiteral("arxiv_id"), QObject::tr("arXiv ID")},
        {QStringLiteral("pmid"), QObject::tr("PubMed ID")},
        {QStringLiteral("language"), QObject::tr("Language")},
        {QStringLiteral("abstract"), QObject::tr("Abstract")},
        {QStringLiteral("conference_name"), QObject::tr("Conference")},
        {QStringLiteral("conference_location"), QObject::tr("Conference location")},
        {QStringLiteral("institution"), QObject::tr("Institution")},
        {QStringLiteral("url"), QObject::tr("Web address")},
    };
    return labels.value(field, field);
}

QString shown(const QString &field, const QString &value)
{
    return field == QLatin1StringView("type") ? typeLabelFromDb(value) : value;
}

} // namespace

ReviewTab::ReviewTab(LibrarySession &session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter);

    m_list = new QListWidget(splitter);
    m_list->setAccessibleName(tr("Publications to review"));
    m_list->setTextElideMode(Qt::ElideRight);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { showPublication(); });
    splitter->addWidget(m_list);

    auto *right = new QWidget(splitter);
    auto *rightColumn = new QVBoxLayout(right);

    m_empty = new QLabel(tr("Nothing is waiting for review."), right);
    m_empty->setAlignment(Qt::AlignCenter);
    rightColumn->addWidget(m_empty);

    m_detail = new QWidget(right);
    auto *column = new QVBoxLayout(m_detail);
    column->setContentsMargins(0, 0, 0, 0);

    m_heading = new QLabel(m_detail);
    m_heading->setTextFormat(Qt::PlainText);
    m_heading->setWordWrap(true);
    QFont bold = m_heading->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.15);
    m_heading->setFont(bold);
    column->addWidget(m_heading);

    m_candidates = new QComboBox(m_detail);
    m_candidates->setAccessibleName(tr("Possible matches"));
    connect(m_candidates, &QComboBox::currentIndexChanged, this, [this] { showCandidate(); });
    column->addWidget(m_candidates);

    m_matchReason = new QLabel(m_detail);
    m_matchReason->setTextFormat(Qt::PlainText);
    m_matchReason->setWordWrap(true);
    column->addWidget(m_matchReason);

    m_comparison = new QTableWidget(m_detail);
    m_comparison->setColumnCount(3);
    m_comparison->setHorizontalHeaderLabels({tr("Field"), tr("In your library"), tr("Found online")});
    m_comparison->verticalHeader()->hide();
    m_comparison->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_comparison->setSelectionMode(QAbstractItemView::NoSelection);
    m_comparison->setWordWrap(true);
    m_comparison->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_comparison->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_comparison->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_comparison->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); // An abstract can be a tall row.
    m_comparison->setAccessibleName(tr("What would change"));
    // Text wraps differently at each width, including the first time this is shown.
    connect(m_comparison->horizontalHeader(), &QHeaderView::sectionResized, m_comparison,
            [this] { m_comparison->resizeRowsToContents(); }, Qt::QueuedConnection);
    column->addWidget(m_comparison, 1);

    auto *buttons = new QHBoxLayout;
    m_accept = new QPushButton(tr("Use these details"), m_detail);
    m_reject = new QPushButton(tr("Not a match"), m_detail);
    m_open = new QPushButton(tr("Open the file"), m_detail);
    connect(m_accept, &QPushButton::clicked, this, &ReviewTab::acceptCurrent);
    connect(m_reject, &QPushButton::clicked, this, &ReviewTab::rejectCurrent);
    connect(m_open, &QPushButton::clicked, this, [this] { emit openRequested(currentPublication()); });
    buttons->addWidget(m_accept);
    buttons->addWidget(m_reject);
    buttons->addStretch();
    buttons->addWidget(m_open);
    column->addLayout(buttons);
    rightColumn->addWidget(m_detail, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({320, 900});

    reload();
}

void ReviewTab::reload()
{
    const qint64 previous = currentPublication();
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    int select = 0;
    try {
        LookupRepository lookups(m_session.database());
        for (const qint64 id : lookups.publicationsWaitingForReview()) {
            const std::optional<Publication> publication = m_session.publications().get(id);
            if (!publication)
                continue;
            if (id == previous)
                select = m_list->count();
            auto *item = new QListWidgetItem(publication->title, m_list);
            item->setData(Qt::UserRole, id);
            item->setToolTip(publication->title);
        }
    } catch (const std::exception &) {
        // Shows as an empty list, and the next change tries again.
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(qMin(select, m_list->count() - 1));
    showPublication();
}

qint64 ReviewTab::currentPublication() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

void ReviewTab::showPublication()
{
    const qint64 id = currentPublication();
    m_detail->setVisible(id != 0);
    m_empty->setVisible(id == 0);
    if (id == 0)
        return;

    const std::optional<Publication> publication = m_session.publications().get(id);
    m_heading->setText(publication ? publication->title : QString());

    const QSignalBlocker blocker(m_candidates);
    m_candidates->clear();
    for (const LookupCandidate &candidate : LookupRepository(m_session.database()).candidatesFor(id)) {
        const FoundRecord record = FoundRecord::fromJson(QJsonDocument::fromJson(candidate.recordJson.toUtf8()).object());
        m_candidates->addItem(tr("%1 (%2, %3% match)")
                                  .arg(record.publication.title, providerLabel(candidate.provider))
                                  .arg(qRound(candidate.score * 100)),
                              candidate.recordJson);
    }
    m_candidates->setVisible(m_candidates->count() > 1);
    showCandidate();
}

void ReviewTab::showCandidate()
{
    m_comparison->setRowCount(0);
    const qint64 id = currentPublication();
    if (id == 0 || m_candidates->count() == 0)
        return;
    const std::optional<Publication> publication = m_session.publications().get(id);
    if (!publication)
        return;
    const FoundRecord record = FoundRecord::fromJson(QJsonDocument::fromJson(m_candidates->currentData().toString().toUtf8()).object());

    if (record.matchedBy == QLatin1StringView("title")) {
        m_matchReason->setText(tr("%1 found this by searching for the title.").arg(providerLabel(record.provider)));
    } else {
        m_matchReason->setText(tr("%1 found this from the identifier in your file, but the title doesn't agree. "
                                  "The file may mention another work's identifier.")
                                   .arg(providerLabel(record.provider)));
    }

    const QList<FieldChange> changes = plannedChanges(*publication, record, fieldsSetByHand(m_session.database(), id));
    QFont changed = m_comparison->font();
    changed.setBold(true);
    // Authors are much of what tells two works apart, so they go near the top and not at the end.
    QStringList fields = metadataFieldNames();
    fields.removeOne(QStringLiteral("authors"));
    fields.insert(fields.indexOf(QStringLiteral("subtitle")) + 1, QStringLiteral("authors"));
    for (const QString &field : std::as_const(fields)) {
        const QString now = metadataFieldValue(*publication, field);
        const auto change = std::find_if(changes.cbegin(), changes.cend(), [&](const FieldChange &c) { return c.field == field; });
        const bool changes_ = change != changes.cend();
        const QString found = changes_ ? change->after : metadataFieldValue(record.publication, field);
        // Everything the source gave, plus the type. What's only in the library stays out of the way.
        if (found.isEmpty() || (field == QLatin1StringView("type") && !changes_))
            continue;
        const int row = m_comparison->rowCount();
        m_comparison->insertRow(row);
        m_comparison->setItem(row, 0, new QTableWidgetItem(fieldLabel(field)));
        // Table items show text as it is, so what comes from a source can't turn into formatting.
        m_comparison->setItem(row, 1, new QTableWidgetItem(shown(field, now)));
        auto *foundItem = new QTableWidgetItem(shown(field, found));
        if (changes_)
            foundItem->setFont(changed);
        m_comparison->setItem(row, 2, foundItem);
    }
    m_comparison->resizeRowsToContents();
}

void ReviewTab::moveOn(int row)
{
    reload();
    if (m_list->count() > 0)
        m_list->setCurrentRow(qMin(row, m_list->count() - 1));
    emit libraryEdited();
}

void ReviewTab::acceptCurrent()
{
    const qint64 id = currentPublication();
    if (id == 0 || m_candidates->count() == 0)
        return;
    const int row = m_list->currentRow();
    try {
        const FoundRecord record = FoundRecord::fromJson(QJsonDocument::fromJson(m_candidates->currentData().toString().toUtf8()).object());
        Transaction transaction(m_session.database());
        applyFoundRecord(m_session.database(), id, record, false);
        LookupRepository(m_session.database()).resolveCandidates(id);
        transaction.commit();
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Review"), tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    moveOn(row);
}

void ReviewTab::rejectCurrent()
{
    const qint64 id = currentPublication();
    if (id == 0)
        return;
    const int row = m_list->currentRow();
    try {
        LookupRepository(m_session.database()).resolveCandidates(id);
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Review"), tr("That couldn't be saved: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    moveOn(row);
}

} // namespace papertrail::ui
