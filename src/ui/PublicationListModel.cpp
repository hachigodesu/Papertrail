#include "ui/PublicationListModel.h"

#include "ui/Labels.h"

#include <QDateTime>
#include <QLocale>

namespace papertrail::ui {

using namespace core;

PublicationListModel::PublicationListModel(const QString &databasePath, QObject *parent)
    : QAbstractTableModel(parent), m_thread(databasePath)
{
    connect(&m_thread, &ListQueryThread::queryReady, this, &PublicationListModel::onQueryReady);
    connect(&m_thread, &ListQueryThread::pageReady, this, &PublicationListModel::onPageReady);
    m_thread.start();
}

void PublicationListModel::setQuery(const PublicationFilter &filter, const PublicationSort &sort)
{
    m_filter = filter;
    m_sort = sort;
    ++m_generation;
    m_resetPending = true;
    m_lastPageUsed = 0;
    m_thread.setQuery(m_generation, m_filter, m_sort, 0, kPageSize);
}

void PublicationListModel::refresh()
{
    ++m_generation;
    m_thread.setQuery(m_generation, m_filter, m_sort, qint64(m_lastPageUsed) * kPageSize, kPageSize);
}

void PublicationListModel::onQueryReady(const ListResult &result)
{
    if (result.generation != m_generation)
        return;
    if (!result.error.isEmpty()) {
        emit queryFailed(result.error);
        return;
    }

    const int newCount = int(result.count);
    if (m_resetPending) {
        m_resetPending = false;
        beginResetModel();
        m_pages.clear();
        m_requested.clear();
        m_count = newCount;
        m_lastPageUsed = 0;
        storePage(result.offset, result.rows);
        endResetModel();
    } else {
        m_pages.clear();
        m_requested.clear();
        if (newCount > m_count) {
            beginInsertRows(QModelIndex(), m_count, newCount - 1);
            m_count = newCount;
            storePage(result.offset, result.rows);
            endInsertRows();
        } else if (newCount < m_count) {
            beginRemoveRows(QModelIndex(), newCount, m_count - 1);
            m_count = newCount;
            storePage(result.offset, result.rows);
            endRemoveRows();
        } else {
            storePage(result.offset, result.rows);
        }
        if (m_count > 0)
            emit dataChanged(index(0, 0), index(m_count - 1, ColumnCount - 1));
    }
    m_hasResults = true;
    emit resultsChanged();
    emit rowsLoaded();
}

void PublicationListModel::onPageReady(const ListResult &result)
{
    if (result.generation != m_generation)
        return;
    if (!result.error.isEmpty()) {
        m_requested.remove(int(result.offset / kPageSize));
        emit queryFailed(result.error);
        return;
    }

    storePage(result.offset, result.rows);
    if (!result.rows.isEmpty()) {
        const int first = int(result.offset);
        const int last = qMin(first + int(result.rows.size()), m_count) - 1;
        if (first <= last)
            emit dataChanged(index(first, 0), index(last, ColumnCount - 1));
    }
    emit rowsLoaded();
}

void PublicationListModel::storePage(qint64 offset, const QList<PublicationSummary> &rows)
{
    const int pageIndex = int(offset / kPageSize);
    m_requested.remove(pageIndex);
    Page page;
    page.rows = rows;
    page.lastUsed = ++m_useClock;
    m_pages.insert(pageIndex, page);

    while (m_pages.size() > kPagesKept) {
        auto oldest = m_pages.begin();
        for (auto it = m_pages.begin(); it != m_pages.end(); ++it) {
            if (it->lastUsed < oldest->lastUsed)
                oldest = it;
        }
        m_pages.erase(oldest);
    }
}

const PublicationSummary *PublicationListModel::rowAt(int row) const
{
    if (row < 0 || row >= m_count)
        return nullptr;
    const int pageIndex = row / kPageSize;
    const auto page = m_pages.constFind(pageIndex);
    if (page == m_pages.constEnd()) {
        if (!m_requested.contains(pageIndex)) {
            m_requested.insert(pageIndex);
            m_thread.requestPage(m_generation, qint64(pageIndex) * kPageSize, kPageSize);
        }
        return nullptr;
    }
    page->lastUsed = ++m_useClock;
    m_lastPageUsed = pageIndex;
    const int inPage = row % kPageSize;
    // A page fetched before the library shrank can be short.
    return inPage < page->rows.size() ? &page->rows.at(inPage) : nullptr;
}

std::optional<qint64> PublicationListModel::idAt(int row) const
{
    const PublicationSummary *summary = rowAt(row);
    return summary ? std::optional<qint64>(summary->id) : std::nullopt;
}

int PublicationListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_count;
}

int PublicationListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

Qt::ItemFlags PublicationListModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.isValid())
        flags |= Qt::ItemIsDragEnabled;
    return flags;
}

QVariant PublicationListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const PublicationSummary *row = rowAt(index.row());
    if (role == IsLoadedRole)
        return row != nullptr;
    if (!row)
        return {};

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case TitleColumn: return row->title;
        case AuthorsColumn: return row->authors;
        case YearColumn: return row->year ? QVariant(*row->year) : QVariant();
        case ContainerColumn: {
            // Issues of one journal share its name, so their numbers tell them apart.
            if (row->type != PublicationType::JournalIssue)
                return row->containerTitle;
            const QString numbers = issueNumbering(row->volume, row->issue, row->month, std::nullopt);
            if (numbers.isEmpty())
                return row->containerTitle;
            return row->containerTitle.isEmpty() ? numbers : row->containerTitle + QStringLiteral(", ") + numbers;
        }
        case TypeColumn: return typeLabel(row->type);
        case FormatColumn: return row->formats.join(QStringLiteral(", ")).toUpper();
        case ReadColumn: return readStatusLabel(row->readStatus);
        case RatingColumn: return ratingStars(row->rating.value_or(0));
        case LastOpenedColumn: {
            if (row->lastOpened <= 0)
                return {};
            const QDateTime opened = QDateTime::fromMSecsSinceEpoch(row->lastOpened);
            // Today's by the time, anything older by the day.
            if (opened.date() == QDate::currentDate())
                return QLocale().toString(opened.time(), QLocale::ShortFormat);
            return QLocale().toString(opened.date(), QLocale::ShortFormat);
        }
        case ProgressColumn:
            return row->progress ? tr("%1%").arg(qRound(*row->progress * 100)) : QVariant();
        }
        return {};
    case Qt::TextAlignmentRole:
        if (index.column() == ProgressColumn || index.column() == LastOpenedColumn)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    case Qt::ToolTipRole: {
        if (index.column() != TitleColumn)
            return {};
        QString text = row->title;
        if (const QString notice = coverNoticeLabel(row->coverNotice); !notice.isEmpty())
            text += u'\n' + notice;
        if (row->needsReview)
            text += u'\n' + tr("Needs checking");
        const QString problem = fileStatusLabel(row->fileStatus);
        if (!problem.isEmpty())
            text += u'\n' + problem;
        if (row->formats.size() > 1)
            text += u'\n' + tr("Available as %1").arg(row->formats.join(QStringLiteral(", ")).toUpper());
        return text;
    }
    case Qt::AccessibleTextRole: {
        if (index.column() == RatingColumn)
            return ratingLabel(row->rating.value_or(0));
        QString text = row->title;
        if (!row->authors.isEmpty())
            text += QStringLiteral(", ") + row->authors;
        if (row->year)
            text += QStringLiteral(", ") + QString::number(*row->year);
        return text;
    }
    case IdRole: return row->id;
    case TitleRole: return row->title;
    case AuthorsRole: return row->authors;
    case YearRole: return row->year ? QVariant(*row->year) : QVariant();
    case CoverShaRole: return row->coverSha;
    case FormatRole: return row->format;
    case FormatsRole: return row->formats.join(QStringLiteral(", ")).toUpper();
    case FileStatusRole: return row->fileStatus;
    case NeedsReviewRole: return row->needsReview;
    case CoverNoticeRole: return row->coverNotice;
    case NumberingRole:
        return row->type == PublicationType::JournalIssue ? issueNumbering(row->volume, row->issue, row->month, row->year)
                                                          : QString();
    default: return {};
    }
}

QVariant PublicationListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case TitleColumn: return tr("Title");
    case AuthorsColumn: return tr("Authors");
    case YearColumn: return tr("Year");
    case ContainerColumn: return tr("Journal or source");
    case TypeColumn: return tr("Type");
    case FormatColumn: return tr("Format");
    case ReadColumn: return tr("Status");
    case RatingColumn: return tr("Rating");
    case LastOpenedColumn: return tr("Last opened");
    case ProgressColumn: return tr("Progress");
    }
    return {};
}

} // namespace papertrail::ui
