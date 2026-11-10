#pragma once

#include "core/query/PublicationFilter.h"
#include "ui/QueryThreads.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>

#include <optional>

namespace papertrail::ui {

// The publications matching the current filter, for both the grid and the list. It only ever holds
// a few pages in memory, and fetches others in the background as the views ask for them.
class PublicationListModel : public QAbstractTableModel {
    Q_OBJECT

public:
    // Columns after ReadColumn came later, so they're hidden until chosen, and a layout saved before them is still used.
    enum Column {
        TitleColumn,
        AuthorsColumn,
        YearColumn,
        ContainerColumn,
        TypeColumn,
        FormatColumn,
        ReadColumn,
        RatingColumn,
        LastOpenedColumn,
        ProgressColumn,
        ColumnCount
    };

    enum Role {
        IsLoadedRole = Qt::UserRole + 1,
        IdRole,
        TitleRole,
        AuthorsRole,
        YearRole,
        CoverShaRole,
        FormatRole,
        FormatsRole, // "PDF, EPUB"
        FileStatusRole,
        NeedsReviewRole,
        // For a journal issue, its numbers and date, such as "Vol. 7, No. 11, Nov 2026", to tell issues apart.
        NumberingRole,
        // "retraction", "partial_retraction" or "withdrawal", for the bar across its cover. Empty if none.
        CoverNoticeRole,
    };

    explicit PublicationListModel(const QString &databasePath, QObject *parent = nullptr);

    // Shows a different set of results, starting from the top.
    void setQuery(const core::PublicationFilter &filter, const core::PublicationSort &sort);

    // Fetches the same results again after the library has changed, and keeps the views where they are.
    void refresh();

    const core::PublicationFilter &filter() const { return m_filter; }
    const core::PublicationSort &sort() const { return m_sort; }

    // Null if that row hasn't been fetched yet.
    std::optional<qint64> idAt(int row) const;

    bool hasResults() const { return m_hasResults; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    // Rows can be dragged, as onto a collection or tag. See dragChosenPublications.
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

signals:
    // A new set of results or a refreshed one has arrived.
    void resultsChanged();

    // Rows have been fetched, including rows that a selection may be waiting to identify.
    void rowsLoaded();

    void queryFailed(const QString &message);

private:
    static constexpr int kPageSize = 100;
    static constexpr int kPagesKept = 40;

    struct Page {
        QList<core::PublicationSummary> rows;
        mutable quint64 lastUsed = 0;
    };

    const core::PublicationSummary *rowAt(int row) const;
    void onQueryReady(const papertrail::ui::ListResult &result);
    void onPageReady(const papertrail::ui::ListResult &result);
    void storePage(qint64 offset, const QList<core::PublicationSummary> &rows);

    mutable ListQueryThread m_thread;
    core::PublicationFilter m_filter;
    core::PublicationSort m_sort;
    quint64 m_generation = 0;
    bool m_resetPending = false; // The next results are a new set, rather than a refresh.
    int m_count = 0;
    bool m_hasResults = false;

    mutable QHash<int, Page> m_pages;
    mutable QSet<int> m_requested;
    mutable quint64 m_useClock = 0;
    mutable int m_lastPageUsed = 0;
};

} // namespace papertrail::ui
